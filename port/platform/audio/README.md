# platform/audio

PC replacement for the audio hardware under MusyX.

The game's sound code (`src/sound`) calls MusyX (`src/musyx/runtime`), which
stays as C. What gets replaced is the hardware end MusyX talks to: AI (DAC
stream), DSP (voice mixing) and ARAM (sample memory). Aurora has no audio
layer (it only allocates the ARAM region), so this is ours to write.

Back ends, one `audio_<name>.c` each, all implementing the `mkgp2pc_audio_*`
hooks in `../include/mkgp2pc/platform.h`:

| Back end | State |
|---|---|
| `null` | present; no sound, so the game can run silent |
| `sdl` | not written; software mixer feeding an SDL3 audio stream |
