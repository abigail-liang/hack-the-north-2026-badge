# AP-Triangulation Badge Finder

**📊 [How it works — slideshow walkthrough](https://claude.ai/artifact/NurMfzXWSLegzPrbmA1wHB)** — the problem, the hardware, the two-stage method, and where it breaks down.

## Inspiration

At Hack the North 2026, we kept running into the same problem: the venue was huge, our team kept scattering across different rooms, and the stock badge firmware could only show you a list of nearby hackers—no distances, no directions. We'd know someone was "around here somewhere" but waste 10 minutes walking in circles trying to find them.

The insight came from looking at the badge hardware: it had full WiFi, an accelerometer, and ESP-NOW peer-to-peer messaging, but the stock Lua sandbox locked all of that away. We realized we could replace the firmware entirely and turn the badge into an actual **person finder**—something that tells you not just how far away your teammate is, but which way to walk.

## What it does

Our custom ESP-IDF firmware transforms the Hack the North hacker badge into a **real-time directional finder**:

- **Finds peer badges** automatically—each badge beacons as `HTN-FIND-XXXX` and scans for others, so either person can find either (no master/slave setup)
- **Measures distance** using 802.11mc Fine Timing Measurement (FTM), the same WiFi ranging protocol in modern phones, with ~1–5 m accuracy
- **Shows direction** even though the badge has no compass or gyro—uses your body blocking the WiFi signal as you spin to recover a bearing
- **Live compass mode** that updates as you walk, inferring your angle off-target from how fast the distance closes
- **Warmer/colder feedback** with an LED proximity ring that pulses as you get closer
- Works on **any WiFi access point** too, not just another badge—so you can navigate to known locations even if no one's wearing a badge there

The end result: point your badge at a teammate, and it tells you "12 meters, walk that way" with an arrow that stays pointed at them as you move.

## How we built it

### Hardware foundation

The badge is built around an **ESP32-C3** with a 320×240 ST7789 display, 8 buttons, an SC7A20 accelerometer, and WS2812B LEDs. Pin map and bring-up docs were available at the event, which saved us hours.

### Replacing the stock firmware

The stock firmware runs a locked-down Lua sandbox with 44-byte BLE broadcasts, no WiFi access, and no ranging. We flashed custom ESP-IDF v5.5.3 firmware that unlocks:

- Full WiFi scanning + SoftAP mode
- ESP-NOW at 50 Hz for peer messaging
- 802.11mc FTM initiator and responder support
- Raw BLE advertise/scan

We kept the partition table identical to stock so `nvs` and LittleFS (contacts, provisioning, saved state) survive flashing—verified by reading back the flash and diffing.

### Direction finding without a compass

The badge has **no magnetometer and no gyro**, so we had to get creative:

**Body-shadow scan:** A human body attenuates 2.4 GHz WiFi by 10–20 dB. Holding the badge against your chest and spinning in place creates a sinusoidal RSSI pattern. We fit the first harmonic of that pattern—using only the shadowed half of the sweep where the notch is sharper than the peak—to recover a bearing. Measured accuracy: **~±29° from a single 360° spin**, improving to ~±17° over three averaged spins.

**Closing-rate compass:** Once walking, geometry alone gives the angle:

\[
\theta = \arccos\left(-\frac{\Delta \text{distance}}{\Delta \text{walked}}\right)
\]

Walk straight at the target and range drops 1 m per metre; walk perpendicular and it barely changes. Steps come from the accelerometer.

### Two-stage triangulation (advanced mode)

We also implemented a **triangulation mode** that uses the building's WiFi access points as shared reference points. There's a [visual walkthrough of the method here](https://claude.ai/artifact/NurMfzXWSLegzPrbmA1wHB).

1. **Stage 1:** Both badges scan all visible APs and exchange their RSSI lists over ESP-NOW. For each common AP, we compute two candidate positions (mirrored across the line between badges).
2. **Stage 2:** User walks ~5 steps in any direction. Re-scanning from the new position resolves the mirror ambiguity and anchors the reconstructed map to the walk direction.

This gives an over-determined system (5–8 AP constraints instead of one), making it more robust to multipath errors. The AP positions can be cached, turning future lookups into single-scan localisation—instant, no walking required.

### The solve

For the closing-rate method, the solver is straightforward trigonometry. For triangulation, we place A₁ at the origin, A₂ at `(s, 0)` where `s` is the walked baseline, and solve for B at the intersection of two circles:

\[
x = \frac{d_1^2 - d_2^2 + s^2}{2s}, \quad y = \sqrt{d_1^2 - x^2}, \quad \theta = \operatorname{atan2}(y, x)
\]

Inconsistent ranges from multipath are clamped to the nearest feasible point rather than failing. AP residuals (predicted vs. reported distance) give a quality metric—under ~4 m mean absolute residual, the geometry is consistent.

## Challenges we ran into

- **ESP-IDF FTM compiled out by default:** Had to manually enable `CONFIG_ESP_WIFI_FTM_ENABLE`, `FTM_INITIATOR_SUPPORT`, and `FTM_RESPONDER_SUPPORT` in sdkconfig
- **esptool reset quirk:** `--after hard-reset` silently does nothing because RTS isn't wired to EN on this board. Had to use `--after watchdog-reset` and physically hold Start while plugging in USB for the first flash
- **RSSI to distance is terrible:** Factor-of-two errors are typical. The "circles" in our triangulation are really thick annuli, which limits angular accuracy to ~±15–25° even with multiple APs
- **Left/right ambiguity:** Distances are invariant under reflection—mirror the entire setup across the walk axis and every range is unchanged. Had to use the body-shadow spin's left/right signal or explicit "walk one way, warmer = correct" prompts
- **Scan blocks for ~2 seconds:** ESP-NOW requests arrive in the RX callback (ISR context), but a WiFi scan blocks and can't run in an ISR. Moved scan handling to the main loop
- **No ground truth in a live venue:** Hard to measure accuracy against known positions when the venue is crowded and moving. Tested in hallways beforehand, but real-world validation is still pending

## Accomplishments that we're proud of

- **Full firmware replacement in 36 hours**—from "can we do this?" to two badges finding each other across a venue
- **Working FTM ranging on ESP32-C3** with custom ESP-IDF config, something not well-documented at the time
- **Direction without a compass**—the body-shadow fit and closing-rate geometry actually work in practice, not just on paper
- **Triangulation mode implemented end-to-end:** ESP-NOW scan exchange, two-capture solver, live visualization with AP rings and candidate positions
- **Data survives flashing:** Partition table matches stock exactly, so contacts and provisioning persist. Verified by diffing flash images
- **Honest accuracy numbers:** ~±29° single spin, ~±17° over three spins, ~1–5 m FTM range. No marketing fluff—hallway-scale finder, not an AirTag

## What we learned

- **RSSI-based distance is fundamentally limited** by multipath and unknown TX power. Even with 5–8 APs constraining the solve, the thick annuli dominate error. Per-AP path-loss calibration might help, but each AP's TX power is unknown.
- **Over-determined systems are more robust:** Triangulation with multiple APs averages out independent multipath errors better than a single target measurement, but the limiting factor remains RSSI→distance error.
- **Geometry can substitute for sensors:** You can recover bearing from pure range measurements if you have a known displacement (walking) and enough constraints. No magnetometer needed.
- **APs are static and cacheable:** Once you solve AP positions, future localisation becomes a single scan matched against a known map. That's the standard indoor-positioning approach and the natural end state of this design.
- **Hardware quirks matter more than algorithms:** The esptool reset behavior, ISR vs. main-loop scan handling, and partition table layout ate more time than the trigonometry. Always read the schematics.
- **Test against ground truth early:** We built the full pipeline before measuring accuracy. Should have validated RSSI→distance and angular fit in a controlled setup first.

## What's next for goose game grinders

- **Measure real-world accuracy** against known positions in the venue—compare triangulation mode vs. closing-rate mode to see if the extra complexity actually helps
- **Per-AP path-loss calibration:** Learn each AP's effective TX power over time to tighten the RSSI→distance conversion
- **AP map caching across sessions:** Turn this into one-scan localisation by storing solved AP positions and reusing them
- **AP subset selection:** Test whether strongest-N APs or geometrically-spread APs (minimising dilution of precision) give better results
- **Multi-leg walking:** A second walk in a known turn direction could resolve left/right without needing the body-shadow spin
- **Port to other ESP32 badges:** The firmware is generic—could work on any ESP32 with WiFi and an accelerometer
- **Open-source release:** Clean up the code, add build docs, and ship it so other hackathons can use it

## Documentation

- **[Screen-by-screen walkthrough](docs/screens.md)** — photographs of the firmware running on two badges, captioned
- **[Triangulation design](finder-triangulation-design.md)** — the method, the geometry, the honest assessment, and what is real vs. scripted in demo mode
- **[Whiteboard](docs/whiteboard/)** — the boards the design was worked out on, and where later measurement contradicted them
- **[Firmware notes](docs/firmware-notes.md)** — the traps that cost us time: toolchain, reset handling, display byte order, ESP-NOW channel pinning
- **[Bearing measurements](docs/bearing-measurements.md)** — raw spin data
- **[Presentation](presentation/)** — slides, narration and animation sources

## Flashing safely

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

The badge help desk can restore stock firmware, but only a flash image restores
your data.

---

**grinding goose games** 🦢

Built at Hack the North 2026 by the goose game grinders team.
