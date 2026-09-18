# AxD

# ALL CREDITS BELONG TO DAGNAZTY. THIS IS NOT MY FIRMWARE. PLEASE SEE ORIGINAL AT https://github.com/dagnazty/awokxdag

### Battery Capacity Info: This version of AxD is currently built against 1.4.4, it includes battery settings & info that I personally needed. AxD Bridge is NOT updated for my changes.
### Please read documentation at [BattInfo.MD](BattInfo.md)

**Dual-band Wi-Fi / BLE penetration-testing toolkit for the ESP32-C5** (AWOK Dual
C5, white-USB screen board with an ILI9341 touchscreen).

- **Version:** 1.4.4 - custom 1.4.5.3
- **Author:** dag nazty - syzmonz custom settings.
- **Target:** ESP32-C5 Dev Module, 8 MB flash, PSRAM, microSD
- **Changelog:** [CHANGELOG.md](CHANGELOG.md)


> ## Authorized use only
> This firmware transmits and disrupts networks (deauthentication, beacon
> flooding, captive/evil-twin portals, targeted probe lures). Use it **only**
> against networks and devices you own or are explicitly contracted to test.
> Deauthentication, credential capture, and impersonation of a network are
> illegal against third parties without consent. **You are responsible for how
> you use this.**

---

## Features

### Recon (passive)
- **Wi-Fi Scan** — dual-band discovery for up to 64 APs: SSID, BSSID, RSSI,
  channel, band, and advertised auth mode, with Prev/Next paging after ten.
  Scans **continuously**, merging by BSSID so the list accumulates every AP seen
  (RSSI refreshed in place) until you select one or leave. Tap a result for a
  passive audit; **Track** graphs its RSSI; **Deauth** targets it; **Grab** jumps
  straight to handshake capture.
- **Channel Map** — 2.4 GHz and detected 5 GHz channel occupancy chart.
- **BLE Scan** — up to 64 advertisers with Prev/Next paging and tap-to-inspect detail (address
  type, TX power, connectable/scannable, manufacturer data, service UUIDs).
  Advertised Flipper service UUIDs get an **[F?]** hint and CSV identification field;
  the hint does not verify device identity.
- **Clients** — probe-request / station sniffer: client MACs, probed SSIDs,
  associated BSSIDs.
- **Packet Monitor** — promiscuous all-frame capture to pcap, hopping every
  channel, with a live management/data/control breakdown.
- **WPS Scan** — lists APs advertising WPS and whether setup is locked.
- **Hidden SSID** — reveals hidden network names from probe-response /
  association frames, with an optional deauth pulse.
- **Cameras** — continuous scanner that flags common surveillance cameras
  (Ring, Blink, Wyze, Nest, Arlo, Reolink, Eufy, Tapo, Hikvision, Dahua...) by
  vendor OUI (beaconing APs *and* connected clients), SSID/BLE name, and BLE
  manufacturer id.
- **Security Audit** — passive beacon-IE posture report: parses each AP's RSN /
  WPA / WPS information elements and classifies it by encryption tier (Open /
  WEP / WPA / WPA2 / WPA2-TKIP / WPA2-WPA3 / WPA3 / OWE / Enterprise), 802.11w
  PMF (none / optional / required) and WPS (open / locked), scores each network's
  risk, and sorts the weakest to the top. Listen-only.
- **BLE Trackers** — flags personal item trackers: Apple Find My / AirTags in the
  separated ("offline finding") state, Tile tags, and Samsung SmartTags. Tracks
  each address over time and raises a **FOLLOW** alert when one persists across a
  long enough span with repeat sightings — the planted-tracker privacy case.
  Passive.
- **Harvester** — all-channel passive EAPOL / PMKID collector. Hops every channel
  recording WPA key frames already in the air (plus one beacon per BSSID for the
  ESSID) to `harvest.pcap`, and writes hashcat-ready PMKID lines. **No deauth is
  sent** — the quiet counterpart to the targeted Grab.
- **Probe Intel** — aggregates directed probe requests by the SSID they name,
  ranked by probe count and distinct devices, revealing the preferred-network
  lists leaking from nearby devices. Passive.
- **Saved** — up to 10 access points kept in NVS across reboots.

### Network Tools (connected LAN)

