# Music and sound

All sound lives in FMOD Studio banks under `audio/fmodstudio/` (`music.bank`, `gui.bank`, `collision.bank`, ambience banks per country, `masterbank.bank` and others). A bank is a RIFF file; its sample data is an FSB5 stream. In this game every bank uses ATRAC9 (the console codec) at 48 kHz.

A bank is replaced as a whole by a loose file at the same path in a mod, like any other game file. `dcmod audio` reads and changes the sounds inside a bank.

```
dcmod audio info @audio/fmodstudio/music.bank
dcmod audio wav @audio/fmodstudio/gui.bank UX_Confirm_03_L -o confirm.wav          # listen to a game sound
dcmod audio import @audio/fmodstudio/gui.bank UX_Confirm_03_L mine.wav -o mymod/audio/fmodstudio/gui.bank
dcmod audio extract @audio/fmodstudio/gui.bank -o out [NAME]                        # the encoded streams (.raw)
dcmod audio replace @audio/fmodstudio/gui.bank NAME stream.raw -o mymod/audio/fmodstudio/gui.bank
```

- `info` lists the sounds ("streams") with name, rate, channels, samples and encoded size. `music.bank` holds 52 streams (full tracks, 3 to 8 MB each), `gui.bank` 164 (menu sounds, countdown beeps, thunder, crowds).
- `wav` decodes a stream to a 16 bit WAV file, so the game's own sounds and music can be heard in any player.
- `import` encodes a WAV file (48 kHz; 8, 16, 24, 32 bit or float; mono or stereo, which is mixed or duplicated to the stream's channels) as ATRAC9 in the configuration of the stream it replaces, and **rebuilds the bank**: the new sound may be shorter or longer than the old one. The other streams and the bank's event data are left as they are. Convert other audio to WAV first, for example `ffmpeg -i song.mp3 -ar 48000 -sample_fmt s16 song.wav`.
- `extract` writes each stream as `NAME.raw` (its superframes, as the game's decoder reads them) and a `streams.json`; `replace` puts such a file back (any length, the bank is rebuilt).

The sound that plays for an event is chosen by the bank's event data, which the kit does not edit: to change what a game sound is, import over the stream it plays (`info` and the names tell which; the sounds with `_L` and `_R` are the left and right sounds of a stereo pair, and `_Ls`, `_Rs` their short versions).

## What was checked

- All 164 streams of `gui.bank` and the first streams of `music.bank` decode with LibAtrac9 from the offsets the kit reads (the offset of a stream in an FSB5 header has 28 bits and counts 16 byte units; an earlier version of the kit read it with 27 bits and found the wrong data for most streams).
- The encoder: a test signal comes back from the decoder with 45 and 68 dB signal to noise ratio (`tools/modkit/tests/at9test.cpp`, part of `test.sh`); a game sound decoded and encoded again into another stream comes back with about 30 dB, which is what a second lossy generation gives.
- In the running game: the stream `countdown_expired_positive01_L` of `gui.bank` was replaced by an 880 Hz beep (1.2 s) made with `import`; the game's audio decoder was given all 53 superframes of the new data while the race started (a log of the frames the decoder gets: `DC_AUDIO_DUMP=file` in the environment writes them, with the configuration and the length of each).

## How the encoder works and what it cannot do

- It writes what the game's decoder (LibAtrac9, MIT licence, `third_party/LibAtrac9`) reads and uses its tables. Frames are mono blocks (a stereo stream is two blocks per frame, no joint stereo) at 48 kHz, with fixed length scale factors, a constant bit allocation gradient chosen per block, and Huffman coded spectra. For each block the band count and gradient value that give the least error within the frame budget are chosen; the budget is the stream's own (the game's streams use 56 to 64 bytes per block and frame, about 84 to 96 kbit/s per channel).
- The stream's configuration (frame size, channel layout) is taken from the stream that is replaced. Only configurations with 48 kHz and mono blocks can be written, which is every configuration in the game's banks (the three that exist: one mono block of 56 byte frames, two mono blocks of 112 or 128 byte frames).
- The decoder's first block (256 samples, 5 ms) is a start up block: the new sound starts that much later than the file. `wav` drops the same 256 samples, so a sound goes out and comes back at the same place.
- The loop points and the volume of a sound are in the bank's event data, not in the stream. The kit does not change them, so a sound that loops in the game keeps looping the way its event says.
- It is a plain encoder, not a tuned one: it has no psychoacoustic model, no joint stereo and no band extension. Music encoded with it is a little worse than the game's own encodes at the same rate.

## Limits

- The bank's event and routing data (which event plays which stream) is not edited by the kit.
- A longer stream was tried in the game: `countdown_expired_positive01_L` (12 KB, 1.2 s) replaced by a 30 s tone (315 KB); the decoder received 562 of its frames in a 25 s run, more than the original has, so the game reads past the old length. A 400 s stereo file was imported over a `music.bank` frontend track (stream 9.6 MB, the bank grows to 250 MB) and decodes back in full with `wav`; the menu music of the home screen is `dc_track5_hybrid_frontend` (found by matching the frames the decoder gets, `DC_AUDIO_DUMP`, against every stream of every bank). A 3 minute 7 second FLAC track converted with `ffmpeg -i in.flac -ar 48000 -ac 2 -sample_fmt s16 song.wav` and imported over all five `dc_track*_hybrid_frontend` streams was heard playing at the home menu, and its frames were what the decoder received.
- A music bank is large (240 MB). Ship a changed `music.bank` only when the mod is about music.
