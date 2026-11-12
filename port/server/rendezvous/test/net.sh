#!/bin/sh
# inet (203.0.113.1: STUN + rendezvous) -- r1 (203.0.113.11) -- h1 (192.168.1.2)
#                                       \_ r2 (203.0.113.12) -- h2 (192.168.2.2)
# NAT=masq (port kept, like most home routers) or NAT=random (another outside port per destination)
for n in inet r1 r2 h1 h2; do ip netns del $n 2>/dev/null; done
set -e
for n in inet r1 r2 h1 h2; do ip netns add $n; ip -n $n link set lo up; done
ip -n inet link add br0 type bridge; ip -n inet addr add 203.0.113.1/24 dev br0; ip -n inet link set br0 up
for i in 1 2; do
  ip link add w$i netns r$i type veth peer name p$i netns inet
  ip -n inet link set p$i master br0; ip -n inet link set p$i up
  ip -n r$i addr add 203.0.113.1$i/24 dev w$i; ip -n r$i link set w$i up
  ip link add l$i netns r$i type veth peer name e0 netns h$i
  ip -n r$i addr add 192.168.$i.1/24 dev l$i; ip -n r$i link set l$i up
  ip -n h$i addr add 192.168.$i.2/24 dev e0; ip -n h$i link set e0 up
  ip -n h$i route add default via 192.168.$i.1
  ip netns exec r$i sysctl -qw net.ipv4.ip_forward=1
  eval kind=\${NAT$i:-masq}
  if [ "$kind" = random ]; then flag="--random-fully"; else flag=""; fi
  ip netns exec r$i iptables -t nat -A POSTROUTING -o w$i -j MASQUERADE $flag
  ip netns exec r$i iptables -A FORWARD -i w$i -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
  ip netns exec r$i iptables -A FORWARD -i w$i -j DROP
  ip netns exec r$i iptables -A INPUT -i w$i -j DROP  # as home routers do; an answered or rejected datagram would occupy the port pair
done
