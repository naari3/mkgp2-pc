#include <mkgp2pc/platform.h>

static Mkgp2PcBackendInfo s_backends[2];

void mkgp2pc_platform_init(void) {
  mkgp2pc_audio_init();
  mkgp2pc_triforce_init();
}

void mkgp2pc_platform_shutdown(void) {
  mkgp2pc_triforce_shutdown();
  mkgp2pc_audio_shutdown();
}

const Mkgp2PcBackendInfo* mkgp2pc_platform_backends(int* count) {
  s_backends[0].family = "audio";
  s_backends[0].backend = mkgp2pc_audio_backend_name();
  s_backends[1].family = "triforce";
  s_backends[1].backend = mkgp2pc_triforce_backend_name();
  if (count) {
    *count = 2;
  }
  return s_backends;
}
