// Checks that the graphics driver offers what the renderer needs, before the game starts.
#pragma once
#include <string>

// "" when the graphics card is fine, or when the check could not be completed (the game then starts as usual).
// Otherwise a sentence for the player: what is missing and what to do about it. The query runs in a separate
// process with a time limit, so a driver that hangs or crashes cannot stop the launcher. When the best card fails but
// another one works, BB_GPU_ID is set so the renderer uses the one that works.
// The launcher has a switch for skipping the check (skip_gpu_check=1 in launcher.ini); an empty file named
// skip_gpu_check.txt next to the launcher does the same.
std::wstring CheckGpu();

// "" when the processor has what the game's code needs (AVX, POPCNT, F16C: Intel Ivy Bridge from 2012,
// AMD Piledriver/Ryzen, or newer; MOVBE/BMI1/LZCNT are lowered at runtime), otherwise a sentence naming what is missing.
std::wstring CheckCpu();

// Entry point of the query process (DriveClub.exe --gpu-check <result file>). Exit code 0: fine, 1: refused.
int GpuCheckChildMain(const wchar_t* result_path);

// One line each for the log of the launcher: the processor with the features the game needs, and Windows with the
// memory of the PC.
std::wstring DescribeCpu();
std::wstring DescribeSystem();
