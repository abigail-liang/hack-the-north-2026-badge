# Screen-by-screen walkthrough

Photographs of the firmware running on two real badges. Captions here are the
ones used in the Devpost gallery.

| | Screen | Caption |
|---|---|---|
| 01 | [Target list](images/screens/01-target-list.jpg) | Every badge beacons as its own access point, so finding a person is just a WiFi scan. `HTN-FIND-Lin` sorts to the top and is tagged `FTM` — she supports time-of-flight ranging. Everything below is real conference WiFi. |
| 02 | [Asking](images/screens/02-consent-asking.jpg) | You cannot track someone who has not agreed. Selecting a target sends a request over ESP-NOW and waits. |
| 03 | [Consent prompt](images/screens/03-consent-prompt.jpg) | What the other badge shows. It names who is asking and what they will see, and nothing is shared until ALLOW. |
| 04 | [Close range](images/screens/04-range-close.jpg) | 0.9 m apart. WiFi at −39 dBm, BLE at −47, and the LED ring goes cyan as you close in. |
| 05 | [Across the room](images/screens/05-range-far.jpg) | 27 m apart, LEDs magenta. FTM reports 24.45 m from the round-trip flight time of the packet. |
| 06 | [Stale ranging](images/screens/06-ftm-stale.jpg) | FTM sessions fail often indoors, so the screen labels a reading `stale` rather than showing a stale number as if it were current. |
| 07 | [Live ranging](images/screens/07-ftm-live.jpg) | The same reading marked `live`. Honest error reporting mattered more than a clean display. |
| 08 | [Spin calibration](images/screens/08-spin-calibration.jpg) | With no compass, direction comes from your own body. Hold the badge to your chest and turn once; the bars are signal strength per angular bin. |
| 09 | [Bearing](images/screens/09-bearing-result.jpg) | Your body blocks WiFi, so the weakest direction is where the target is. A first-harmonic fit over the shadowed half gives a bearing — 345°, turn 15° left. The ±29° is shown because it is real. |
| 10 | [Triangulate, step 1](images/screens/10-triangulate-step1.jpg) | The second mode: locate someone using the room's own access points. Labelled `DEMO — sim APs`, because the AP geometry is synthetic. |
| 11 | [One capture is not enough](images/screens/11-stage1-map.jpg) | After one capture every unknown is a ring, not a point. Each access point has two possible positions and so does Lin, mirrored across the axis — ranges alone cannot pick a side. |
| 12 | [Walking the baseline](images/screens/12-walk-counter.jpg) | Accelerometer step detection measures the baseline. 3 steps, 2.2 m — enough to break the symmetry. |
| 13 | [Solved](images/screens/13-stage2-solved.jpg) | Second capture from the new position. Rings gone, one position per access point, and a heading: turn right 45°, then walk. |
