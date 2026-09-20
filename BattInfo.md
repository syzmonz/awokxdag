# Battery Info

This build adds a battery readout to the AWOK Dual C5 Touch on a battery backpack. **<ins>It has been built for the battery backpack only.<ins>**  This page explains
what it does, how to use it, and what it can and cannot do.

## What it is

The battery backpack only feeds power to the board over 5V and GND. It does not
report the cell to the chip, so there is no real voltage to read and no true state
of charge to measure.

Instead, this build estimates the battery in firmware. It counts how much charge
has been used over time and subtracts that from the LiPo mAh you set. This is a
guess, not a measurement. It is useful for a rough idea of how full you are, not
for an exact number.

## How it works

Every loop the firmware adds up the charge used, based on what the device is
doing:

- Idle: low power draw
- Scanning: higher power draw
- Transmitting: highest draw

Backlight brightness and a small end of life sag factor are also included. The
running total is stored as milliamp hours used, and the percentage is
(capacity - used) / capacity.

The percentage shows on the Status screen next to Power and is counted in 5% stages.

## Settings

Both live on page 1 of Settings, and there is added page navigation buttons to access the other settings. (use the arrow keys to reach it):

- **Batt** sets your battery size in mAh. Tap to change between 500mAh up to 5000mAh.
  Set this to your actual battery size, for example 2000mAh.
- **Batt Tune** is a calibration dial from 50% to 150%, default 100%. It scales
  the current estimate up or down. Use it after a test run (see Calibration).

Changing Batt resets the running count, because the old count no longer matches
the new selected battery size. Changing Batt Tune does not reset it.

## Reset the gauge

Tap the Power row on the Status screen to reset the estimate to full. Do this
right after you charge the battery, since the firmware & hardware cannot detect a charge on its
own.

## Low battery

At 10% the device does two things:

- Shows a "LOW BATTERY" banner for 5 seconds
- Stops any active transmit tool (deauth, beacon, portal, lure) so a brownout
  does not corrupt an open SD file. Passive scans are not stopped. You may continue to transmit after the autostop, its not disabled.

The Power row also turns yellow near 20% and red near 10%. i like visual indicators

## Saving

The estimate saves to flash about once a minute and reloads on boot, so a reboot
keeps your reading. It does not survive a reflash, because flashing wipes the
saved data.

## Fleet battery

When a fleet is running, each node sends its battery percent to the coordinator
with its wardrive rows. The coordinator shows every node's battery in the fleet
list, and the phone app shows it too. This lets you watch the whole fleet's
battery from one place.

## Calibration

The three current figures (idle, scan, transmit) are estimates, not measured on
your board, so the number can be off. To dial it in:

1. Fully charge the battery and tap the power row in status to reset to 100%.
2. Set Batt to your real battery size.
3. Run the device on one steady task until it dies, and time it.
4. If it died sooner than the gauge expected, raise Batt Tune. If it lasted
   longer, lower Batt Tune.

Repeat until the gauge roughly matches real runtime. One idle run is enough for a
good baseline.

## Limits

- It is an estimate. Treat it as a guide, not an exact charge %.
- It assumes the battery was full at the last reset.
- It cannot detect charging, so reset it by hand after a charge.
- It drifts over a long session and with cell age.
- Re-flashing firmware sets the power back to 100%

The only way to get a true percent would be a fuel gauge chip wired to the cell,
which this hardware does not have.
