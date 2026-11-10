/* Triforce hardware emulation: links between the pieces in port/platform/triforce. */
#ifndef MKGP2PC_TRIFORCE_H
#define MKGP2PC_TRIFORCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Media board (mediaboard.c). The DVD layer routes absolute reads of board
 * addresses here. */
int mkgp2pc_mediaboard_owns(uint32_t address);
void mkgp2pc_mediaboard_read(uint32_t address, void* buffer, uint32_t length);
void mkgp2pc_mediaboard_write(uint32_t address, const void* buffer, uint32_t length);
int mkgp2pc_mediaboard_irq(void); /* baseboard status bits: 4 main board, 0x10 network board */

/* Baseboard (baseboard.c): runs the callback the game registered for an EXI
 * channel, as the interrupt would. */
void mkgp2pc_baseboard_raise_exi(int channel);

#ifdef __cplusplus
}
#endif

#endif