Open **Recon → page 3 → Network Tools**. These tools send discovery/service
queries on the Wi-Fi network you join; they require a normal network connection.

- **Connect / Wi-Fi** — choose an AP from the last scan (or rescan), enter its
  password with the on-device character picker, then Join. Both Touch and Mini
  support SSID/password entry. Credentials stay in RAM; the password entry is
  masked and cleared after a connection attempt. Leaving Network Tools disconnects.
- **Discover Hosts** — ARP discovery with IP/MAC results and subnet-mask handling.
  Tap a host to inspect it individually. The top-level service buttons scan all
  discovered hosts.
- **TCP Ports** — checks 19 common TCP ports with one nonblocking connection at a
  time. Results establish an open TCP port, not a verified service or vulnerability.
- **LAN Cameras** — checks RTSP OPTIONS on 554/8554 and ONVIF device-information
  responses on HTTP 80/8000/8080/8899. Shows RTSP candidates, ONVIF manufacturer/model
  when returned, or ONVIF authentication requirements. Complements the existing
  passive Cameras tool; RTSP alone does not prove a device is a camera.
- **Printers** — checks raw-print (9100), IPP (631), and LPD (515) ports; labels
  responses as printer candidates. Does not submit print jobs.
- **SIP Services** — sends UDP OPTIONS on 5060 and matches responses to the queried
  host and request. Includes authentication/error responses as service evidence.
- **UPnP Mappings** (second page) — discovers a compatible gateway using SSDP,
  reads its IGD service description, and lists existing port mappings. It does
  not create, delete, or change mappings.
- **Results / exports** — paginated results with detail inspection, Back/Stop,
  and Save. Completed and cancelled scans automatically save to SD when available;
  CSVs retain partial-result flags, network context, and timeout/error counts.
  Serial **h** exits and disconnects.

Network Tools retain up to 128 hosts and 128 service results on C5, or 32 each on
original ESP32 boards. Subnets of at most 1,024 usable addresses are scanned in
full; larger subnets scan only the local /24 intersection and show a partial-scope
notice. IPv4 /31 and /32 networks are unsupported. ARP observes the local broadcast
domain; isolation, sleeping devices and packet loss can hide hosts. Timeouts are
inconclusive. UPnP accepts HTTP URLs using the gateway's literal IPv4 address,
limits responses to 8 KiB and mappings to 64 (32 on original ESP32), and reports
unsupported or oversized responses. HTTPS camera endpoints and SIP over TCP/TLS
are outside this first implementation. The character picker supports printable
ASCII (SSID up to 32 bytes, WPA password 8–63 characters or empty for open Wi-Fi);
enterprise authentication and raw 64-digit PSKs are not supported.

