# Parsec Linux client: bug reports

**Environment:** Parsec Linux client `release1 150-105c` (self-updated from the `parsec-bin 150-104a` package), Arch-based distro, kernel 7.3. Steam (Linux) with the new **Steam Controller** (wireless puck `28de:1304`), exposed to applications through **Steam Input's virtual X-Box 360 pad** (`28de:11ff`, "Microsoft X-Box 360 pad 0"). Parsec is launched from Steam as a non-Steam shortcut. Host: Windows work machine.

---

## Bug 1: gamepads that exist when Parsec starts are never registered

**Symptom:** Settings → Gamepad shows "No controllers detected", and the host gets no gamepad input. This always happens when Parsec is launched from Steam, because Steam creates its virtual pad a few seconds **before** it starts the game process (kernel log: pad at 10:29:12, Parsec log "Started" at 10:29:16).

**What happens:**

- Parsec opens the device: `/proc/<pid>/fd` shows `/dev/input/event15` (the Steam virtual pad) open for the whole lifetime of the process.
- The device is never registered as a controller.
- Any pad that **appears after** Parsec is running (hotplug through the udev monitor) is registered and works normally. We confirmed this by creating a uinput mirror of the same pad, with the same VID/PID, after startup.

**Analysis:** In libmatoya (`src/unix/linux/x11/evdev.c`), the initial `udev_enumerate` scan runs on the first `mty_evdev_poll()` call (the `init_scan` flag). In the 150-105c binary this check is at `0xab2aa`. That scan fires `MTY_EVENT_CONNECT` for every device already present. Hotplug connects take the same path and work, so the Parsec application code apparently drops the connect events from that first poll. Our guess is that its gamepad/controller state isn't initialised yet on the first iteration of the app loop.

**Suggested fix (in the Parsec app):** handle `MTY_EVENT_CONNECT` from the first poll, or re-enumerate controllers once the app is fully initialised.

**Workaround we use:** `parsec-pad-proxy` plus `parsec-steam-launch` (in `workaround/`). After Parsec has started, they create a uinput mirror pad and grab the original pad. The preload shim also hides Steam's own pad from Parsec, so you don't see a second, dead "HID Controller".

---

## Bug 2: stuck buttons and input lag with high-rate gamepads (libmatoya evdev reader)

**Symptom:** with the Steam Controller, buttons regularly stay "pressed" on the host. This doesn't happen with a real Xbox controller.

**Cause:** `mty_evdev_poll()` calls `poll()` on all evdev fds, then `evdev_joystick_event()` `read()`s **exactly one** `struct input_event` per ready fd per app-loop iteration. Confirmed both in upstream master (`952f2a2`) and in the 150-105c binary (`read(fd, buf, 0x18)` at `0xab310`). Steam Input's virtual pad, driven by the Steam Controller's trackpads and gyro, produces far more events per second than the loop consumes. As a result:

1. Input lag grows as the kernel's evdev client buffer fills.
2. The buffer overflows. The kernel discards the queue and emits `SYN_DROPPED`, which libmatoya ignores, so **button-release events are lost** and buttons stay stuck.

**Reproduction (simulation):** a reader that works like libmatoya (poll, then one read per frame at 60 Hz), against a uinput pad sending stick updates at about 1 kHz plus 40 taps of A and 40 taps of the d-pad:

| reader | A presses | A releases | d-pad taps |
|---|---|---|---|
| one event per frame (current) | 1/40 | 1/40 | 2/40 |
| drain + coalesce (fix) | 40/40 | 40/40 | 40/40 |

**Fix:** `libmatoya-evdev-drain.patch` (against libmatoya master `952f2a2`, compiles cleanly):

- `evdev_joystick_event()` reads in batches until `EAGAIN`.
- Button and d-pad changes are reported individually, so a press and release in the same batch are both delivered.
- Analog axis changes are coalesced into one report per poll.
- `SYN_DROPPED` is handled as the evdev protocol requires: discard events up to the next `SYN_REPORT`, then resync the state from `EVIOCGKEY` and `EVIOCGABS`.

The fix only touches `evdev.c`, and the public API doesn't change.

**Workaround we use until a fix ships:** `evdev_coalesce.c`, an `LD_PRELOAD` shim that does the same draining and coalescing behind Parsec's 24-byte `read()` calls on evdev fds. Its `poll()` wrapper reports fds with queued events as readable.

---

## Files in this repository

