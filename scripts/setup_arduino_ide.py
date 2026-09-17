#!/usr/bin/env python3
"""Install the Arduino IDE flags the Verify button does not pass.

The sketch overrides ieee80211_raw_frame_sanity_check so authorized deauth
frames can be injected. Espressif's libnet80211.a also defines that symbol.
arduino-cli/CI pass -Wl,-z,muldefs; Arduino IDE 2 does not, so Verify fails
with a multiple-definition error unless this flag is in platform.local.txt.

--wrap=esp_wifi_init routes Arduino's wifiLowLevelInit through
wifi_init_wrap.c to trim STA buffers + CSI/AMPDU; --wrap=esp_bt_controller_init
trims the BLE controller's duplicate lists.

AWOKxDAG is BLE Observer-only (scan; no client/server/adv anywhere in the
sketch), so the CONFIG_BT_NIMBLE_ROLE_*_DISABLED flags trim NimBLE-Arduino's
default Central/Peripheral/Broadcaster footprint. The actual fix for BLE scan
failing with HCI "Memory Capacity Exceeded" (NimBLE rc=519) on the C5 is the
time-multiplex RadioScheduler in the firmware (only one radio DMA-resident at
a time); these flags just keep the footprint lean.

(NimBLE buffer-count and MEM_ALLOC_MODE macros are NOT overridable by a
compiler define: the esp32 core's own generated sdkconfig.h #defines them
unconditionally and silently wins. Don't add flags for those; they compile
without warnings but have no effect.)

build.code_debug=1 is Error-level CORE_DEBUG_LEVEL: silent unless something
is wrong, but it's what surfaces NimBLE's own error rc codes, which are
compiled out entirely at the default level 0.
"""
from pathlib import Path
import os

LINKER_FLAGS = ("-Wl,-z,muldefs", "-Wl,--wrap=esp_wifi_init",
                "-Wl,--wrap=esp_bt_controller_init")
NIMBLE_FLAGS = (
    "-DCONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED",
    "-DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED",
    "-DCONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED",
)
PROPERTIES = {
    "compiler.c.elf.extra_flags": LINKER_FLAGS,
    "compiler.cpp.extra_flags": NIMBLE_FLAGS,
    "build.code_debug": ("1",),
}
PACKAGES = Path.home() / ".arduino15/packages/esp32/hardware/esp32"


def cores():
    if not PACKAGES.is_dir():
        raise SystemExit(f"No ESP32 Arduino core under {PACKAGES}")
    found = sorted(p for p in PACKAGES.iterdir() if (p / "platform.txt").is_file())
    if not found:
        raise SystemExit(f"No platform.txt under {PACKAGES}")
    return found


def ensure_local(core: Path) -> str:
    path = core / "platform.local.txt"
    text = path.read_text() if path.is_file() else ""
    lines = text.splitlines()
    remaining = dict(PROPERTIES)
    changed = False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("#") or "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        key = key.strip()
        if key not in remaining:
            continue
        flags = value.split()
        missing = [flag for flag in remaining[key] if flag not in flags]
        del remaining[key]
        if not missing:
            continue
        flags.extend(missing)
        lines[i] = f"{key}={' '.join(flags)}"
        changed = True
    if not remaining:
        if changed:
            path.write_text("\n".join(lines) + "\n")
            return f"updated {path}"
        return f"unchanged {path}"
    out = "\n".join(lines)
    if out and not out.endswith("\n"):
        out += "\n"
    out += (
        "# AWOKxDAG: ieee80211_raw_frame_sanity_check must win over "
        "libnet80211.a; wrap esp_wifi_init/esp_bt_controller_init trims the "
        "radios; NimBLE compiled Observer-only\n"
    )
    for key, flags in remaining.items():
        out += f"{key}={' '.join(flags)}\n"
    path.write_text(out)
    return f"wrote {path}"


def main():
    os.umask(0o022)
    for core in cores():
        print(ensure_local(core))
    print("Restart Arduino IDE if it is already open, then Verify again.")


if __name__ == "__main__":
    main()
