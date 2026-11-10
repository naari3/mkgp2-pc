#ifndef MKGP2PC_PLATFORM_H
#define MKGP2PC_PLATFORM_H

/*
 * Entry points of the PC platform layer (port/platform).
 *
 * Each device family has one back end selected at build time. Today every
 * family has only the "null" back end, which does nothing and reports itself
 * as such; real ones (SDL audio mixer, JVS on SDL input, card reader on a
 * file, ...) are added beside it under the same interface.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char* family;  /* "audio", "triforce" */
  const char* backend; /* "null" until a real one exists */
} Mkgp2PcBackendInfo;

/* Bring every back end up / down. Call around the Aurora main loop. */
void mkgp2pc_platform_init(void);
void mkgp2pc_platform_shutdown(void);

/* Table of the back ends compiled in; *count receives its length. */
const Mkgp2PcBackendInfo* mkgp2pc_platform_backends(int* count);

/* Per-family hooks, implemented once per back end. */
const char* mkgp2pc_audio_backend_name(void);
void mkgp2pc_audio_init(void);
void mkgp2pc_audio_shutdown(void);

const char* mkgp2pc_triforce_backend_name(void);
void mkgp2pc_triforce_init(void);
void mkgp2pc_triforce_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
