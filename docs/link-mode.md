# Link Mode

Two AxD units pair over ESP-NOW and can run **Split Wardrive**: they
divide the channel list so each board scans a different part of the spectrum.

Reached from **GPS → Link**, or serial `n`. Wi-Fi-only in this release; BLE is
kept off so ESP-NOW is not torn down on Mini boards.

## Pairing

1. Open Link on both units.
2. Each board broadcasts a HELLO on channel 1. Discovery is plaintext; ESP-NOW
   cannot encrypt broadcast.
3. Both screens show the same 4-digit confirm code, derived from the unordered
   pair of MAC addresses. Nothing is typed.
4. Tap **Confirm** on each unit. The lower MAC becomes master and owns the
   session id and clock.
5. After pairing, SYNC/TELEM move to an encrypted unicast peer (PMK + per-pair
   LMK baked into the firmware). Anyone with the binary can derive those keys;
   encryption only stops casual sniffing.

Cancel pairing from the Link screen, or serial **h** from Home, to stop HELLO
broadcasts. Unpair from the paired idle screen. Serial **h** does not unpair a
finished session.

## Protocol

Every frame is the `LinkPacket` POD in `awok_common.h` (36 bytes on every
supported ABI). Magic `AWKL`, **protocol version 2** on all boards. Version 1
C5 units from 1.2.0 will not pair with 1.3.0+; reflash both sides.

HELLO flags advertise dual-band capability (`kLinkFlagDualBand`). Band
capability is *not* encoded in the version number; 1.3.0 made the version
uniform so a C5 and a classic 2.4 GHz board can pair.

## Split Wardrive

- Two matching units **alternate-deal** the shared plan (even/odd indices).
  Two C5s split the full dual-band list; two classic boards split 2.4 GHz.
- A mixed C5 + 2.4 GHz pair **splits by band**: the C5 takes all of 5 GHz, the
  classic board takes all of 2.4 GHz, with no overlap.
- A one-second rendezvous on channel 1 exchanges telemetry. Each screen shows
  its own AP count, the partner's, the combined count, partner link RSSI, and a
  partner-lost alert.
- Each board writes its own WiGLE `wardrive.csv` and geotags from its own GPS.
- Started unpaired, Link Mode wardrives every assigned channel solo.

See `AWOKxDAG/link.ino` for the state machine.