- `upstream/libmatoya-evdev-drain.patch`: upstream fix for bug 2, submitted as https://github.com/snowcone-ltd/libmatoya/pull/152. The bug was already reported upstream as https://github.com/snowcone-ltd/libmatoya/issues/145, with an earlier, simpler fix in https://github.com/snowcone-ltd/libmatoya/pull/146 (no `SYN_DROPPED` handling).
- `workaround/evdev_coalesce.c`: `LD_PRELOAD` workaround for bug 2; it also hides Steam's own pads, which is part of the bug 1 workaround.
- `workaround/parsec-pad-proxy`, `workaround/parsec-steam-launch`: workaround for bug 1 (Steam launch wrapper and mirror pad).
- `tools/matoya_sim.py`: the simulation behind the table under bug 2.

---

## Aside: native Steam Controller support

The Steam Controller only reaches Parsec through Steam Input's virtual Xbox pad. So it only works when Parsec is launched from Steam, and it depends on Steam's focus and action-set handling. Steam's virtual pad is also what triggers both bugs above.

SDL3 already supports these controllers directly through HIDAPI, under the MIT-compatible zlib license:

- `src/joystick/hidapi/SDL_hidapi_steam_triton.c`: the new Steam Controller and its wireless dongle (`28de:1304` / `28de:1305`; `USB_PRODUCT_VALVE_STEAM_PROTEUS_DONGLE` / `..._NEREID_DONGLE` in `usb_ids.h`)
- `SDL_hidapi_steam.c`: the original Steam Controller
- `SDL_hidapi_steamdeck.c`: Steam Deck controls

Parsec (or libmatoya's HID layer, which already has drivers for other pads) could use the same approach and support Steam controllers directly, without needing Steam to run.

---

## Bug 3: session freezes when the host's NAT re-maps its UDP port

**Symptom:** about half of the connections freeze shortly after connecting. The client logs `Client disconnected with status code: -12007` only minutes later. A session that survives the first few minutes usually stays stable all day.

**Setup:** both ends are behind NAT. The host is behind a corporate firewall; the client is behind a home router. The log shows a direct connection (`net = BUD|<host public IP>|<port>`, `BUD AES_GCM = 256`).

**Packet capture of a frozen session** (client side, UDP only; times are seconds after the connection started):

| t | what happens |
|---|---|
| 0 s | P2P flow established: client `:30297` ↔ host `<host IP>:12092`, ~300–700 packets/s each way |
| 42.7 s | host packets suddenly arrive from **`<host IP>:30666`** instead of `:12092`. The stream is otherwise unchanged: same ~30 ms cadence of 112-byte packets, same header counter (`…0041`) continuing across the switch. So the host is still using the same socket; the host-side NAT gave the flow a new external port. |
| 42.7 s → | the client keeps sending **only to `:12092`**, which no longer reaches the host, and never switches to `:30666`. The picture freezes. |
| 102 s | the host stops sending, about 60 s after the switch (its own timeout: it stopped receiving the client's packets). Two packets also arrive from a third port (`:54138`); the client ignores them too. |
| 160 s+ | the client is still sending to `:12092` at ~130–230 packets/s and hasn't timed out yet. It eventually reports `-12007`. |

The other freeze that night (just before the capture started) ended after 43 s as well, so the re-mapping seems to happen at a fixed point after the session starts. Sessions where it doesn't happen stay up all day.

**Cause:** the client pins the peer's address and port for the whole session. When a NAT re-maps the peer ("NAT rebinding"), the client drops the peer's packets even though they authenticate correctly with the session key.

**Suggested fix (client):** accept authenticated packets from a new source address/port and switch the send address to it (address roaming, as in WireGuard or QUIC connection migration). The packets are AES-GCM authenticated, so following a new address after a successful decrypt is safe.

**Workaround on the client side:** `workaround/parsec-nat-follow` (a root service) notices the port change and adds `nftables` NAT rules, so Parsec keeps talking to the old port while the packets actually use the new one. In a simulation (`tools/nat_rebind_test.sh`) traffic resumes about a second after the switch. Confirmed in a real session: the host's port changed 55 s in, the helper followed it within a second (`peer … moved 53680 -> 24693 (568 pkt/s)`), and the session carried on without dropping.

**Workaround on the host side:** give the host a stable public UDP mapping, e.g. a static port forward on the host's firewall to the host's Parsec ports (`host_start_port`), or a NAT policy that keeps the same external port for the lifetime of the flow.

**Related:** after a disconnect followed by a reconnect within about a second, the client log fills with thousands of `MTY_AESGCMDecrypt: 'EVP_DecryptFinal_ex' failed` lines (19,330 in one case), followed by `nat_read_packet = -34001` and repeated `bud_thread = -28002`. It looks like the client still receives the previous session's packets and tries to decrypt each one with the new key. Those packets should be dropped quietly, and probably rate-limited in the log.
