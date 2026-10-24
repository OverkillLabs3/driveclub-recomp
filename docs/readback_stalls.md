# Guest readback stalls (4K DLAA frame-time bursts)

What this is: at 4K output with **DLAA** (`BB_AA=dlss`, `BB_AA_SCALE=native`) DriveClub's frame times
alternate instead of holding one value — the presentation graph shows a thick line or bursts, and the
game feels unsteady. At 4K with **DLSS Quality** the same scene is flat. This note records what was
measured, what was tried, and what is left to do, so the work does not have to be rediscovered.

All numbers below are from `BB_BENCHMARK` runs on the reference machine (RTX 5070 Ti, Ryzen 7 7700X,
120 Hz display), with the harness described at the end.

## Summary of the 2026-10-24 session, read this first

**What shipped: a bound on the submission lead, not on the frame rate** (`BB_LEAD_PERCENT`, default
85). The guest's readbacks wait for GPU work that was submitted before them, so the old whole-frame
bound (`BB_FRAMES_AHEAD=1`) let a read wait for up to a frame of queued work: that is the 9.5 / 22 ms
beat. The presenter now waits until the GPU has completed 85 % of the frame being submitted — the
guest reads its GPU results late in the frame, so at 85 % the write a read needs is already done —
instead of waiting for all of the previous frame. The GPU stays as busy as before and the queue in
front of a read is nearly gone. Measured at 4K DLAA, 20 s runs, same build, alternating:

| setting | FPS | 1 % low | p99 | max | frame-time sd |
|---|---|---|---|---|---|
| `BB_LEAD_PERCENT=0` (old), 3 runs | 63.4 / 64.6 / 64.1 | 42.7 / 41.9 / 38.6 | 23.0 / 22.7 / 23.9 | 24.1 / 31.0 / 37.3 | 4.44 / 3.36 / 3.60 |
| **`BB_LEAD_PERCENT=85` (new default), 3 runs** | **62.6 / 62.6 / 62.8** | **48.5 / 48.5 / 46.6** | **19.6 / 20.0 / 19.6** | **24.9 / 22.6 / 33.3** | **1.91 / 1.85 / 2.09** |

Averages: **-2.1 % FPS, 1 % low +17 %, p99 -15 %, frame-time sd -49 %**. The 9.5 / 22 ms alternation
becomes 14.4 / 17.8 ms (a 3.3 ms breath instead of a 12.5 ms one). Every run of the new setting is
better than every run of the old one on the 1 % low, the p99 and the sd. The 0.1 % low is dominated
by one or two isolated spikes per run (a 33 ms frame in the 30 s run, a 37 ms one in the old
setting's 30 s run), so it moves with the run rather than with the setting. No frame-rate target is
involved: the rate is still whatever the GPU produces.

The burst is **gone in practice**, not mathematically: the 3.3 ms alternation that remains is the
read that still waits a little plus the display's quantisation. "What the stall is (and what it is
not)" has the mechanism; "The fix" has the tuning curve.

**What was tried and cannot be the fix:** host-visible readback windows (`BB_READBACK_WINDOW_HOSTVISIBLE`,
four variants, all slower), a copy queue that waits only on the write's submission
(`BB_READBACK_COPY_QUEUE`, built this session: the wait it removes is not the one that costs — the
4.5 ms is the GPU reaching the write, before the copy), a submit-depth bound (deadlocks), the
two-thread draw pipe off (flat but -9 % FPS), and less GPU work (no effect). Each is recorded below
with its numbers.

## Corrected measurements (2026-10-24), read this first

Two different problems were being mixed up:

1. **A 140 ms stall every 5.0 s is `BB_FRAME_STATS` itself**, not the game. Every stats run shows
   spikes at 8.3 / 13.3 / 18.3 / 23.4 s (exactly 5 s apart, 140-157 ms each); runs with only
   `BB_FRAME_LOG` or `BB_FRAME_TRACE` never have a frame over 30 ms. The 6.9 FPS 0.1 % low that the
   earlier numbers in this file quote is mostly that artifact. Never measure a burst with
   `BB_FRAME_STATS` on.

