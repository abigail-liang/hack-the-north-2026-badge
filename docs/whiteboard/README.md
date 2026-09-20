# Whiteboard — working out the triangulation

Photographs of the boards where the AP-triangulation design was worked out,
before any of it was written down or coded. Kept because the reasoning is
easier to follow here than in the solver, and because two of these boards
record conclusions that later measurement overturned.

| | Board | What it shows |
|---|---|---|
| 01 | [Circle intersection](01-circle-intersection.jpg) | The core construction. A range circle about A and one about B intersect at two points, `1` and `2`. The dashed verticals and the paired arrows show the two candidates are a mirror pair — and that the chords between them are parallel, which is the geometric fact the whole two-stage method rests on. |
| 02 | [Board overview](02-board-overview.jpg) | Stage 1 and stage 2 side by side with the circle construction between them. |
| 03 | [Board overview, second angle](03-board-overview-alt.jpg) | Same boards, different framing — the stage 1 rings read more clearly here. |
| 04 | [Stage 2, after the second capture](04-stage2-after-second-capture.jpg) | The target state: rings gone, one position per access point, one heading arrow. This is the sketch the stage-2 screen was built from. |
| 05 | [Stage 1, first capture](05-stage1-first-capture.jpg) | Every unknown is a ring, not a point. This is the sketch the stage-1 screen was built from, and getting the mirror axis right took three attempts in firmware. |
| 06 | [Full board with error analysis](06-full-board-with-error-analysis.jpg) | Everything at once: both stages, the circle constructions, the per-AP trilateration and the error budget. |
| 07 | [Trilateration from two positions](07-trilateration-two-positions.jpg) | Ranges to three access points from A₁ (16, 7, 25) and again from A₂ (27, 25, 10). Arcs that should meet at a point instead meet in a region — drawn to scale, this is the accuracy problem made visible. |
| 08 | [Error budget](08-error-budget.jpg) | The variances we thought mattered, the mitigation we expected to work (rolling average) and the assumptions the method needs: B does not move or turn, A does not turn. |

## What measurement later changed

The mitigation on board 08 — a rolling average over AP RSSI — does not work. We
measured per-AP RSSI spread over several seconds with both badges stationary and
found only **1–4 dB**. That is systematic error, not random, so averaging has
almost nothing to remove. The dominant term is the **unknown transmit power** of
each access point, roughly 10 dB, which is about **135% distance error** and
does not average away at all.

Combined with geometric dilution — a ~5 m walked baseline against access points
tens of metres away amplifies range error by about **4×** — this is why the
shipped triangulate mode runs as a labelled demo with synthetic AP geometry,
and why the body-shadow spin remains the method that actually produces a
bearing. Board 07 turns out to be the honest picture.
