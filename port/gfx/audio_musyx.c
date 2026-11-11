/*
 * Sound output for the rendering executable.
 *
 * The game's MusyX calls (snd*) go to the PC target of the MusyX
 * decompilation (port/extern/musyx, CC0), built as a library without a
 * device layer; its renderer mixes in software what the DSP mixes on the
 * console and reads the game's sound data as it is on the disc. This file
 * is the device layer: an SDL audio stream that pulls from sndPCRender.
 *
 * The link wraps sndInit (port/tools/link_census.py), which is where the
 * game brings the sound system up.
 */
#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* include/musyx/pc.h of the library; `bool` is the game's 4-byte one there
 * (the library is built with it, docs/port/build.md) */
typedef struct SND_PC_CONFIG {
  uint32_t mixRate;
  uint32_t channels;
} SND_PC_CONFIG;
unsigned long sndPCConfigure(const SND_PC_CONFIG* config);
unsigned long sndPCSetSynchronization(void (*enter)(void), void (*leave)(void));
unsigned long sndPCRender(int16_t* output, size_t frames);

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);

#define MIX_RATE 32000
#define CHANNELS 2

/* MusyX's critical section is the game's "interrupts off": the game's own
 * stream callbacks run inside the renderer, on the audio thread. */
static _Thread_local int s_levels[32];
static _Thread_local int s_depth;
static void sync_enter(void) {
  int was = OSDisableInterrupts();
  if (s_depth < 32) {
    s_levels[s_depth] = was;
  }
  s_depth++;
}
static void sync_leave(void) {
  s_depth--;
  OSRestoreInterrupts(s_depth < 32 ? s_levels[s_depth] : 0);
}

static void SDLCALL audio_callback(void* userdata, SDL_AudioStream* stream, int additional, int total) {
  int16_t buffer[1024 * CHANNELS];
  (void)userdata, (void)total;
  while (additional > 0) {
    int frames = additional / (int)(CHANNELS * sizeof(int16_t));
    if (frames > 1024) {
      frames = 1024;
    }
    if (frames <= 0) {
      break;
    }
    if (!sndPCRender(buffer, (size_t)frames)) {
      memset(buffer, 0, (size_t)frames * CHANNELS * sizeof(int16_t));
    }
    SDL_PutAudioStreamData(stream, buffer, frames * (int)(CHANNELS * sizeof(int16_t)));
    additional -= frames * (int)(CHANNELS * sizeof(int16_t));
  }
}

int32_t __real_sndInit(uint8_t voices, uint8_t music, uint8_t sfx, uint8_t studios, uint32_t flags, uint32_t aramSize);
int32_t __wrap_sndInit(uint8_t voices, uint8_t music, uint8_t sfx, uint8_t studios, uint32_t flags, uint32_t aramSize) {
  static SDL_AudioStream* stream;
  const SND_PC_CONFIG config = {MIX_RATE, CHANNELS};
  sndPCConfigure(&config);
  sndPCSetSynchronization(sync_enter, sync_leave);
  int32_t result = __real_sndInit(voices, music, sfx, studios, flags, aramSize);
  if (result != 0 || stream != NULL) {
    return result;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr, "audio: no audio subsystem (%s); the game runs silent\n", SDL_GetError());
    return result;
  }
  const SDL_AudioSpec spec = {SDL_AUDIO_S16, CHANNELS, MIX_RATE};
  stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
  if (stream == NULL) {
    fprintf(stderr, "audio: no playback device (%s); the game runs silent\n", SDL_GetError());
    return result;
  }
  SDL_ResumeAudioStreamDevice(stream);
  fprintf(stderr, "audio: %d Hz, %d channels\n", MIX_RATE, CHANNELS);
  return result;
}
