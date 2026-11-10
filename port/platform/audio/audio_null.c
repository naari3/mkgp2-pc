/* Audio back end that produces no sound. See README.md in this directory. */
#include <mkgp2pc/platform.h>

const char* mkgp2pc_audio_backend_name(void) { return "null"; }
void mkgp2pc_audio_init(void) {}
void mkgp2pc_audio_shutdown(void) {}
