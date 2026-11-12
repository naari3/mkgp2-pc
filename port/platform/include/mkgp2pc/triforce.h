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
/* Once per retrace: answers that had to wait (accept, connect) are delivered. */
void mkgp2pc_mediaboard_tick(void);
/* Board memory, for the command handlers. */
void mkgp2pc_mediaboard_load(uint32_t address, uint8_t* data, uint32_t length);
void mkgp2pc_mediaboard_store(uint32_t address, const uint8_t* data, uint32_t length);

/* Cabinet link (netlink.c): the network board's socket commands on host
 * sockets. `command` is the value amsock.c stores, `arg` the six arguments
 * in host order, `result` the packet's result field as the game reads it. */
enum { MKGP2PC_NETLINK_UNKNOWN, MKGP2PC_NETLINK_DONE, MKGP2PC_NETLINK_LATER };
int mkgp2pc_netlink_command(uint16_t command, const uint32_t* arg, uint32_t* result);
int mkgp2pc_netlink_poll(uint16_t command, const uint32_t* arg, uint32_t* result);
void mkgp2pc_netlink_tick(void);
void mkgp2pc_netlink_pump(void); /* delayed sends of the measurement options; any time, lock held */
/* For the game flow (linkflow.c); see netlink.c. */
void mkgp2pc_netlink_set_active(int on);    /* the link at run time; starts on */
unsigned mkgp2pc_netlink_heard_mask(void);  /* bit k: cabinet k+1 heard in the last 3 s */
int mkgp2pc_netlink_address_ok(void);       /* 0: the cabinet number just set is taken on this PC */

/* Baseboard (baseboard.c): runs the callback the game registered for an EXI
 * channel, as the interrupt would. */
void mkgp2pc_baseboard_raise_exi(int channel);

/* Devices on the baseboard's two serial ports (jvs_si.c forwards the
 * traffic). Port 0 is the steering drive board (drive_board.c), port 1 the
 * card unit (card_reader.c). */
void mkgp2pc_serial_write(int port, const uint8_t* data, int n);
int mkgp2pc_serial_read(int port, uint8_t* out, int max);
void mkgp2pc_card_serial_write(const uint8_t* data, int n);
int mkgp2pc_card_serial_read(uint8_t* out, int max);
/* The player puts the card (the card file) into the unit's slot. */
void mkgp2pc_card_insert(void);
/* 0 with MKGP2PC_CARD=0: the game is then started with the DIP switch that
 * turns its card service off (jvs_si.c). */
int mkgp2pc_card_enabled(void);
/* First byte of the I/O board's general purpose outputs, as the game last
 * wrote it. Bit 0x80 enables the drive board; the others are lamps. */
void mkgp2pc_jvs_outputs(uint8_t bits);

#ifdef __cplusplus
}
#endif

#endif
