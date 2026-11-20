# Parsec Linux client: bug reports and Steam Controller fixes

Bugs in the Parsec Linux client (tested on `150-105c`). The first two make the Steam Controller unusable through Steam Input:

1. **Controller not detected.** Gamepads that already exist when Parsec starts are opened but never registered. Steam always creates its virtual pad before it starts Parsec, so launching Parsec from Steam gives "No controllers detected". Pads plugged in later work.
2. **Stuck buttons.** libmatoya reads only one input event per gamepad per frame. The Steam Controller's trackpads and gyro send events much faster than that, so the kernel queue overflows and button releases are lost. Upstream: [libmatoya#145](https://github.com/snowcone-ltd/libmatoya/issues/145), fixes in [#146](https://github.com/snowcone-ltd/libmatoya/pull/146) and [#152](https://github.com/snowcone-ltd/libmatoya/pull/152).

3. **Frozen sessions.** When the host's NAT re-maps its UDP port mid-session, the client keeps sending to the old port and ignores the host's packets from the new one. The picture freezes, and `-12007` is logged minutes later.

The analysis, evidence and suggested fixes are in **[REPORT.md](REPORT.md)**.

## Workaround

Needs `gcc`, `python-evdev`, and write access to `/dev/uinput` (desktop sessions usually grant this through the `uaccess` udev tag).

```bash
cd workaround
make install          # installs to ~/.local (override with PREFIX=...)
```

In Steam, set the Parsec shortcut's **Properties → Launch Options** to:

```
~/.local/bin/parsec-steam-launch %command%
```

What it does:
- `evdev_coalesce.so` is preloaded into Parsec. It reads the whole input queue, coalesces stick movement without dropping any button event, and recovers from kernel queue overflows. It also hides Steam's own virtual pad from Parsec.
- `parsec-pad-proxy` takes exclusive control of Steam's virtual pad and mirrors it to a new pad, created a few seconds after Parsec has started. Parsec sees exactly one controller, including after the controller sleeps or is unplugged and replugged.

Limitation: rumble isn't forwarded.

## Workaround for frozen sessions (bug 3)

`parsec-nat-follow` is a small root service. It watches Parsec's UDP socket, and when the host's packets switch to a new public port, it adds `nftables` NAT rules: outgoing packets go to the new port, and incoming packets look to Parsec like they still come from the old one. The flow gets a fresh conntrack zone each time, and the rules are removed when the session ends. It handles IPv4 only.

```bash
cd workaround
sudo make install-nat-follow
sudo systemctl daemon-reload
sudo systemctl enable --now parsec-nat-follow
journalctl -u parsec-nat-follow -f      # logs each port change it follows
```

`tools/nat_rebind_test.sh` simulates a rebinding between two network namespaces and needs no root (`unshare -rn tools/nat_rebind_test.sh`, or add `--without` for the baseline).

The proper fix is in the client (follow authenticated packets from a new address), or on the host's network (a stable public port for the host).

## Layout

| Path | Contents |
|---|---|
| `REPORT.md` | full bug report |
| `workaround/` | launcher, pad proxy, preload shim, `parsec-nat-follow` + systemd unit, Makefile |
| `upstream/libmatoya-evdev-drain.patch` | upstream fix for bug 2 |
| `tools/matoya_sim.py` | simulation of libmatoya's input loop, with and without the fix |
| `tools/nat_rebind_test.sh` | NAT rebinding simulation for `parsec-nat-follow` |

## License

MIT, see [LICENSE](LICENSE). `upstream/libmatoya-evdev-drain.patch` modifies libmatoya, which is also MIT-licensed.
