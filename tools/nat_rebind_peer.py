#!/usr/bin/env python3
"""Peers for nat_rebind_test.sh.

host:   answers on OLD; after --switch seconds sends from NEW instead and stops
        listening on OLD (what a NAT rebinding looks like from the outside).
client: sends to host:OLD and, like Parsec, only accepts packets from
        host:OLD. Prints how many packets each side got per second.
"""
import argparse
import select
import socket
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("role", choices=["host", "client"])
ap.add_argument("--host-ip", default="10.99.0.2")
ap.add_argument("--client-ip", default="10.99.0.1")
ap.add_argument("--old", type=int, default=12092)
ap.add_argument("--new", type=int, default=30666)
ap.add_argument("--lport", type=int, default=30297)
ap.add_argument("--switch", type=float, default=4.0)
ap.add_argument("--duration", type=float, default=12.0)
ap.add_argument("--pps", type=int, default=200)
a = ap.parse_args()


def udp(ip, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((ip, port))
    s.setblocking(False)
    return s


start = time.monotonic()
counts = {}
interval = 1.0 / a.pps
next_send = start

if a.role == "host":
    old, new = udp(a.host_ip, a.old), udp(a.host_ip, a.new)
    client = (a.client_ip, a.lport)
    while (t := time.monotonic() - start) < a.duration:
        active = old if t < a.switch else new
        r, _, _ = select.select([active], [], [], max(0, next_send - time.monotonic()))
        if r:
            try:
                while active.recvfrom(2048):
                    counts[int(t)] = counts.get(int(t), 0) + 1
            except BlockingIOError:
                pass
        if time.monotonic() >= next_send:
            active.sendto(b"h" * 100, client)
            next_send += interval
    print("host   got/s:", [counts.get(i, 0) for i in range(int(a.duration))], flush=True)
else:
    s = udp(a.client_ip, a.lport)
    peer = (a.host_ip, a.old)
    while (t := time.monotonic() - start) < a.duration:
        r, _, _ = select.select([s], [], [], max(0, next_send - time.monotonic()))
        if r:
            try:
                while True:
                    data, src = s.recvfrom(2048)
                    if src == peer:  # Parsec pins the peer address
                        counts[int(t)] = counts.get(int(t), 0) + 1
            except BlockingIOError:
                pass
        if time.monotonic() >= next_send:
            s.sendto(b"c" * 100, peer)
            next_send += interval
    print("client got/s:", [counts.get(i, 0) for i in range(int(a.duration))], flush=True)