### Attacks (active — authorized targets only)
- **Deauth** — per-network from a Wi-Fi result: single AP, or multi-select
  several BSSIDs (a network's 2.4 GHz + 5 GHz radios) and round-robin across
  their channels/bands.
- **Handshake / PMKID capture** — from the Deauth screen (**Capture**) or a
  Wi-Fi result (**Grab**): locks the target channel, pulses deauth to force
  reconnection, records the WPA 4-way EAPOL handshake to pcap, and writes a
  hashcat-ready **PMKID** line when seen.
- **Beacon Flood** — broadcasts fake, clearly-synthetic test SSIDs.
- **Evil Portal** — open SoftAP + captive portal, logging submitted form fields.
- **Evil Twin** — clones the last-selected network's SSID as an open twin +
  portal.
- **Probe Lure (PineAP-lite)** — scoped to the last-selected SSID: beacons that
  one network and counts probe requests naming it, logging the probing client.

### Monitor (defensive)
- **Deauth Watch** — hops 2.4/5 GHz counting deauth/disassoc frames; logs the
  last offender.
- **Rogue Watch** — flags evil-twin APs: one SSID on multiple BSSIDs, or a saved
  SSID appearing on a new BSSID.
- **BLE Spam Watch** — flags BLE advertisement floods (Apple continuity, Swift
  Pair, Samsung, Fast Pair) by rate + vendor payload. Passive.
- **Karma Watch** — the inverse of Rogue Watch: flags a single BSSID that beacons
  or probe-responds for *many* different SSIDs — the signature of a WiFi
  Pineapple / Karma / MANA rogue AP answering every network a victim looks for.
  Passive.
- **Beacon Watch** — counts distinct BSSIDs beaconing per short window and alerts
  on a spike — beacon-flood / fake-AP spam (mdk4, or this firmware's own Beacon
  Flood). Passive.
- **Auth Flood** — detects authentication / association-request floods against an
  AP (mdk4 `a` / connection-flood DoS) and names the targeted AP. Passive.
- **Advanced Watch** — runs a shared Wi-Fi/BLE anomaly pipeline that monitors
  beacon/RSN/PMF/WPS integrity, saved-network security downgrades, grouped
  disconnect reason storms, channel-switch announcements, EAPOL and association
  spikes, RF noise-floor changes, and rapid BLE address churn. Alerts are
  thresholded and GPS logged. Passive; RF and BLE churn results are heuristics.

### GPS
- **GPS status** — fix, satellites, coordinates, speed, HDOP, plus a baud cycler
  and NMEA link diagnostics.
- **Wardrive** — logs each Wi-Fi BSSID and BLE device once to a WiGLE-compatible
  CSV while moving. A GPS-fix indicator sits in the header on every screen, and
  scan/deauth/client/portal/handshake logs are geotagged with the current fix.

### Link (two-unit)
- **Link Mode** — pairs two AxD units (any mix of C5 and original 2.4 GHz
  boards, Touch or Mini) over an ESP-NOW back-channel using a display-and-confirm
  4-digit code — no typing on either board. Powers **Split Wardrive**: the pair
  divides the channels so each unit scans a different part of the spectrum and the
  combined logs cover it all. Two matching units **alternate-deal** their shared
  plan (two C5s split the full dual-band list; two 2.4 GHz units split 2.4 GHz),
  while a **mixed C5 + 2.4 GHz pair splits by band** — the C5 takes all of 5 GHz
  and the 2.4 GHz unit takes all of 2.4 GHz, with no overlap. A one-second
  time-synced rendezvous on channel 1 swaps telemetry, so each screen shows its
  own, the partner's, and the combined AP count, the partner's link RSSI, and a
  partner-lost alert. Each board logs its own WiGLE `wardrive.csv` and uses its
  own GPS. Started unpaired, it wardrives every channel solo. Reached from the GPS
  screen; Wi-Fi-only in this release.

### Status / utility
- **Status** — uptime, free heap, chip temp, SD used/total, GPS fix, Wi-Fi MAC,
  battery (set `kBatteryAdc` in `board_pins.h` to enable).
- **Settings** (Status → Settings, serial `t`) — sleep timeout (off / 15s / 30s /
  1m / 2m / 5m), brightness, GPS baud, boot splash, two-tap confirm before
  active tests, NMEA echo, and screen test. Stored in NVS. Footer **Defaults**
  restores those prefs. A sleeping screen wakes on the first tap or button
  without triggering an action. Baud from the GPS screen or serial `u` is
  remembered across reboots.
- **Screen test** (Settings → Screen test) — full-bleed color bars, checkerboard,
  rotation marks, backlight sweep, and touch or five-button probe. Mini draws
  the panel directly, then again through the firmware canvas, so a dead ST7735
  can be told apart from a MiniLayout bug. Serial `h` aborts to Home.
- **Capture manager** (Status → Files) — browse `/awokxdag/` files with sizes;
  delete behind a two-tap confirm.

---

## Menu map

```
Home  page 1: Recon | Attacks | Monitor | GPS | Status      (footer: About >)
      page 2: About  (name, version, board, authorized-use notice)

Recon page 1: Wi-Fi Scan | Channel Map | BLE Scan | Clients | Packet Mon | WPS Scan
      page 2: Hidden SSID | Cameras | Security Audit | BLE Trackers | Harvester | Probe Intel
      page 3: Saved | Network Tools

Network Tools page 1: Connect / Wi-Fi | Discover Hosts | TCP Ports | LAN Cameras |
                      Printers | SIP Services
              page 2: UPnP Mappings | Last Results

Attacks:      Beacon Flood | Evil Portal | Evil Twin | Probe Lure
              (Deauth / Handshake launch from a scanned Wi-Fi result)

Monitor:      Deauth Watch | Rogue Watch | BLE Spam Watch | Karma Watch |
              Beacon Watch | Auth Flood | Advanced Watch

GPS:          status screen -> Baud / Drive / Link
Link:         unpaired -> Pair / Solo;  paired -> Unpair / Start (Split Wardrive)
Status:       health -> Home / Settings / Files
Settings:     Sleep | Bright | GPS | Splash | Active confirm | NMEA | Screen test
              footer: Back / Defaults
```

## Serial commands (115200 baud)

`w` Wi-Fi scan · `c` channel map · `b` BLE scan · `p` clients · `k` packet
monitor · `m` deauth watch · `g` GPS screen · `n` Link mode · `u` cycle GPS baud
· `r` toggle raw NMEA echo · `s` saved · `t` settings · `d` retry SD · `h` home
(also stops any running tool).

## SD-card output (`/awokxdag/`)

Insert a FAT32 microSD before boot. Saved networks live in NVS so the device
works without a card, and readable snapshots mirror to:

| File | Source |
| --- | --- |
| `firmware_audit.csv` | Rotating operational audit trail (boot, active tests, configuration and file changes) |
| `firmware_audit.previous.csv` | Previous audit segment after the live log reaches 256 KB |
| `saved_networks.csv` | saved AP list |
| `latest_wifi_scan.csv` | last Wi-Fi scan |
| `latest_ble_scan.csv` | last BLE scan |
| `latest_wifi_signal.csv` | signal monitor |
| `latest_lan_hosts.csv` | Last LAN host scan, including partial-scan status |
| `latest_network_services.csv` | Last ports/camera/printer/SIP/UPnP scan |
| `latest_clients.csv` | client sniffer |
| `latest_deauth_log.csv` | Deauth Watch |
| `rogue_log.csv` | Rogue Watch |
| `<ESSID>_<BSSID>.pcap` | WPA handshake capture; hidden SSIDs use `<BSSID>.pcap` (link type 105) |
| `latest_handshake_loc.csv` | Sidecar for each handshake capture: target, channel, EAPOL/PMKID flags, GPS |
| `pmkid.txt` | captured PMKID (hashcat-ready) |
| `portal_creds.csv` | evil portal / evil twin |
| `pktmon.pcap` | packet monitor |
| `wardrive.csv` | WiGLE 1.4 wardrive (Wi-Fi + BLE) |
| `security_audit.csv` | Security Audit posture report |
| `ble_trackers.csv` | BLE Trackers scan |
| `harvest.pcap` | Harvester capture (link type 105) |
| `harvest_pmkid.txt` | Harvester PMKIDs (hashcat-ready) |
| `probe_intel.csv` | Probe Intel SSID map |
| `karma_log.csv` | Karma Watch alerts |
| `beacon_flood_log.csv` | Beacon Watch flood windows |
| `auth_flood_log.csv` | Auth Flood alerts |
| `advanced_watch.csv` | Combined beacon, downgrade, disconnect, CSA, EAPOL, association, RF and BLE-churn alerts |

The camera and BLE-spam watches are live-view only.

The firmware audit trail includes a per-boot session id, firmware version, GPS
UTC time when available (otherwise uptime), result, non-secret details, and GPS
position. It intentionally does not copy portal submissions, packet payloads,
or credentials. The live segment rotates at 256 KB and retains one previous
segment.

---

## Build & flash (Arduino IDE)

1. Arduino IDE 2.x, board-manager URL
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`, install
   **esp32 by Espressif** 3.3.x. Then run `python3 scripts/setup_arduino_ide.py`
   (and again after any core update) so Verify gets `-Wl,-z,muldefs` and
   `-Wl,--wrap=esp_wifi_init -Wl,--wrap=esp_bt_controller_init`. Without muldefs the sketch's
   `ieee80211_raw_frame_sanity_check` collides with Espressif's copy in
   `libnet80211.a`. The wrap shrinks Arduino's default STA RX/TX buffers so
   Dual C5 Touch can keep BLE up beside Wi-Fi.
2. Libraries: **Adafruit GFX**, **Adafruit ILI9341**, **NimBLE-Arduino**,
   **XPT2046_Touchscreen**, **TinyGPSPlus**.
3. Open `AWOKxDAG.ino`, select **ESP32C5 Dev Module** with:
   - Flash Size **8 MB**, Partition Scheme **8M with spiffs (3MB APP)**,
     PSRAM **Enabled**, USB CDC On Boot **Disabled**.
4. Plug the USB cable into the port whose ESP32-C5 you want to flash (the AWOK
   Dual C5 has a white and an orange port, one chip each). The connected port
   enumerates as `/dev/ttyUSB*` (Linux), `/dev/cu.usbserial-*` (macOS), or
   `COM*` (Windows). It auto-resets into download mode; if a board does not,
   hold SCREEN BOOT while applying power, then release.

The sketch is currently configured for **Touch** in Arduino IDE
(`AWOK_DUAL_C5_TOUCH`). The packaging script selects the requested board
explicitly, independent of that default. To package the Touch profile:

```bash
python3 scripts/build_firmware.py dual-c5-touch
```

### Flash from the terminal (no IDE)

`scripts/flash_firmware.py` writes a packaged image to the board over the
CP2102 USB port with esptool — no Arduino IDE needed. It builds the image
first if it is missing.

```bash
python3 scripts/flash_firmware.py dual-c5-touch            # autodetect the port
python3 scripts/flash_firmware.py dual-c5-touch --port /dev/ttyUSB0
python3 scripts/flash_firmware.py dual-c5-touch --build    # (re)build, then flash
```

The port is autodetected when only one is present; pass `--port` if several
serial devices are attached. Any board profile `build_firmware.py` accepts
works here too.

### Phone control over BLE (both chips)

The Dual C5 has two ESP32-C5 chips, and **both now run the full firmware**. Flash
the **screen** chip (white port) with `dual-c5-touch` **or** `dual-c5-mini`, and
the **bridge** chip (orange port) with `dual-c5-bridge` — the *same* firmware
built headless (no screen) with a BLE GATT server folded in:

```bash
python3 scripts/flash_firmware.py dual-c5-touch    # (or dual-c5-mini) white port
python3 scripts/flash_firmware.py dual-c5-bridge   # cable in the orange port
```

The bridge advertises as **`AxD-Bridge`** over BLE. Open the control page —
`https://dagnazty.github.io/awokxdag/control.html` (or the local
`website/public/control.html`) — in **Bluefy** on iOS or **Chrome** on Android
(Safari has no Web Bluetooth), tap **Connect**, then pick a **Target chip**:

