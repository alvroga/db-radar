# Radar User Manual

*Powered by DRAC OS — the open-source firmware running on this radar.*

Everything you need to actually use the device day to day — modes, settings, waypoints, hints,
BLE beacon tracking, and where the project is headed. For flashing and the technical architecture,
see the [README](../README.md) and [CLAUDE.md](../CLAUDE.md); for wiring, see
[Assembly Instructions](assembly.md).

**Assembly photos and the beacon-proximity GIF referenced below live in
[`assets/manual/`](../assets/manual/) — drop the actual image/GIF files there; this doc references
them by filename so they show up once added.**

---

## The Radar Screen

Your position is a red triangle at the center of the screen. Every loaded waypoint is a yellow dot,
placed by real GPS distance and bearing. Five zoom levels — 1km, 500m, 200m, 100m, and a 50m
precision mode — control how far out the radar reaches; cycle through them with the physical button
(see [Controls](#controls) below).

Waypoints outside the current zoom radius don't disappear — they collapse into edge arrows, one per
compass sector (up to 8 at once), so a GPX file with a hundred pins in it stays readable instead of
turning into a wall of dots. Tap an edge arrow the same way you'd tap an on-screen waypoint.

---

## Navigation Modes

Two ways to read the radar, switchable any time in **Settings > Display > Navigation Mode**:

- **Heading-Up** (default) — the whole radar rotates as you turn, so "up" on the screen always means
  "the direction you're currently facing." This is the compass talking, not GPS — the heading source
  is the onboard magnetometer, read continuously, so it works standing still (no need to be moving
  for the radar to orient correctly).
- **North-Up** — the radar stays in a fixed orientation; north is always up on the screen, regardless
  of which way you're facing.

Your choice is saved and reapplied on every boot.

---

## Controls

| Action | Gesture |
|---|---|
| Fix on a waypoint | Tap it on the radar |
| Change zoom level | Single-press the physical button (in), double-press (out) |
| Open Settings | Long-press the physical button (2 seconds) |
| Enter standby / sleep | Extra-long-press the physical button (4 seconds) |
| Wake from standby | Any button press |
| Catch a found beacon | Tap the "ball" once it appears on-screen (see Beacon Tracking below) |

---

## Waypoints

### Loading a GPX file

The radar reads standard GPX waypoint files — the same format every GPS device and geocaching app
uses. Power the device on, connect to its WiFi (access point or your home network, see
[WiFi & GPX Management](#wifi--gpx-management) below), open the web portal in a browser, and upload
a `.gpx` file. Waypoints appear on the radar immediately, no reboot needed.

### Creating your own waypoints and hints

Don't have a GPX file yet? Two options:

- **[GPX Generator](https://alvroga.github.io/db-radar/gpx-generator/)** — a simple web tool for building a
  waypoint file from scratch, including hint text per waypoint.
- **Any existing geocaching/GPX tool** — the radar reads the standard `<groundspeak:encoded_hints>`
  field automatically, so a file exported from another geocaching app already carries its hints over.

A hint is just text attached to a waypoint — a clue for whoever finds that coordinate. Tap a fixed
waypoint's detail screen to read its hint, if it has one.

### Fixing on a waypoint

Tap any waypoint (on-screen or an edge arrow) to fix on it. Fixing gives you two things a regular
on-screen dot doesn't:

- A **continuously-updating live distance readout**, refreshed as you move.
- A **proximity sonar** — the buzzer's tempo speeds up continuously as you get closer. It's a smooth
  ramp tied to actual distance, not discrete "near/far" zones, so the feedback is genuinely useful
  for the last few meters.

Fixing auto-releases if you end up more than 100km from the target — a safety net so you don't stay
locked onto something you've clearly given up on.

### Sharing waypoints — send someone on an adventure

A GPX file is just a file. Build one (with hints) using the GPX Generator, then send it to a friend
however you'd send any file — message, email, USB. They upload it to their own radar the
same way you would, and now they're hunting for whatever you hid.

**Quests are coming** — a way to tag a whole set of waypoints as one adventure, with progress
tracking across the set and a small collectible badge for finishing it. The design is done; the
feature itself isn't built yet. See [ROADMAP.md](../ROADMAP.md) for where it stands.

---

## BLE Beacon Tracking — Catch Your Own "DRAC Ball"

Separate from GPS waypoints entirely: point the radar at a specific Bluetooth device by its MAC
address, and it becomes a live proximity target — no coordinates involved, pure signal strength.

Set it up in **Settings > Beacon**: enter the target MAC address. Once it's in range, switch to the
50m zoom level to see it — an arc gauge and a continuous buzzer tone climb as you get closer. Get
close enough and a "ball" appears on-screen at the center; tap it to confirm you've found it (the
Settings > Beacon tab also shows a Found/Missing status you can reset from there).

Tag anything with a known MAC — a keychain, a bag, a Bluetooth beacon you planted somewhere. Hand it
to someone and let them hunt for it with nothing but the radar.

![Beacon proximity in action](../assets/manual/beacon-proximity.gif)

---

## WiFi & GPX Management

Two WiFi modes, switchable in **Settings > WiFi**:

- **Access Point mode** — the device hosts its own WiFi network (default SSID `Radar-GPX`, default
  password `radar123`). Connect your phone or laptop directly to it, no router or home network
  required. The web portal is at `http://192.168.4.1`.
- **Station (STA) mode** — the device joins your home WiFi instead. The web portal is then reachable
  at the device's IP address, shown right there in the Settings screen.

The web portal is where you upload, browse, and delete `.gpx` files, and (once the device is on your
network) where firmware updates over WiFi happen too — see the README's Installation section for the
OTA update flow.

---

## Settings Reference

Settings are organized into tabs, reached via **long-press the physical button (2s)** from the radar
screen.

### GPS

Restart modes (Hot/Warm/Cold) and a Factory Reset for the GPS module, for troubleshooting a fix
that's stuck or wrong. GPX waypoint count and a manual refresh button live here too.

### WiFi

Access Point vs. Station mode toggle, AP network name/password, and connection status. Changing WiFi
mode requires a reboot to apply — the Settings screen prompts for it.

### Display

- **Brightness** — screen backlight level
- **Navigation Mode** — Heading-Up / North-Up (see [Navigation Modes](#navigation-modes) above)
- **N Indicator** — show/hide the north-pointing triangle in Heading-Up mode
- **Daylight Mode** — high-contrast black-on-white theme for direct sun, vs. the default green
  radar theme for normal/indoor use
- **HUD Auto-Hide** — automatically hide the heads-up display labels after a configurable delay,
  for an uncluttered view
- **Auto Sleep** — enter standby automatically after a period of no touch/button activity, or never
- **Calibrate Compass** — walk through the calibration routine if the compass heading feels off after
  moving the device to a new environment (opening the enclosure, being near strong magnets, etc.)

### Sound

- **All Sounds** — master on/off, controls both of the below together
- **Proximity Sound** — the beacon/waypoint sonar tone specifically
- **Button Sound** — audible feedback on physical button presses
- **Test Beep** — confirm the buzzer works without waiting for a real proximity event

### Beacon

Target MAC address (edit via the on-screen dialog), and a Found/Missing status toggle you can reset
manually once you've retrieved whatever you were tracking. See
[BLE Beacon Tracking](#ble-beacon-tracking--catch-your-own-drac-ball) above for how it works.

---

## Building One

For the full bill of materials and enclosure/print settings, see the
[README's Bill of Materials and 3D Enclosure sections](../README.md#3d-enclosure) (full README content
ships once the project is out of "coming soon" mode — see `docs/private/public_release_plan.md`); for
wiring diagrams, see [Assembly Instructions](assembly.md) (in progress). What follows is the
assembly sequence itself, photo by photo — see
[`assets/manual/README.md`](../assets/manual/README.md) for the exact shot list and file naming;
each step below already names the file it's waiting on.

1. **Print the enclosure.** PLA or PETG, 0.2mm layer height, 20% infill (see
   [3D Enclosure](../README.md#3d-enclosure)). Check fit before installing anything —
   `01-printed-parts.jpg`.
2. **Wire the GPS/compass module** to the board per [Assembly Instructions](assembly.md) —
   `02-wiring-module.jpg`.
3. **Wire the boost converter** between the board's 3.3V rail and the module's power pins, per
   [Assembly Instructions](assembly.md) — `03-wiring-boost-converter.jpg`.
4. **Seat the board in the enclosure**, USB-C and button aligned with their cutouts —
   `04-board-in-enclosure.jpg`.
5. **Mount the LiPo** in its recess underneath the board, connector reachable —
   `05-battery-mount.jpg`.
6. **Route and dress the wiring** so nothing pinches when the case closes —
   `06-wiring-routed.jpg`.
7. **Close the enclosure** with the M2 screws, then secure any internal components with the M1.4
   screws — `07-case-closed.jpg`.
8. **First power-on.** Flash the firmware (see the [README's Installation section](../README.md#installation))
   before or after closing the case — whichever makes probing wiring easier if something's wrong —
   `08-first-boot.jpg`.

Don't have the photos yet? The steps above stand on their own — the file names are just where the
pictures will slot in once taken.

---

## Troubleshooting

Common issues and fixes: [`docs/troubleshooting.md`](troubleshooting.md).

---

## Developer Mode

A hidden, serial-only mode aimed at people debugging their own build — it isn't needed for normal
use, and there's no touchscreen switch for it (`dev on` over serial is the only way in).

Turning it on (`dev on`) enables:

- **The DEV tab** in Settings — SD-card logging toggle + status, NTP sync status, a live
  render-performance readout, and buttons into two bench-testing screens (Field Log, Tilt Bench)
  used while developing the compass tilt-compensation feature.
- **SD-card system logging** (`system_logger`) — diagnostic logs written to the physical SD card,
  separate from the GPX files on FFat.
- **The `/logs` web management page** — 404s when dev mode is off.
- **A battery-voltage debug readout** on-screen, in addition to the normal charge icon.

Turn it off again with `dev off` — this also tears down the Field Log writer task cleanly (a real
bug, FT-08, once left it running in the background for the rest of the session).

Full command list: [`docs/serial_commands.md`](serial_commands.md).

---

## Going Deeper

This manual covers day-to-day use. For architecture, every subsystem in technical detail, and the
full serial diagnostic command reference, see [CLAUDE.md](../CLAUDE.md), the complete
[Serial Command Reference](serial_commands.md), and the linked component docs in [`docs/`](.).
