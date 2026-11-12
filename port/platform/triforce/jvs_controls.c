/*
 * Wheel and pedal positions as the values the cabinet's potentiometers
 * would give on the JVS board's analog channels 0 to 2.
 *
 * The game does not take the channels' whole 16 bits as the travel. It
 * keeps a centre and a range per channel (set from the calibration in its
 * settings record, JvsInput_Set*Calibration in src/game/
 * JvsInput_Calibration.c) and InputObj_ReadRawInput turns a reading into
 *     wheel   (raw - centre) / range       held to -1 .. 1, range 0x5000
 *     pedal   (raw - centre) / range       held to  0 .. 1, range 0x4000,
 *                                          centre = released reading + 0x800
 * so a position has to be laid onto that window to arrive as the same
 * position. The pedals keep the cabinet's free play: the first 0x800 of
 * 0x4800 (a ninth of the travel) does nothing, and a calibration run in the
 * test menu with a controller attached reads the same values it is given.
 * The keyboard does not come through here; it sends the ends of the 16 bits.
 */
#include <stdint.h>

extern int g_steeringCenterRaw, g_steeringMaxRange;
extern int g_accelCenterRaw, g_accelRange;
extern int g_brakeCenterRaw, g_brakeRange;

static uint16_t raw16(float v) {
  return v <= 0.0f ? 0 : v >= 65535.0f ? 0xFFFF : (uint16_t)(v + 0.5f);
}

/* -1 (left) .. 1 (right) */
uint16_t mkgp2pc_jvs_steer_raw(float position) {
  float p = position < -1.0f ? -1.0f : position > 1.0f ? 1.0f : position;
  return raw16((float)g_steeringCenterRaw + p * (float)g_steeringMaxRange);
}

/* 0 (released) .. 1 (to the floor) */
uint16_t mkgp2pc_jvs_pedal_raw(int brake, float position) {
  enum { FREE_PLAY = 0x800 };
  float p = position < 0.0f ? 0.0f : position > 1.0f ? 1.0f : position;
  int centre = brake ? g_brakeCenterRaw : g_accelCenterRaw;
  int range = brake ? g_brakeRange : g_accelRange;
  return raw16((float)(centre - FREE_PLAY) + p * (float)(range + FREE_PLAY));
}