- **This bridge** — the orange chip runs the tool *locally* (Wi-Fi/BLE scan,
  monitors, attacks). GPS-wardrive and Files/SD are screen-chip only for now.
- **Screen chip** — the command is relayed over ESP-NOW to the white chip, which
  runs it.

Either way the network list, per-AP actions (Deauth / Grab / Track / Evil Twin /
Probe Lure) and live counts stream back to the phone, tagged with the chip that
produced them. The bridge sweeps every channel when relaying so a command reaches
the screen chip even while it is hopping. The two chips talk over ESP-NOW; the shared wire format is
`AWOKxDAG/link_protocol.h`.

### Dual C5 Mini

Install **Adafruit ST7735 and ST7789 Library** in addition to the libraries above,
then build and package the Mini profile:

```bash
python3 scripts/build_firmware.py dual-c5-mini
```

Outputs are in `build/dual-c5-mini-1.4.4/`, with explicit board names and
`SHA256SUMS`. This command only compiles and packages; it does not flash.
The Mini uses a native 128 × 128 layout with readable text, highlighted menu
rows, wrapped details, and compact charts. Up/down moves through rows, center
selects, right jumps to actions, and left returns to the top of the screen. At
boot it shows the same AWOK logo as the Touch board, downscaled to 96 × 128 by
`scripts/gen_mini_boot.py` (re-run with `--threshold` to retune the 1-bit art).
Use `dual-c5-touch` with the same script to package the default Touch build.

