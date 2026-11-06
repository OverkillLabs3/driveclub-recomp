# Music and sound

All sound lives in FMOD Studio banks under `audio/fmodstudio/` (`music.bank`, `gui.bank`, `collision.bank`, ambience banks per country, `masterbank.bank` and others). A bank is a RIFF file; its sample data is an FSB5 stream. In this game every bank uses ATRAC9 (the console codec) at 48 kHz.

A bank can be replaced as a whole by a loose file at the same path in a mod, like any other game file. `dcmod audio` works on the streams inside a bank.

```
dcmod audio info @audio/fmodstudio/music.bank
dcmod audio extract @audio/fmodstudio/gui.bank -o out [NAME]
dcmod audio replace @audio/fmodstudio/gui.bank NAME new.raw -o mymod/audio/fmodstudio/gui.bank
```

- `info` lists the streams with name, rate, channels, samples and encoded size. `music.bank` holds 52 streams, `gui.bank` 164.
- `extract` writes each stream as `NAME.raw` (the encoded data) and a `streams.json`.
- `replace` puts a file in place of one stream. The file must be in the same codec and no longer than the old stream; the rest is zero filled, so the bank keeps its size and every other stream stays where it was. A stream of the same channel layout and rate is needed, and a shorter stream plays a shorter sound.

Checked in the game: a `gui.bank` with a replaced stream is loaded and the race starts.

## Limits

- There is no encoder to ATRAC9 in the kit. Streams can be moved between sounds of the same layout (for example a UI sound onto another UI sound), or encoded with an external ATRAC9 encoder and the result used as the raw file.
- The bank's event and routing data (which event plays which stream) is not edited by the kit; replacing a stream keeps the events as they are.
