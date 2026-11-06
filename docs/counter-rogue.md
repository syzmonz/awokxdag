# Counter-Rogue

Counter-Rogue is an active defence add-on to **Rogue Watch**. Every other
Watch tool only alerts. Counter-Rogue acts: when Rogue Watch sees an evil
twin of one of **your own saved networks**, it deauthenticates the rogue
AP's clients off it, so devices stop associating to the impostor.

It is deliberately scoped as tight as the law allows: it fires **only** on
an AP that is broadcasting the SSID of a network you have **saved**, from a
**different BSSID** than the one you saved. That is the classic evil-twin /
Wi-Fi-pineapple case against a network you control. It never touches a
network you have not saved.

## What counts as a target

Counter-Rogue fires on an AP when **both** are true:

1. The AP's SSID exactly matches an SSID in your **Saved** list.
2. The AP's BSSID is different from the BSSID you saved for that SSID.

That is the `saved-twin` tag you see in red on the Rogue Watch list. An AP
that is merely on two BSSIDs but is not one of your saved networks shows as
`multi-BSSID` and is **not** a Counter-Rogue target; it is only an alert.

## Setup

1. **Save the network you want to protect.** Scan Wi-Fi, open your real
   home/office AP, and tap **Save**. Counter-Rogue uses the saved SSID +
   BSSID as the "this is mine" reference. Without a saved network it has
   nothing to defend and will never fire.
2. Open **Rogue Watch** from the Recon menu and let it run. The footer is
   `Stop | Arm | Home`.
3. Tap **Arm**. You will get the active-test confirmation prompt
   ("Counter-rogue") because this transmits. Confirm it.
4. The status line turns red: `COUNTER-ROGUE | twin-only | N fired`. The
   middle button now reads **ARMED**. Tap it again to disarm.

## Operating

- Counter-Rogue only acts while Rogue Watch is **running** and **armed**.
  Leaving Rogue Watch disarms it automatically.
- When a saved-twin is seen, it sends 3 broadcast deauth frames addressed
  from the rogue's BSSID, on the rogue's own channel, so only that
  impostor's clients are affected.
- It is rate-limited to once per 1.5 s per target BSSID, so it nudges
  rather than floods.
- Every fire is logged to the SD card at `/awokxdag/counter_log.csv` as
  `uptime_s,rogue_bssid,ssid` for your records.

## Limitations and safety

- **Only use this on networks you own or are authorised to defend.**
  Transmitting deauth frames at an AP you do not control is illegal in most
  jurisdictions. The saved-twin scope is there to keep you on the right
  side of that line; do not work around it.
- It defends by kicking clients off the **rogue**. It cannot force clients
  back onto your real AP; they re-associate on their own.
- It needs the rogue to be on a channel the watch is currently sweeping to
  catch it; a rogue that appears only briefly may be missed.
- Disarm (tap **ARMED**, or leave Rogue Watch) when you are done. It does
  not persist across a reboot; it always starts disarmed.