### Original ESP32 boards (2.4 GHz)

The original **Dual ESP32 Touch v1/v2/v3** and **Dual ESP32 Mini v1/v2/v3**
profiles target the classic **ESP32 Dev Module** (4 MB flash, `huge_app`
partition, PSRAM disabled) and add the **Adafruit ST7735 and ST7789 Library** for
the Mini panels. Package any of them with the same script:

```bash
python3 scripts/build_firmware.py dual-esp32-touch-v1   # or -v2 / -v3
python3 scripts/build_firmware.py dual-esp32-mini-v1    # or -v2 / -v3
```

## Hardware map (Dual C5 Touch)

| Function | GPIO |
| --- | ---: |
| SPI SCK / MISO / MOSI | 6 / 2 / 7 |
| ILI9341 CS / DC / Reset | 23 / 24 / (none) |
| Backlight | 8 (active high) |
| XPT2046 touch CS | 3 |
| SD card CS | 10 |
| GPS UART1 RX / TX | 14 / 13 @ 115200 NMEA |
| Battery ADC | unset (`kBatteryAdc = -1`) |

Pins and touch calibration live in `board_pins.h`.

## Hardware map (Dual C5 Mini)

| Function | GPIO |
| --- | ---: |
| SPI SCK / MISO / MOSI | 6 / 2 / 7 |
| ST7735 CS / DC / Reset | 23 / 24 / (none) |
| Backlight | 5 (active low) |
| Buttons Left / Center / Up / Right / Down | 0 / 1 / 4 / 8 / 9 |
| SD card CS | 10 |
| GPS UART1 RX / TX | 14 / 13 @ 115200 NMEA |
| Battery ADC | unset (`kBatteryAdc = -1`) |

