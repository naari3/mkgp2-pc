/* The cabinet's player camera on the PC (port/platform/camera/camera.c,
 * docs/port/camera.md). */
#ifndef MKGP2PC_CAMERA_H
#define MKGP2PC_CAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The picture the game takes from its camera. */
enum { MKGP2PC_CAMERA_WIDTH = 320, MKGP2PC_CAMERA_HEIGHT = 240 };

/* 0 unless MKGP2PC_CAMERA names a picture source. */
int mkgp2pc_camera_enabled(void);

/* For the network stand-in (triforce/netlink.c), which plays the camera's
 * web server.
 *  - request: the game has asked for a picture. Wakes the device up.
 *  - reply: 1 when the answer to the last request is complete; `data` and
 *    `size` then give the whole HTTP answer, valid until the next request.
 *    0: not yet (the device is still starting). Never fails: a device that
 *    gives nothing is answered for with a grey picture. */
void mkgp2pc_camera_request(void);
int mkgp2pc_camera_reply(const uint8_t** data, uint32_t* size);
/* Once per retrace: a device nobody has asked for some seconds is let go. */
void mkgp2pc_camera_tick(void);

/* For a device back end (camera_mf.c), called on the capture thread:
 * one captured picture. `stride` is the distance between rows in bytes and
 * may be negative (then `pixels` is still the first byte of the top row).
 * `bytesPerPixel` 3 or 4, blue first. */
void mkgp2pc_camera_store(const uint8_t* pixels, int width, int height, int stride, int bytesPerPixel);

/* The device back end, all three called on the capture thread only.
 * open: `name` is "" for the first device, a number (1 = first) or part of
 * the device's name; 0 on success, otherwise `error` holds a line of text.
 * grab: waits for one picture and hands it to mkgp2pc_camera_store(); 0 on
 * success or when none came this time, -1 when the device is gone. */
int mkgp2pc_camera_device_open(const char* name, char* error, int errorSize);
int mkgp2pc_camera_device_grab(void);
void mkgp2pc_camera_device_close(void);

#ifdef __cplusplus
}
#endif

#endif
