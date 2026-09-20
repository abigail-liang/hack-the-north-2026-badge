# Bearing algorithm — measured results

Direction finding with no magnetometer and no gyro, measured against known
ground truth rather than estimated.

## Method

The badge is held flat against the chest and turned a full 360°. A body
attenuates 2.4 GHz by 10–20 dB, so RSSI varies roughly sinusoidally with
heading; the sweep is binned into 16 angular bins and fitted.

Ground truth came from spinning at known start orientations relative to a
fixed peer badge (facing it, 90° off, 180° off, then facing it again as a
repeatability check).

## Estimator comparison

All four run on the **same** 16-bin patterns, n = 8 spins:

| estimator | bias | scatter | notes |
|---|---|---|---|
| MAX — argmax bin | +51° | 60° | worst; no common-mode rejection |
| MIN+180 — argmin, opposite | +14° | 42° | much better than MAX |
| D180 — argmax of antipodal difference | +45° | 48° | quantised to 22.5° |
| **DFT — first harmonic** | +32° | **34°** | continuous angle, uses all bins |

Antipodal differencing rejects common-mode effects — transmit power, overall
path loss, slow drift — which is why everything beats the raw peak.

The DFT is the optimal form of that idea: a linearly-weighted circular
centroid *is* the first-harmonic DFT, and removing the 2nd harmonic cannot
change the 1st (orthogonal basis). Both variants returned byte-identical
results, which is a useful correctness check.

## The null carries more information than the peak

Measured curvature of the angular pattern:

```
at the PEAK  3.17 dB
at the NULL  5.44 dB     <- 1.7x sharper
```

The body-shadow notch is a sharper feature than the broad unblocked arc, so it
localises better. Weighting the fit toward the shadowed side improves accuracy
monotonically:

| DFT variant | scatter |
|---|---|
| equal weight | 33.9° |
| null-weighted ×2 | 31.3° |
| null-weighted ×3 | 30.4° |
| **shadowed half only** | **28.9°** |

The monotonic trend is what makes this convincing at n = 8, rather than any
single variant winning.

## Bias is dead time, and it is correctable

Every estimator shared a large positive bias with much smaller scatter. 44° is
12% of a spin — about one second of an eight-second sweep, i.e. the gap between
pressing the button and actually starting to turn. Those stationary bins shift
the whole pattern later.

A **fixed countdown** beats a motion trigger here. Triggering on detected
motion sounds better but fires at an inconsistent point in the turn: it cut
bias but tripled scatter (16° → 42°) and wrecked repeatability (18° → 104°).
**A consistent wrong delay is worth more than an inconsistent right one**,
because a constant can simply be subtracted.

## Accuracy

Single spin ≈ **±29°**. Averaging improves as √n:

| spins | accuracy |
|---|---|
| 1 | ±29° |
| 3 | ±17° |
| 4 | ±14° |

Average the complex first-harmonic **vectors**, not the angles — that is the
correct way to mean circular data and it weights each spin by its own
amplitude for free.

Confidence (first-harmonic amplitude) correlates with accuracy at only
**−0.16**, i.e. essentially not at all. It is a useful descriptor, not a
quality filter — do not weight by it.

## Live compass while walking

After the spin, the angle can be maintained without any heading sensor:

```
theta = arccos( -delta_distance / delta_walked )
```

Straight at the target the range drops 1 m per metre walked; perpendicular it
barely changes; away it grows. Distance from FTM, displacement from
accelerometer step counting.

Left/right is **not observable** this way — the closing rate is identical
either side — so the sign is carried over from the calibration spin.

## Caveats

- Reflections can produce a confident bearing pointing at a wall. Re-spinning
  a few metres away is the check: a true bearing is stable, a reflection
  usually is not.
- Below ~5 dB of span the body-shadow effect has not materialised and the
  result is noise. The firmware reports span and flags this.
- A rigid reflector behind the antenna would likely help less through added
  directivity than through **consistency** — the body already provides 10–20 dB,
  but varies with posture and grip, and that variation is the dominant error.
