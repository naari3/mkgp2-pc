#!/usr/bin/env python3
"""A STUN server that answers Binding requests with XOR-MAPPED-ADDRESS, for tests.
   fakestun.py ADDRESS PORT [PORT...]"""
import socket, select, struct, sys
socks = []
for p in sys.argv[2:]:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((sys.argv[1], int(p)))
    socks.append(s)
while True:
    for s in select.select(socks, [], [])[0]:
        d, a = s.recvfrom(2048)
        if len(d) < 20 or d[:2] != b"\x00\x01" or d[4:8] != b"\x21\x12\xa4\x42":
            continue
        ip = struct.unpack("!I", socket.inet_aton(a[0]))[0] ^ 0x2112A442
        attr = struct.pack("!HHBBHI", 0x0020, 8, 0, 1, a[1] ^ 0x2112, ip)
        s.sendto(struct.pack("!HH", 0x0101, len(attr)) + d[4:20] + attr, a)
        print("stun", a, "on", s.getsockname()[1], flush=True)
