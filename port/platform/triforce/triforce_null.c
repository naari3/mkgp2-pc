/* Triforce board back end with no devices. See README.md in this directory. */
#include <mkgp2pc/platform.h>

const char* mkgp2pc_triforce_backend_name(void) { return "null"; }
void mkgp2pc_triforce_init(void) {}
void mkgp2pc_triforce_shutdown(void) {}
