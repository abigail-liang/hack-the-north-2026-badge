# Hack the North 2026 — Badge Finder

Custom ESP-IDF firmware for the Hack the North 2026 hacker badge that turns it
into a **person finder**: point it at another badge and it tells you how far
away they are and which way to walk.

Built during the event, replacing the stock firmware.

## Why custom firmware

The stock badge runs a Lua sandbox that exposes a deliberately restricted BLE
broadcast channel — 44-byte payloads, no WiFi, no ranging. Everything this
project depends on lives outside that sandbox:

| | Stock Lua app | This firmware |
|---|---|---|
| WiFi | not available | full scan + SoftAP |
| ESP-NOW | not available | 50 Hz peer ranging |
| 802.11mc FTM | not available | time-of-flight distance |
| BLE | 44 B broadcast, filtered | raw advertise + scan |

## What it does

- **Finds a peer badge.** Each badge beacons as `HTN-FIND-XXXX` and scans for
  others, so either can find either — no master/slave setup.
- **Distance** via 802.11mc Fine Timing Measurement, with an RSSI path-loss
  fallback for targets that do not support ranging.
- **Direction** from a guided 360° scan. The badge is held against your chest;
  your body shadows the signal, and the resulting angular pattern is fitted to
  recover a bearing.
- **Live compass** that keeps updating as you walk, without any compass
  hardware — it infers your angle off-target from how fast the range closes.
- **Warmer / colder** feedback and an LED proximity ring.
- Works on any WiFi access point too, not just another badge.

## How the direction finding works

The badge has **no magnetometer and no gyro**, so it cannot know which way it
is pointing. Two tricks get a bearing anyway:

**Body-shadow scan.** A human body attenuates 2.4 GHz by 10–20 dB. Turning in
place produces a roughly sinusoidal RSSI pattern, and the first harmonic of
that pattern points at the target. The fit uses only the shadowed half of the
sweep, where the notch is measurably sharper than the peak and carries more
directional information.

**Closing rate.** Once walking, the angle follows from geometry alone:

```
theta = arccos( -delta_distance / delta_walked )
```

Straight at the target the range drops 1 m per metre walked; perpendicular it
barely changes. Steps come from the accelerometer.

Measured accuracy: roughly **±29° from a single spin**, improving as √n when
spins are averaged (≈±17° over three). Honest numbers from testing against
known ground truth — this is a hallway-scale finder, not an AirTag.

See [finder-triangulation-design.md](finder-triangulation-design.md) for a
proposed next iteration using building access points as shared reference
points.

## Hardware

ESP32-C3-MINI-1-N4 · 400 KB SRAM, no PSRAM · 4 MB flash · ST7789 320×240 SPI
display · 8 buttons via 74HC165 shift register · SC7A20 accelerometer ·
MFRC522 NFC reader · 6× WS2812B · AA battery power.

Pin map and bring-up notes: <https://badge.hackthenorth.com/custom-flash>

## Build

Requires ESP-IDF v5.5.3.

```bash
idf.py set-target esp32c3
idf.py build
```

FTM is compiled out of ESP-IDF by default and must be enabled:

```
CONFIG_ESP_WIFI_FTM_ENABLE=y
CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT=y
CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT=y
```

## Flashing

```bash
esptool --chip esp32c3 -p /dev/cu.usbmodemXXXX \
  --before usb-reset --after watchdog-reset -b 460800 \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/badge_fw.bin
```

Two things that will waste your time otherwise:

- **`--after hard-reset` silently does nothing.** RTS is not wired to EN on
  this board, so esptool exits 0 and leaves the badge dead in the bootloader,
  looking exactly like a failed flash. Use `watchdog-reset`.
- **The first entry into download mode must be physical** — hold Start while
  plugging in USB. Once custom firmware is running, `--before usb-reset` works
  and the whole loop becomes software-driven.

## Your badge data survives

Keep the partition table byte-identical to stock and flashing touches only the
bootloader, partition table and app. `nvs` at `0x9000` and the LittleFS
`storage` partition are never written, so provisioning, contacts and saved app
state all survive. Verified by reading the flash back and diffing.

```
nvs,      data, nvs,      0x9000,   16K
phy_init, data, phy,      0xd000,   4K
factory,  app,  factory,  0x10000,  2688K
storage,  data, littlefs, 0x2b0000, 1280K
```

Take a full backup first regardless:

```bash
esptool --chip esp32c3 -p /dev/cu.usbmodemXXXX read-flash 0 0x400000 badge-backup.bin
```

The badge help desk can restore stock firmware, but only a flash image
restores your data.
