# AP-triangulation finder — design

Two-stage method for getting a **direction to walk**, not just a distance,
using the building's WiFi access points as shared reference points.

Origin: team ideation session, whiteboard diagrams below.

**Status: implemented and on hardware.** See `firmware/main.c`, the
TRIANGULATE page. What follows describes the design; the
[Implementation](#implementation) section at the end records what was actually
built and what changed once it met reality.

---

## Why

The current finder measures **distance** well (FTM, ~1–5 m) and gives only a
weak **bearing** (body-shadow spin, ±29° single spin). Knowing you are 8 m from
your teammate without knowing which way to walk is not useful enough.

Key insight from the team: **we are not alone in the room.** There are 5–8
access points visible, they are static, and *both* badges can see them. They
are free reference points.

---

## The method

### Stage 1 — first capture

![Stage 1, first capture](docs/images/design-stage1-first-capture.jpg)


1. A picks B as the target and sends B a "locate me" request (ESP-NOW).
2. **A scans**: RSSI of every visible AP, plus range to B (FTM).
3. **B scans** the same way and sends its AP list back to A.
4. A keeps only the **AP subset visible to both**.
5. For each common AP *i*:
   - `dA_i` = distance A→AP from RSSI path loss
   - `dB_i` = distance B→AP from RSSI path loss
   - Circle of radius `dA_i` about A, circle of radius `dB_i` about B
   - They intersect at **two candidate positions** for that AP

After stage 1 every AP has two possible locations, mirrored across the line AB,
and B itself has two possible bearings.

### Stage 2 — second capture

![Stage 2, after second capture](docs/images/design-stage2-after-second-capture.jpg)


6. Prompt the user to **walk a few steps in any direction**. Step count gives
   the displacement magnitude; the direction is "straight ahead" in the user's
   own body frame.
7. **A re-scans** from the new position A₂.
8. Three anchors now exist — A₁, B, A₂ — which resolves the mirror ambiguity
   and fixes each AP's true position.
9. The reconstructed map is now **anchored to the direction the user walked**,
   so the bearing to B can be expressed as an angle relative to their heading.

---

## Geometry check: the parallel-chords hypothesis

![Circle intersections give two candidate AP positions](docs/images/design-circle-intersection.jpg)

![All chords parallel, perpendicular to AB](docs/images/design-parallel-chords.jpg)

**Team hypothesis:** the lines joining each AP's two candidate positions are
all parallel, and the direction to travel is perpendicular to them.

**Verified — both parts are true.** Two circles centred at A and B intersect
at two points that are reflections of each other across the line AB, so the
chord joining them is perpendicular to AB. That holds for every AP, so all
chords are parallel to each other and perpendicular to the A→B direction.
Checked numerically against six random AP positions with a non-axis-aligned
AB: every chord came out at exactly 90.00° to AB.

**But there is a catch, and it matters.**

To draw those circles you must place A and B in some coordinate frame. We know
`|AB|` from FTM but *not* the bearing, so in practice you put A at the origin
and B at `(|AB|, 0)`. In that frame the chords are perpendicular to the x-axis
**by construction**. The relationship is real, but in the only frame we can
actually build it is tautological — it recovers the axis we ourselves chose,
not a real-world direction.

So stage 1 alone cannot tell you which way to walk. It reconstructs the
*relative* geometry of {A, B, APs} correctly, but that shape is free to rotate
and reflect: without a compass there is nothing tying it to the room.

**A second, harder limit found while implementing this: left/right is not
recoverable from ranges at all.** Every measurement we have is a distance, and
distances are invariant under reflection — mirror B *and* every AP across the
walk axis and every single range is unchanged. No amount of extra APs fixes
this, because it is a symmetry of the data, not a shortage of it. Something
outside the range measurements has to break the tie: the body-shadow spin, a
second walk leg in a known turn direction, or simply walking one way and
seeing whether it gets warmer.

**Stage 2 is what actually supplies the missing information.** Walking gives a
displacement whose direction the user can feel. Re-measuring from A₂ both
kills the reflection ambiguity and pins the reconstructed map to the walk
direction. That is what converts the shape into an instruction.

---

## Honest assessment

**What this adds over the closing-rate method already implemented:**
the same fundamental principle (walk, re-measure, solve), but with 5–8 extra
constraints per capture instead of one. An over-determined system is far more
robust to any single bad measurement, and multipath errors on different APs are
largely independent, so they partially average out. That is a real improvement,
not a different capability.

**The strongest reason to do it, which came out of the diagrams:**
**APs are static.** Once their positions are solved they can be cached. A later
localisation becomes a single scan matched against a known map — no walking, no
spinning, instant. That is the standard indoor-positioning approach and it is
the natural end state of this design.

**Main risks**

| Risk | Detail |
|---|---|
| RSSI→distance is poor | Factor-of-two typical. The rings are thick annuli, not thin circles. Errors here dominate. |
| Geometric dilution | If the common APs are nearly collinear with AB, the intersections are ill-conditioned and the solution is unstable. Prefer APs spread in bearing. |
| Needs a data channel | B must return its scan. ESP-NOW carries 250 B; 8 APs × (6 B BSSID + 1 B RSSI) = 56 B. Comfortable. |
| Step length | 0.72 m assumed. Errors scale the whole map, though not the bearing. |
| Walk direction must be straight | A curved walk breaks the displacement assumption. |

**Expected accuracy:** better than the current ±29° single spin, because the
system is over-determined — but bounded by RSSI distance error, so probably
±15–25° rather than a precise arrow. Worth measuring rather than predicting.

---

## UI

**Stage 1 screen** (per IMG_7304): user is the dot at centre; dashed concentric
rings are the RSSI-derived distances to each AP; black squares mark AP
candidate positions; two triangles show the two mirrored candidate bearings for
B. Prompt: *"walk a few steps"*.

**Stage 2 screen** (per IMG_7303): ambiguity resolved. Rings collapse to single
AP positions, one triangle remains, and a single arrow points the way.

**After stage 2:** a live compass. As the user turns, the map rotates with them
and the arrow keeps pointing at B.

> Note the live-compass step still needs a heading source. With no gyro or
> magnetometer, rotation cannot be tracked directly — so this has to be
> maintained the same way as the current implementation, by continuing to
> re-measure while walking, not by dead-reckoning the turn.

---

## Open questions

1. How bad is RSSI→distance for the APs in practice? Measure before building.
2. Does per-AP path-loss calibration help, given each AP has unknown TX power?
3. Best AP subset: strongest N, or the spread that minimises dilution?
4. Can the AP map be cached across sessions, turning this into one-scan
   localisation?

---

## Implementation

Built and flashed to both badges. TRIANGULATE is the third page; **RIGHT**
from the target list or the finder reaches it.

### Protocol

Scan exchange over ESP-NOW broadcast, two message types:

```
MSG_SCAN_REQ  0xA1   [type][0]
MSG_SCAN_RESP 0xA2   [type][n][ {bssid[6], rssi} x n ]
```

16 APs max, 7 bytes each — 114 bytes, comfortably inside ESP-NOW's 250.
The request arrives in the RX callback but is served from the main loop, since
a scan blocks for ~2 s and must not run in an ISR.

### Flow

1. Lock a peer on the target list, press RIGHT for TRIANGULATE.
2. **A** — captures the AP scan plus the FTM range to B, and requests B's scan.
3. Screen prompts **WALK ~5 STEPS**; the accelerometer accumulates the baseline.
4. **A** again — second capture, then solve.

### Solver

A₁ at the origin, A₂ at `(s, 0)` where `s` is the walked baseline and the
**walk direction is +x**. B sits at the intersection of circles `dAB1` about
A₁ and `dAB2` about A₂:

```
x = (d1² − d2² + s²) / 2s
y = √(d1² − x²)
θ = atan2(y, x)
```

Inconsistent ranges (`d1² < x²`, which multipath will produce) clamp to the
nearest feasible point rather than failing.

The APs are used as a **quality metric rather than to solve**: each common AP
is placed from our own two ranges, and its predicted distance to the solved B
is compared against the range B actually reported. The mean absolute residual
is displayed — under ~4 m the geometry is consistent, above it the solution
should be distrusted.

### What the screen shows

Both mirror solutions are drawn, since ranges cannot choose between them. The
one matching the last spin's left/right is highlighted. Text is explicit that
it is *"LEFT or RIGHT — walk one way; warmer = correct"*, rather than
pretending to a certainty the physics does not support.

### Honest status

The plumbing works: both badges beacon, exchange scans on request, capture at
two points and solve. What has **not** been established is whether the answer
is any *better* than the closing-rate compass already running — the AP
residual gives a consistency check, but accuracy against known ground truth
has not been measured. That is the next thing to do, and the design should not
be trusted over the simpler method until it has been.

The expected limiting factor remains RSSI→distance error: the rings are thick
annuli, so the AP constraints are weak even though there are several of them.