The Mini shares the SPI bus, display CS/DC, SD, and GPS wiring with the Touch
board; it swaps the ILI9341 + XPT2046 touchscreen for a 128 × 128 ST7735 driven
by five buttons, and its backlight is active-low on GPIO 5. Selected by building
with `AWOK_DUAL_C5_MINI` (via `scripts/build_firmware.py dual-c5-mini`). This
mapping was recovered from the bundled Mini firmware (see the comments in
`board_pins.h`) and is confirmed working on hardware.

## Hardware map (original ESP32 Touch v1/v2/v3)

| Function | GPIO |
| --- | ---: |
| SPI SCK / MISO / MOSI | 18 / 19 / 23 |
| ILI9341 CS / DC / Reset | 17 / 16 / 5 |
| Backlight | 32 (active high) |
| XPT2046 touch CS | 21 |
| SD card CS | 12 (v1) · 14 (v2, v3) |
| GPS UART2 RX / TX | 4 / 13 @ 115200 NMEA (selectable) |
| Battery ADC | unset (`kBatteryAdc = -1`) |

## Hardware map (original ESP32 Mini v1/v2/v3)

| Function | GPIO |
| --- | ---: |
| SPI SCK / MISO / MOSI | 18 / 19 / 23 |
| ST7735 CS / DC / Reset | 17 / 16 / 5 |
| Backlight | 32 (active low) |
| Buttons Left / Center / Up / Right / Down | 13 / 34 / 36 / 39 / 35 |
| SD card CS | 4 |
| GPS UART2 RX / TX | 21 / 22 @ 9600 NMEA (selectable) |
| Battery ADC | unset (`kBatteryAdc = -1`) |