2. **The real 4K DLAA pattern is a 2-frame beat**, not 150 ms spikes: 22 ms then 9.5 ms, over and
   over (e.g. 21.2 10.4 22.9 9.7 21.6 10.3 ...). Baseline without instrumentation:
   64.7 FPS, 1 % low 43.0, 0.1 % low 41.2, p99 22.8 ms, max 24.3 ms. With DLSS Quality the same
   scene is 102 FPS, 1 % low 68.1, max 16.4 ms.

Measured with a frame trace plus a readback log in the same run (`corr_dlaa`):

* Slow frames (22.9 ms): 2.52 readbacks, **18.3 ms of readback thread time**.
* Fast frames (12.0 ms): 0.98 readbacks, 6.4 ms.
* Correlation between the frame interval and the readback time: **+0.87**.

So the readback wait is what makes the long frame long. Where it goes (`BB_READBACK_LOG`, median /
p90 per call): pickup 0.12 / 7.8 ms (waiting for the GPU command thread), data 0 / 0 ms (the writing
submission), copy 0.6 / 9.6 ms (waiting for the copy's submission).

### Why prefetching and coalescing cannot remove it

* Disabling the early (prefetch) copies: 63.9 FPS, 1 % low 41.8 — no change. The prefetched copy is
  itself a GPU copy of the same submission, so it cannot finish earlier than the read needs it.
* One arena-wide copy instead of one per window (`BB_READBACK_OFFLOAD_ARENA=1`): 49 FPS with 3.1 s
  stalls, i.e. still unstable, and the earlier run that did work showed the same total wait.
* Flushing the frame every 150 draws (`BB_FLUSH_DRAWS`): the copy wait stays at p90 9.4 ms. The wait
  is not for the current batch's tail; it is for work that was submitted before the fault.
* `BB_FRAMES_AHEAD` 0/1/2, a deeper swapchain (`BB_SWAPCHAIN_IMAGES`), Mailbox and Immediate present
  modes: none change the beat.
* GPU utilisation during the measured phase of a 4K DLAA run: **avg 92 %, p90 95 %, max 97 %**
  (`nvidia-smi --query-gpu=utilization.gpu --loop-ms=200`). The GPU is nearly saturated; the beat is
  the CPU/GPU pipeline phase, not spare GPU time.

A copy queue was tried too (a second queue of the graphics family, which the device exposes 16 of).
It deadlocks if it waits on the write's tick, because `StampGpuWrite` records the **open** tick: the
submission that wrote the data has not been submitted yet, and only the blocked guest can submit it.
Waiting only when the GPU had already passed that tick made it almost never fire, and the measured
readback timings did not move. That path was removed again.

The correct fix is therefore one of:
* a non-sparse, host-visible arena (today the driver allows only memory type 1 for a sparse buffer,
  so direct reads are impossible — see `BB_ARENA_PROBE`), or
* a copy queue with a submission boundary recorded after the write, so the copy waits for a tick that
  covers the write only (a scheduler change, not a buffer-cache change).

**What the guest is reading (Ghidra, `eboot.elf`, guest address `0x758980`).** The faulting load is
`*(uint *)(ctx + 0x28 + index * 0x30)` inside a small lookup: a 0x30-byte-stride descriptor table
holding a value at `+0x20` (returned through `param_3`) and a count at `+0x28`, clamped to
`ctx + 0xc` and returned through `param_4`. Callers: `FUN_00574d30`, `FUN_00601df0`,
`FUN_00702750` (three call sites), `FUN_005ea670`, `FUN_01051f00`. It is a real GPU-written
descriptor the CPU needs to size the next pass, not a fence or a label — there is nothing to
special-case away.

**Pacing was tried four ways and is a dead end under "uncapped".** The goal was to present the same
63.9 FPS average evenly instead of 22/9.5. Variants, all at 4K DLAA:

| target source | result |
|---|---|
| median of release intervals | locked at the slow frame's 22.8 ms → 43.7 FPS, sd 0.17 |
| mean of release intervals | drifted up → 30 FPS |
| mean of GPU-thread flip times | drifted up → 27 FPS |
| mean of guest flip times (+ deeper frames-ahead) | drifted up → 27 FPS |

The cadence is perfectly even in every case, so the mechanism works; the rate collapses because the
pipeline is only one frame deep (`BB_FRAMES_AHEAD=1`, 3 swapchain images). Any delay the pacer adds
stops the guest from submitting the next flip, that delay enters the next "production" sample, and
the target rises without bound. Breaking the loop needs a target that the pacing cannot influence,
i.e. one frozen after a calibration window — a self-calibrated cap. That is the cap this work is
required to avoid, so the pacer was removed rather than shipped with a hidden cap.

**The allocator change was tried and is a dead end (2026-10-24).** `BB_ARENA_HOSTVISIBLE=1` backed
the arenas with non-sparse, device-local, host-visible memory (the GPU exposes memory type 4 in the
16 GB local heap) in 64 MiB pages, and filled the existing direct-read path from the arena mapping
instead of the sparse-residency list. Results at 4K DLAA:

| build | FPS | 1 % low | notes |
|---|---|---|---|
| sparse arena (shipped) | 63.9 | 42.8 | baseline |
| host-visible arena, copy path | 54.8 | 36.6 | 9 FPS slower, **vertex explosions** |
| host-visible arena, direct reads | 54.9 | 38.0 | same speed |

Two independent reasons it cannot be the fix. First, the placement itself costs ~9 FPS: the same
run with the copy path (`BB_READBACK_DIRECT=0`) is 54.8, so it is not the direct reads. Second, the
shaders resolve guest addresses through `bda_pagetable_buffer`, which is filled from the sparse
arena's bindings; with a fully resident arena those pages are never registered, so vertex buffers
resolve to the wrong address — hence the vertex explosions. Fixing that would still leave the
9 FPS loss. Reverted.

### Host-visible backing for the readback windows only (four variants, all slower)

The narrower version of the same idea: leave the arena sparse and device-local, and move only the
few 512 KiB windows the CPU reads every frame into non-sparse, device-local, host-visible memory
(`BB_READBACK_WINDOW_HOSTVISIBLE`, memory type 4, sparse-bound per window; the driver accepts a
sparse bind to the CPU-visible type of the same heap, probed with `vkprobe_type4.exe`). A guest read
would then be served from the mapping on the faulting thread. Every stage of it was built and
measured at 4K DLAA, 10 s runs:

| variant | FPS | 1 % low | p99 | max | sd | why it failed |
|---|---|---|---|---|---|---|
| sparse arena (baseline) | 64.16 | 40.76 | 23.90 | 25.34 | 4.70 | — |
| page-sized delivery on the faulting thread | 59.84 | 14.46 | 37.97 | 113.63 | 6.15 | one wait per 4 KiB page instead of per window: 530 reads/s instead of 288, and 10 % of reads still fell back to the copy path |
| window-wide delivery, one copy per read | 55.55 | 6.09 | 101.04 | 256.73 | 6.05 | the write list kept every binding since the last read (390 overlapping entries); a read then copied 18 MB out of the mapping |
| + overlapping writes merged | 61.62 | 35.57 | 27.72 | 28.84 | 4.88 | reads of the mapping run at ~70 MB/s (18 MB took 263 ms, fixed by the merge; 512 KiB still costs ~7 ms) |
| + only windows the GPU has finished | 61.64 | 34.91 | 28.17 | 29.41 | 5.34 | only 24 % of reads found the GPU past the window's writes; the other 76 % went to the copy path anyway |

Reading device memory through the mapping is the first problem: `vkprobe_type4.exe` shows the data
is there and coherent (a GPU fill of a type-4 allocation is visible through the mapping after a
fence wait), but a `memcpy` out of it runs at ~70 MB/s — each 4 KiB page is a separate PCIe
transaction. A 512 KiB window therefore costs ~7 ms to read, as much as the GPU copy it replaces.

The second, and fatal, problem: the CPU can only read a window once the GPU has finished *writing*
it, and the GPU runs a frame behind. Of 1024 guest reads in a 10 s DLAA run, only 245 found the
GPU already past the window's writes; the others waited for exactly the work the copy path waits
for. The mapping removes the copy and the command-thread round trip, not the wait, and the
placement costs ~2.5 FPS because the GPU reads and writes those windows through BAR memory.

The window code is still in the build (`BB_READBACK_WINDOW_HOSTVISIBLE=1`, DLAA only) for the
record and for memory types where CPU reads are not that slow; it is off by default. Two bugs found
on the way and fixed in it: the direct path resolved its spans through the sparse-residency list,
which does not always describe the promoted chunk (a window's span pointed one 512 KiB window too
far), and overlapping writes were queued and copied repeatedly.

## What the stall is made of

`BB_READBACK_LOG` (one line per guest read that downloads GPU-written data), 4K DLAA, 12 s:

```
3271 readbacks = 4.5 per frame
  data_us   0.6 ms total   waiting for the submission that wrote the data: negligible, the data is done
  copy_us   8.05 s total   the copy itself, 2.5 ms each
  pickup_us 8.73 s total   waiting for the GPU command thread to take the job, 2.7 ms each
```

The per-call cost does not depend on size at these sizes: 152 bytes and 162 KB both cost 1.4-3.3 ms.
It is GPU queue turnaround plus command-thread availability, not bandwidth.

## Why the emulator cannot remove it

`page_manager.cpp` serves a read fault from the CPU backing store only for bytes the GPU did not write.
At 4K DLAA every remaining declined fault is reported as `reason 8` (`Read faults declined by
instruction: vmovdqu x53/s (reason 8) ...`), i.e. the loaded range contains bytes the GPU wrote, so the
data has to come from the GPU. The emulator gaps that *could* be closed were closed:

- 256-bit VEX loads (`vmovdqu ymm, [mem]`) — the largest single class when this started.
- 128-bit vector ALU with a memory source (`vblendps` and friends) — `vblendps` was ~30 faults/s.

## Where the command-thread time goes (`BB_FRAME_STATS`, drains by reason)

```
drains by reason/s: commands=92 (0.9%)   compute=6021 (16.6%)   submission end=41 (2.7%)
stage A waits over 0.5%: ProcessCompute:1501 16.6%   Process:254 2.7%   ProcessCommands:160 0.9%
```

The readback's own drain is 0.9 %. The 2.7 ms pickup is the command thread being **unavailable**: it
spends ~17 % of its time draining stage B for compute submissions (`ProcessCompute`), so a readback
queued behind one waits. Disabling `PipelinedDispatch` only relabels it (`rasterizer=4256/s 18.1%`,
FPS 58.0 -> 57.5), so it is inherent to recording compute on that thread.

## Approaches tried, with results (all at 4K DLAA)

| change | result |
|---|---|
| one copy per frame instead of 4-5 (`BB_READBACK_OFFLOAD_ARENA=1`) | faults 295 -> 129/s, total wait 593 -> 631 ms/s, sd unchanged; **kept as an option, off by default** |
| that job's guarded unmark fixed (wide jobs unmark what they copied) | removed a hang: the guest re-faulted on the same instruction forever. **in the build** |
| early-copy prefetch of neighbouring windows (`BB_READBACK_PREFETCH`) | copies made 236 -> 360/s, used stayed ~85/s, faults unchanged (lists are rewritten every frame) |
| early copies off (`BB_READBACK_EARLY=0`) | identical (60.1 FPS, 644 ms/s) |
| readback window 512 KB -> 4 MB | faults 305 -> 254/s, wait and FPS unchanged |
| synchronous download (`BB_READBACK_OFFLOAD=0`) | faults 305 -> 131/s but FPS 60 -> 54 (blocks the command thread) |
| host-visible arena for direct guest reads | refused: the sparse buffer's memory requirements list only memory type 1 (`arena memory type bits 0x2 allow types: 1`). A sparse *bind* to the CPU-visible type 4 of the same heap is accepted (probed later with `vkprobe_type4.exe`), which is what the window experiment below uses |
| static mirror / object motion off (less GPU work) | wait 630 -> 667 ms/s, sd unchanged |
| two-thread draw pipe off | FPS 64.2 -> 55.6, frame-time sd 3.63 -> 1.81 ms: flatter but slower |

## What the stall is (and what it is not), 2026-10-24 session

The session's measurements moved the diagnosis from "the readback copy is slow" to "the read waits
for GPU work queued ahead of the write it needs".

* **The copy and the pickup are not the problem, the GPU backlog is.** With reads served from a
  host-visible mapping (no copy, no command-thread round trip) the per-read wait was still 5.0 ms:
  the same GPU catch-up the copy path pays. `BB_READBACK_LOG` shows `stamped_tick` 2-3 submissions
  behind `open_tick` and the GPU 4 (median) to 16 (p90) submissions behind `open_tick`: a read waits
  for the frame's earlier graphics to finish on the GPU.
* **Only a quarter of reads are for data the GPU already produced.** In the 10 s "completed-only"
  run, 245 of 1024 reads found `KnownGpuTick() >= the window's last write tick`; the other 779 had
  to wait.
* **Reducing GPU work does not flatten it.** `BB_MIRROR_RATE=4` (a quarter of the mirror work):
  65.3 FPS mean, sd 4.67 ms — the same beat. The alternation is a CPU/GPU phase effect at the
  readback, not a pure GPU-time shortage.
* **The GPU command thread (not the GPU) idles during the slow frame.** `BB_FRAME_TRACE` on slow
  frames: `gpu_idle_ms` 15.5 (the command thread has nothing to submit while the guest thread is
  blocked), `rf_thread_max_ms` 22.7, `submit_done_to_flip_ms` 0.3; fast frames: 0.0 / 0.9 / 9.7. The
  guest stall starves the command thread, so nothing new is recorded while the GPU finishes the
  queue.
* **Reading device memory from the CPU is slow.** ~70 MB/s through the mapping (18 MB took 263 ms;
  512 KiB ~7 ms), so a mapping read only pays for small dirty ranges.

### The fix: bound the submission lead in ticks (`BB_LEAD_PERCENT`)

`BB_FRAMES_AHEAD=1` bounds the lead in whole frames: at the flip of frame N the presenter waits for
frame N-1 to finish. The guest can still be a whole frame of *submissions* ahead of the GPU when it
faults, so a read can wait for up to a frame of queued work. `BB_LEAD_PERCENT` makes the wait target
a fraction of the frame being submitted instead of the end of the previous one:

```cpp
// vk_presenter.cpp, PrepareFrame, after the frame's flush
u64 tick = recent_frame_ticks.front();          // previous frame's ready tick
if (lead_percent) tick += (frame->ready_tick - tick) * lead_percent / 100;
draw_scheduler.Wait(tick, 3);
```

The wait happens where the guest would wait for the display anyway, so it does not add to the frame
time as long as the GPU stays busy; the queue in front of a read shrinks by the same fraction.

Tuning at 4K DLAA (10 s runs, same build):

| `BB_LEAD_PERCENT` | FPS | 1 % low | p99 | sd | beat in the frame log |
|---|---|---|---|---|---|
| 0 (old) | 64.16 | 40.76 | 23.90 | 4.70 | 9.4 / 22.0 |
| 30 | 62.78 | 42.23 | 23.07 | 3.95 | 10.5 / 24.0 |
| 50 | 62.83 | 46.27 | 21.48 | 3.54 | 11.8 / 19.5 |
| 70 | 62.09 | 45.67 | 21.54 | 3.03 | 11.9 / 19.6 |
| 80 | 63.8 | — | — | 2.20 | 13.2 / 18.2 |
| **85 (default)** | **63.00** | **45.79** | **19.46** | **1.83** | **14.5 / 17.2** |
| 92 | 63.19 | 41.36 | 22.06 | 2.67 | 14.6 / 17.2, min 8.5 |

The curve has a knee at ~85 %: that is where the GPU has completed the part of the frame that the
reads need (the guest reads its GPU results late in the frame), so by the time the next read comes
the write is already done. Beyond it (92 %) the waits overshoot and short frames appear (min 8.5 ms);
below it the beat stays at 11.8 / 19.5. At 85 % the alternation becomes 14.5 / 17.2 — a 2.6 ms
breathing instead of a 12.5 ms one.

Longer runs at 85 % (the default) confirm the 10 s curve: three runs of 20 / 20 / 30 s give 62.6-62.8
FPS, 1 % low 46.6-48.5, p99 19.6-20.0 ms and sd 1.85-2.09 ms, against 63.4-64.6 FPS, 38.6-42.7,
22.7-23.9 and 3.36-4.44 for the old setting. `BB_LEAD_PERCENT=0` restores the old behaviour; above
~90 the wait outlasts the read and short frames appear (min 8.5 ms at 92).

## Other attempts in the 2026-10-24 session

| change | result |
|---|---|
| submit-depth bound (`BB_SUBMIT_DEPTH=2`, wait in `Scheduler::SubmitExecution` for an earlier tick) | **hangs** at the start of the race; the stack dump (`bench/hang_depth2.cdb.txt`) shows the presenter inside the driver's present path. Removed. A submission earlier than the waited one always exists, so the likely cause is a driver-level dependency on a present the blocked thread holds; a safe version needs to exclude the presenter's submissions |
| host-visible readback windows (four variants) | 59.8 / 55.6 / 61.6 / 61.6 FPS, all below the 64.2 baseline; see the table above |
| direct reads from a type-4 mapping | work and are coherent (`vkprobe_type4.exe`: GPU fill visible after a fence wait) — the earlier "direct read differs" was the span bug, not coherence |
| two-thread draw pipe off (`BB_DRAW_PIPE=0`) | 58.18 FPS, 1 % low 43.94, p99 19.97, sd 3.77: flatter and better lows than the baseline, -9 % FPS (the CPU becomes the bottleneck, so this is a rate reduction in effect, not shipped) |
| less GPU work (`BB_MIRROR_RATE=4`) | 64.0 FPS, sd 4.67: no change to the beat |
| `BB_FRAME_TRACE` while measuring | the traced samples are 2-5 % slower (r15 60.4 vs r12 63.2, same build); never compare FPS or sd across traced and untraced runs |
| **copy queue readback** (`BB_COPY_QUEUE=1 BB_READBACK_COPY_QUEUE=1`: a second queue of the graphics family copies the window, its submission waiting only for the timeline value of the write — the doc's "submission boundary recorded after the write") | built and measured. Whole-window copies: 60.3 FPS, sd 4.74 (512 KiB copied per read is more GPU work than the readback saves). Only the marked pages (1-4 KiB usually): 63.0 FPS, sd 4.10, but the fence wait for the write's tick is **4.5 ms mean / 11.9 ms p90**, the same as the normal path's 4.1 ms copy wait. The backlog is *before* the write, so no arrangement of queues removes it. Off by default; the code stays for the record |

The copy-queue numbers settle the question the earlier session left open: the readback wait is not
the copy, not the command thread, and not the queue the copy runs on. It is the GPU reaching the
submission that wrote the descriptor, and every mechanism that waits for that submission pays the
same ~4.5 ms (12 ms at p90). Only making that submission execute earlier (below) or shortening the
queue ahead of it (the lead bound) changes it.

## What is left to do

1. **Make the lead follow the scene.** 85 % is where this benchmark scene reads its GPU results; a
   scene that reads earlier (more compute-heavy frames) or later would want a different value.
   `BB_LEAD_PERCENT` is the knob and `BB_LEAD_PERCENT=0` restores the old behaviour. An auto-tune
   could watch the read-fault time (`BbStats::t_read_faults`) per frame and move the lead a step.
2. **Async compute / queue priority** would remove the remaining ~3 ms: the read still waits for
   graphics work queued ahead of the compute that writes the descriptor. On the console the compute
   ring runs concurrently with graphics; the port records both into one command buffer on one queue.
   A second queue that runs the *readback copy* has been built and measured
   (`BB_READBACK_COPY_QUEUE`) and does not help — the wait is for the write itself, which is behind
   the queue, not for the copy. Splitting the *writing compute* onto a second queue with the
   dependency tracker's semaphores is the only change that can remove the rest.
3. **Cache-ownership rework** (smaller, keeps the memory layout): let a readback copy record without
   draining stage B. Today `BufferCache::RecordReadback` calls `ApplyQueuedStores()` (which returns
   unless the caller is the command thread or stage B), `CollectDownloads()` (consumes
   `gpu_modified_ranges`) and `Runtime::CopyBuffer()` (ends the render pass, updates the barrier
   tracker). `ProcessCommands()` then drains stage B for **every** command, which is what makes the
   command thread the cache owner again. The pattern to copy is the `state_only` fast path at
   `liverpool.cpp` in `ProcessCompute`. This removes the 2.3 ms pickup, which the lead bound has
   already made a smaller part of the stall.

Until then, for play: 4K + DLSS Quality is flat (102.3-103.2 FPS, sd 1.3-1.6 ms in this build's runs;
the 103.6 / sd 0.31 figures earlier in this file are from a different build).

## Run log (2026-10-24, 4K output, `BB_FPS_LIMIT=117`; DLAA unless the row says Quality)

Each line is one `run-game.ps1` invocation; the file is `bench/<name>.txt` / `.frames`. "windows"
means `BB_READBACK_WINDOW_HOSTVISIBLE`.

| run | seconds | flags | FPS | 1 % low | 0.1 % low | p99 | max | sd |
|---|---|---|---|---|---|---|---|---|
| r1_base | 10 | windows=0 | 64.16 | 40.76 | 39.47 | 23.90 | 25.34 | 4.70 |
| r2_hv | 10 | windows=1 (page fast read) | 59.84 | 14.46 | 8.80 | 37.97 | 113.63 | 6.15 |
| r3_fast | 10 | windows=1 (window fast read) | 55.55 | 6.09 | 3.90 | 101.04 | 256.73 | 6.05 |
| r4_coalesce | 10 | windows=1, writes merged | 61.62 | 35.57 | 34.68 | 27.72 | 28.84 | 4.88 |
| r6_hybrid | 10 | windows=1, completed only | 61.64 | 34.91 | 34.00 | 28.17 | 29.41 | 5.34 |
| r7_nopipe | 10 | `BB_DRAW_PIPE=0` | 58.18 | 43.94 | 39.12 | 19.97 | 25.56 | 3.77 |
| r8_lessgpu | 10 | `BB_MIRROR_RATE=4` | 64.03 | 42.64 | 41.94 | 22.74 | 23.85 | 4.67 |
| r9_lead50 | 10 | lead=50 | 62.83 | 46.27 | 45.95 | 21.48 | 21.76 | 3.54 |
| r10_lead30 | 10 | lead=30 | 62.78 | 42.23 | 41.33 | 23.07 | 24.20 | 3.95 |
| r11_lead70 | 10 | lead=70 | 62.09 | 45.67 | 44.85 | 21.54 | 22.29 | 3.03 |
| r12_lead50_20s | 20 | lead=50 | 63.16 | 44.93 | 39.72 | 21.36 | 25.18 | 3.94 |
| r13_base_20s | 20 | lead=0 | 63.41 | 42.73 | 41.56 | 23.01 | 24.06 | 4.44 |
| r14_lead70_20s | 20 | lead=70 | 62.92 | 45.26 | 38.78 | 21.18 | 25.79 | 3.40 |
| r15_final | 20 | default + `BB_FRAME_TRACE` | 60.37 | 40.98 | 34.62 | 23.30 | 28.88 | 4.64 |
| r16_base_20s_b | 20 | lead=0 | 64.59 | 41.86 | 32.28 | 22.74 | 30.98 | 3.36 |
| r17_base_30s | 30 | lead=0 | 64.08 | 38.58 | 26.82 | 23.94 | 37.28 | 3.60 |
| r18_lead50_30s | 30 | lead=50 | 62.75 | 43.20 | 27.88 | 21.66 | 35.86 | 4.11 |
| r19_ship | 20 | default, **DLSS Quality** (no `BB_AA_SCALE=native`) | 100.84 | 66.81 | 62.56 | 14.50 | 16.10 | 1.55 |
| r20_ship_dlaa | 20 | **shipped defaults** (lead=50) | 63.12 | 46.04 | 43.30 | 21.17 | 23.10 | 3.93 |
| r21_qual_lead0 | 10 | DLSS Quality, lead=0 | 102.34 | 65.43 | 61.50 | 14.69 | 16.26 | 1.61 |
| r22_copyq | 10 | lead=0, copy queue, whole window | 60.34 | 38.78 | 37.72 | 25.36 | 26.51 | 4.74 |
| r23_copyq2 | 10 | lead=0, copy queue, marked pages | 63.01 | 39.52 | 38.02 | 24.60 | 26.30 | 4.10 |
| r24_ship2 | 20 | **shipped defaults** (lead=50, copy queue off) | 63.41 | 46.31 | 44.29 | 21.09 | 22.58 | 3.88 |
| r25_lead85 | 10 | lead=85 | 63.00 | 45.79 | 30.45 | 19.46 | 32.84 | 1.83 |
| r26_lead92 | 10 | lead=92 | 63.19 | 41.36 | 38.56 | 22.06 | 25.93 | 2.67 |
| r27_lead80 | 10 | lead=80 | 63.8 (frame log) | — | — | — | 21.64 | 2.20 |
| r28_lead85_20s | 20 | lead=85 | 62.61 | 48.53 | 40.14 | 19.59 | 24.91 | 1.91 |
| r29_ship85 | 20 | **shipped defaults** (lead=85) | 62.64 | 48.48 | 44.24 | 19.98 | 22.61 | 1.85 |
| r30_qual85 | 10 | DLSS Quality, lead=85 (default) | 103.16 | 69.01 | 60.61 | 13.87 | 16.50 | 1.32 |
| r31_ship85_30s | 30 | default, **DLSS Quality** (no `BB_AA_SCALE=native`) | 102.54 | 68.31 | 56.15 | 13.87 | 20.57 | 1.31 |
| r32_ship85_dlaa_30s | 30 | **shipped defaults** (lead=85), 4K DLAA | 62.81 | 46.59 | 30.06 | 19.57 | 33.26 | 2.09 |

`sd` is from `episodes.py` over the steady part of `BB_FRAME_LOG`; long runs include one or two
isolated spikes (30-37 ms) that move `max` and the 0.1 % low by more than the effect being measured.

## Harness (in the workspace, not shipped)

Build with `bash out/profile-build.sh` here, not `build.sh`: the latter's CMake flags leave out
`-include out/msys2-fsr-compat.h`, and the vendored FSR sources then fail with MSYS2 clang
(`_countof`, `swprintf_s`, `log2`, placement `new`). The script writes `out/DriveClubGame.exe`; copy
it to `Driveclub/runtime/DriveClubGame.exe` before a run.

`run-game.ps1` starts the runtime directly with the launcher's settings (so `BB_FRAME_STATS`,
`BB_FRAME_TRACE`, `BB_READBACK_LOG`, `BB_PHASE_ONE` survive; the launcher clears them) and kills a hung
game after `seconds + 210`. `episodes.py` reports frame-time sd and the worst 40-frame windows,
`bursts.py` correlates bursts with `BB_FRAME_TRACE` columns, `tracecols.py` compares slow and fast
frames, `frametime.py` summarises `BB_FRAME_LOG` intervals. For a hang: `cdb -p <pid> -logo out.txt
-c "~*kb 30; !cs -l; .detach; q"` (cdb from the WinDbg package must be copied out of `WindowsApps`,
which refuses to execute it), then symbolise `DriveClubGame+0x...` frames with `llvm-addr2line`.
Run-to-run spread at 4K DLAA is large (the old setting gives 3.4-4.4 ms sd and 62.8-64.6 FPS, the new
default 1.85-2.09 ms and 62.6-62.8): compare settings with interleaved runs and more than one sample.
