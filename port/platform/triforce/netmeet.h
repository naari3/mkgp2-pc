/* Finding the other cabinets by a room name (netmeet.c), for netlink.c. */
#ifndef MKGP2PC_NETMEET_H
#define MKGP2PC_NETMEET_H

#include <stdint.h>

/* 1: MKGP2PC_LINK_ROOM and MKGP2PC_LINK_SERVER are set. */
int mkgp2pc_meet_on(void);
/* At boot: the cabinet number the room's server gives, 0 none. `wanted`
 * (1-4, 0 any) is given when nobody in the room has it, else the first
 * free one. Waits for the answer, five seconds at most (twice). */
int mkgp2pc_meet_boot_id(int wanted);
/* Every retrace. `socket_` is this cabinet's UDP socket, `index` its number
 * minus one or -1 while it has none. Asks the STUN server through the
 * socket and hands the room's members to mkgp2pc_netlink_room_peer(). */
void mkgp2pc_meet_tick(uintptr_t socket_, int index, uint64_t now);
/* A datagram that arrived at the socket: 1 when it was a STUN answer. */
int mkgp2pc_meet_datagram(const uint8_t* d, int n, const void* from);
/* With a room: 0 for a sender (address in network order) that is neither a
 * member of the room nor on this PC or LAN. Without a room always 1. */
int mkgp2pc_meet_allows(uint32_t host);

/* In netlink.c: cabinet `index` is at this address (network order). */
void mkgp2pc_netlink_room_peer(int index, uint32_t host, uint16_t port);

#endif