These original 2.4 GHz-only ESP32 profiles share the classic display bus and are
selected with `AWOK_DUAL_ESP32_TOUCH_V<n>` / `AWOK_DUAL_ESP32_MINI_V<n>`. On the
Mini, GPIO34–39 are **input-only with no internal pull-ups** (Center / Up / Right
/ Down rely on the board's external biasing; Left on GPIO13 uses `INPUT_PULLUP`).
Pin sources and validation status: [original Touch](docs/dual-esp32-touch.md),
[original Mini](docs/dual-esp32-mini.md). Link pairing: [Link Mode](docs/link-mode.md).

## Collection capacity

The capacities below describe the C5 profiles. Original ESP32 Touch profiles
retain 32 results per table and 128 Wardrive deduplication addresses; combined
views use Wi-Fi only. See [original Touch](docs/dual-esp32-touch.md) and
[original Mini](docs/dual-esp32-mini.md).

Wi-Fi and BLE scans retain up to **64 results** each (dual-band C5). BLE results use ten rows
per page, with Prev/Next controls and detail inspection on every page.
Clients, Security Audit, BLE Trackers, Cameras, WPS, Hidden SSID, Harvester,
Probe Intel, and Karma Watch each retain up to **64 entries** (Probe Intel counts
SSIDs). Live summary screens keep their existing row limits; available CSV
exports include the full collected table. Continuous BLE scans use callbacks
without retaining a NimBLE result list, so the regular scan's 64-result cap does
not stop their incoming observations. Their feature tables remain bounded.

On Mini, the Wi-Fi, BLE, Clients, Probe Intel, and Karma Watch result tables
are allocated in PSRAM at boot, preserving their 64-entry capacities and moving
**47 KiB** of table storage out of internal RAM. Strings are constructed normally;
radio buffers and callback queues remain internal. A table falls back to internal
RAM if its PSRAM allocation fails; failure of both allocations stops startup.
Touch keeps its static result tables.

Mini Wardrive, Cameras, and Advanced Watch now attempt Wi-Fi + BLE when at least
32 KiB of tables were placed in PSRAM and, after stopping previous radios, at
least 110 KiB of internal RAM is free with a 36 KiB contiguous block. BLE starts
first, then Wi-Fi. If the budget check, radio initialization, or BLE scan start
fails, the view retains Wi-Fi-only operation and displays BLE off. These memory
thresholds are estimates; simultaneous scanning and repeated tool switching still
need hardware validation under load. See Serial Monitor for table placement and
internal free/largest-block readings before and after radio initialization.

## Source layout

Single Arduino sketch split into feature tabs (one translation unit):
`AWOKxDAG.ino` (globals, UI, scanning, menus, setup/loop) + `awok_common.h`
(types/enums/constants) + `board_pins.h`, and per-feature tabs: `gps`,
`deauth`, `handshake`, `sniffer`, `beacon`, `portal`, `wardrive` (in gps),
`pktmon`, `cameras`, `wps`, `hidden`, `roguewatch`, `bledetect`, `probelure`,
`securityaudit`, `settings`, `screentest`, `tracker`, `harvester`, `probeintel`, `karmawatch`,
`beaconwatch`, `authflood`, `advancedwatch`, `locator`, `link` (Link Mode +
Split Wardrive; [docs/link-mode.md](docs/link-mode.md)), `auditlog`, `status`,
`files`, `input`, `networktools`. `network_parse.h` contains bounded
network-response parsers shared with the host tests in `tests/host/` (run
`bash tests/host/run.sh`).

## Recovery

Flashing replaces the app on whichever ESP32-C5 the USB cable is plugged into. Keep an official
Marauder `_v8.bin` and its C5 bootloader/partition files so factory firmware
can be restored.

## Credits & license

AxD is original firmware, but it stands on prior work and would not exist
without it. Thanks to:

- **[Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project)** by
  **7h30th3r0n3** — thanks for the work behind the LAN host/port scanning,
  CCTV, printer, SIP OPTIONS, UPnP mapping, and Wall of Flippers features used
  as references for our Network Tools and BLE identification hints. We adapted
  those ideas to AxD's radio lifecycle, bounded scan engine, and Touch/Mini
  controls. The linked Evil-Cardputer source carries an MIT notice; some sections
  credit other projects, including Bruce. See [third-party notices](THIRD_PARTY_NOTICES.md).
- **[ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder)** by
  **justcallmekoko (Justin Hazard)** — the reference for the original ESP32
  board pin maps, display/SPI setup and touch-calibration constants (see
  [dual-esp32-touch.md](docs/dual-esp32-touch.md) and
  [dual-esp32-mini.md](docs/dual-esp32-mini.md) for exact upstream sources),
  the AWOK white-port board compatibility mapping, and the radio-lifecycle
  approach that `shutdownWiFi()` / `shutdownBLE()` follow.
- **[FZEasyMarauderFlash](https://github.com/SkeletonMan03/FZEasyMarauderFlash)**
  by **SkeletonMan03** — flashing reference used for the original Mini pin
  sourcing.
- The libraries this firmware builds on: **Adafruit GFX**, **Adafruit ILI9341**,
  **Adafruit ST7735 and ST7789**, **Adafruit BusIO**, **NimBLE-Arduino**,
  **XPT2046_Touchscreen**, and **TinyGPSPlus** — each under its own license.

**License:** AxD's own code is released under the **MIT License** (see
[LICENSE](LICENSE)). Hardware pin numbers and calibration constants are factual
board-interface values, and the radio-lifecycle behavior was reimplemented rather
than copied. ESP32 Marauder is licensed **GPL-3.0** and each referenced project
and library remains under its own license — consult those upstreams for their
terms before redistributing derived work.
