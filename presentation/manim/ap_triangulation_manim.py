"""AP-triangulation finder, step by step — Manim CE animation with voiceover.

Mirrors finder-triangulation-design.md: two badges, building APs as shared
reference points, circle intersections with a mirror ambiguity, and the short
walk that resolves it.  Narration clips live in narr/{i}.wav (macOS `say`);
each step is padded so the visuals never outrun the voice.
"""
import wave

import numpy as np
from manim import *

# ---- world geometry (metres) ----
SC = 0.72  # metres -> scene units


def P(p):
    # centred on the midpoint of AB; no caption to clear, so use the full frame
    return np.array([(p[0] - 2.5) * SC, p[1] * SC, 0.0])


A1 = (0.0, 0.0)
B = (5.0, 0.0)
APS = [(1.4, 1.9), (3.9, 1.5), (2.1, -1.7)]
WALK_ANG = np.deg2rad(62)
WALK_LEN = 1.6
A2 = (WALK_LEN * np.cos(WALK_ANG), WALK_LEN * np.sin(WALK_ANG))


def dist(p, q):
    return float(np.hypot(p[0] - q[0], p[1] - q[1]))


DA = [dist(A1, p) for p in APS]
DB = [dist(B, p) for p in APS]

C_A = "#5b96ff"      # badge A
C_B = "#ff7a5c"      # badge B
C_OK = "#4cc98a"     # resolved
C_GHOST = "#6a6d75"  # ambiguity
C_INK = "#e9e9e4"

NARR = [f"narr/{i}.wav" for i in range(9)]


def wav_dur(path):
    with wave.open(path) as w:
        return w.getnframes() / w.getframerate()


DUR = [wav_dur(f) for f in NARR]

Text.set_default(font="Helvetica")
Paragraph.set_default(font="Helvetica")


def dashed_circle(center, r, color, dash_ratio=0.55, num=60, width=2.5):
    c = Circle(radius=r * SC, color=color, stroke_width=width).move_to(P(center))
    return DashedVMobject(c, num_dashes=num, dashed_ratio=dash_ratio)


def ap_square(p, color=C_INK, fill=True, size=0.22):
    s = Square(side_length=size, color=color, stroke_width=2.5).move_to(P(p))
    if fill:
        s.set_fill(color, opacity=1)
    return s


def badge_tri(p, color=C_B, fill=True, size=0.26):
    t = Triangle(color=color, stroke_width=2.5).scale(size).move_to(P(p))
    if fill:
        t.set_fill(color, opacity=1)
    return t


class APTriangulation(Scene):
    # ---- timing helpers: keep each step at least as long as its narration ----
    def play(self, *args, **kwargs):
        rt = kwargs.get("run_time", 1.0)
        super().play(*args, **kwargs)
        self._elapsed += rt

    def wait(self, duration=1.0, **kwargs):
        super().wait(duration, **kwargs)
        self._elapsed += duration

    def step_start(self, i):
        self.add_sound(NARR[i])
        self._step_i = i
        self._step_t0 = self._elapsed

    def step_end(self, tail=0.25):
        used = self._elapsed - self._step_t0
        pad = DUR[self._step_i] - used + tail
        if pad > 0:
            self.wait(pad)

    # captions removed — the voiceover carries the narration
    def caption(self, title, body, color=C_A):
        return None

    def swap_caption(self, new):
        pass

    def construct(self):
        self._cap = None
        self._elapsed = 0.0

        # ---------- 0 · title ----------
        self.step_start(0)
        t1 = Text("Badge Finder", font_size=48, weight=BOLD, color=C_INK)
        t2 = Text("AP triangulation — a direction from borrowed WiFi",
                  font_size=26, color=C_GHOST)
        title = VGroup(t1, t2).arrange(DOWN, buff=0.35)
        self.play(FadeIn(t1, shift=UP * 0.3), FadeIn(t2), run_time=1.2)
        self.step_end()
        self.play(FadeOut(title), run_time=0.6)

        # ---------- 1 · the cast: A, B, and the APs ----------
        self.step_start(1)
        cap = self.caption("Step 1 · The room",
                           ["You hold badge A; your teammate holds badge B, 5 m away.",
                            "The building's access points never move — and both badges see them."])
        self.swap_caption(cap)

        dotA = Dot(P(A1), color=C_A, radius=0.1)
        labA = Text("A (you)", font_size=22, color=C_A).next_to(dotA, UP, buff=0.15)
        triB = badge_tri(B)
        labB = Text("B", font_size=22, color=C_B).next_to(triB, UP, buff=0.15)
        self.play(FadeIn(dotA, scale=0.5), FadeIn(labA), run_time=0.9)
        self.wait(1.2)
        self.play(FadeIn(triB, scale=0.5), FadeIn(labB), run_time=0.9)
        self.wait(1.6)
        aps = VGroup(*[ap_square(p) for p in APS])
        ap_labels = VGroup(*[
            Text(f"AP{i+1}", font_size=18, color=C_GHOST).next_to(a, UP, buff=0.12)
            for i, a in enumerate(aps)
        ])
        self.play(LaggedStart(*[FadeIn(a, scale=0.5) for a in aps], lag_ratio=0.25),
                  FadeIn(ap_labels), run_time=1.6)
        self.step_end()

        # ---------- 2 · the problem: a distance is not a direction ----------
        self.step_start(2)
        cap = self.caption("Step 2 · The problem",
                           ["FTM time-of-flight says B is 5.0 m away — but with no",
                            "compass or gyro, B could be anywhere on this ring."], C_B)
        self.swap_caption(cap)

        ring = dashed_circle(A1, 5, C_B, num=70)
        dlab = Text("|AB| = 5.0 m  (FTM)", font_size=22, color=C_GHOST,
                    font="Menlo").move_to(P((-0.2, -3.5)))
        self.play(Create(ring), FadeIn(dlab), run_time=1.8)
        ghosts = VGroup(*[
            badge_tri((5 * np.cos(a), 5 * np.sin(a)), fill=False)
            for a in [0.9, 2.2, 3.3, -1.2, -2.4]
        ]).set_opacity(0.45)
        self.play(LaggedStart(*[FadeIn(g) for g in ghosts], lag_ratio=0.15),
                  run_time=1.2)
        self.step_end()

        # ---------- 3 · badge A draws its circles ----------
        self.step_start(3)
        cap = self.caption("Step 3 · A measures the APs",
                           ["RSSI path loss turns signal strength into a rough distance,",
                            "putting each AP somewhere on a dashed circle around A."])
        self.swap_caption(cap)
        self.play(FadeOut(ring), FadeOut(ghosts), FadeOut(dlab), run_time=0.7)

        ringsA = VGroup(*[dashed_circle(A1, r, C_A) for r in DA])
        self.play(LaggedStart(*[Create(r) for r in ringsA], lag_ratio=0.3),
                  run_time=2.4)
        self.step_end()

        # ---------- 4 · B answers, frame chosen ----------
        self.step_start(4)
        cap = self.caption("Step 4 · B scans too — and a frame is chosen",
                           ["B sends its AP list back over ESP-NOW (56 bytes).",
                            "Put A at the origin, B at (5, 0) — on the x-axis by construction."],
                           C_B)
        self.swap_caption(cap)

        # no A-B line yet: it will be derived from the chord midpoints in the next step
        ringsB = VGroup(*[dashed_circle(B, r, C_B) for r in DB])
        self.play(ringsA.animate.set_opacity(0.35),
                  LaggedStart(*[Create(r) for r in ringsB], lag_ratio=0.3),
                  run_time=2.4)
        self.step_end()

        # ---------- 5 · intersections, parallel chords ----------
        self.step_start(5)
        cap = self.caption("Step 5 · Circles intersect — twice",
                           ["Each AP's two circles cross at two mirror points across AB.",
                            "Every chord between a pair is perpendicular to AB — all parallel."],
                           C_OK)
        self.swap_caption(cap)

        cands, mirrors, chords = VGroup(), VGroup(), VGroup()
        for p in APS:
            m = (p[0], -p[1])
            cands.add(ap_square(p, fill=False))
            mirrors.add(ap_square(m, fill=False))
            chords.add(DashedLine(P(p), P(m), color=C_OK, stroke_width=2.5,
                                  dash_length=0.09))
        self.play(ringsB.animate.set_opacity(0.35),
                  aps.animate.set_opacity(0.3), ap_labels.animate.set_opacity(0.3),
                  FadeIn(cands), FadeIn(mirrors), run_time=1.2)
        self.play(LaggedStart(*[Create(c) for c in chords], lag_ratio=0.3),
                  run_time=1.8)
        perp = Text("all chords parallel", font_size=22, color=C_OK,
                    font="Menlo").move_to(P((4.6, 3.6)))
        self.play(FadeIn(perp), run_time=0.6)
        # derive the A-B line from the chord midpoints
        mids = VGroup(*[Dot(P((p[0], 0.0)), color=C_OK, radius=0.055) for p in APS])
        self.play(FadeIn(mids), run_time=0.7)
        axis = DashedLine(P((-0.7, 0)), P((5.7, 0)), color=C_INK, stroke_width=2,
                          dash_length=0.08).set_opacity(0.7)
        self.play(Create(axis), run_time=1.4)
        alab = Text("midpoints → the A–B line", font_size=18, color=C_INK,
                    font="Menlo").move_to(P((2.5, -0.7)))
        self.play(FadeIn(alab), run_time=0.5)
        self.step_end()

        # ---------- 6 · the ambiguity ----------
        self.step_start(6)
        cap = self.caption("Step 6 · The catch — the shape can still flip",
                           ["The relative shape is right, but nothing ties it to the room:",
                            "it can rotate freely, and every AP could sit on either side of AB."],
                           C_GHOST)
        self.swap_caption(cap)
        self.play(FadeOut(ringsA), FadeOut(ringsB), FadeOut(perp),
                  FadeOut(ap_labels), FadeOut(aps), FadeOut(mids), FadeOut(alab),
                  run_time=0.8)

        # the axis is known — extend it both ways; the SIGN along it is not
        axis_ext = DashedLine(P((-0.7, 0)), P((-5.7, 0)), color=C_INK,
                              stroke_width=2, dash_length=0.08).set_opacity(0.7)
        self.play(Create(axis_ext), run_time=0.9)
        arr_fwd = Arrow(P((0.4, 0)), P((3.0, 0)), color=C_GHOST, buff=0,
                        stroke_width=5, max_tip_length_to_length_ratio=0.14)
        arr_bwd = Arrow(P((-0.4, 0)), P((-3.0, 0)), color=C_GHOST, buff=0,
                        stroke_width=5, max_tip_length_to_length_ratio=0.14)
        self.play(GrowArrow(arr_fwd), GrowArrow(arr_bwd), run_time=1.0)
        ghostB = badge_tri((-5.0, 0), fill=False)
        qb = Text("or B here?", font_size=20, color=C_GHOST).next_to(
            P((-5.0, 0)), UP, buff=0.25)
        self.play(FadeIn(ghostB), FadeIn(qb), run_time=0.8)
        # pulse the two candidate directions alternately
        self.play(arr_fwd.animate.set_color(C_A).set_opacity(1),
                  arr_bwd.animate.set_opacity(0.3), run_time=0.8)
        self.play(arr_bwd.animate.set_color(C_A).set_opacity(1),
                  arr_fwd.animate.set_color(C_GHOST).set_opacity(0.3), run_time=0.8)
        self.play(arr_fwd.animate.set_opacity(0.7),
                  arr_bwd.animate.set_color(C_GHOST).set_opacity(0.7), run_time=0.6)
        self.step_end()

        # ---------- 7 · walk and re-measure ----------
        self.step_start(7)
        cap = self.caption("Step 7 · Stage 2 — walk, then measure again",
                           ["Walk a few steps: the accelerometer counts them, and the direction",
                            "is 'straight ahead' in your own body frame. Re-scan from A₂."])
        self.swap_caption(cap)

        self.play(FadeOut(arr_fwd), FadeOut(arr_bwd), FadeOut(ghostB),
                  FadeOut(qb), FadeOut(axis_ext), run_time=0.6)
        walk = Arrow(P(A1), P(A2), color=C_A, buff=0.05, stroke_width=5,
                     max_tip_length_to_length_ratio=0.18)
        labA2 = Text("A₂", font_size=22, color=C_A).next_to(P(A2), UP, buff=0.15)
        wlab = Text("1.6 m", font_size=18, color=C_A,
                    font="Menlo").next_to(walk.get_center(), LEFT, buff=0.2)
        self.play(GrowArrow(walk), dotA.animate.move_to(P(A2)),
                  labA.animate.next_to(P(A2), UP, buff=0.15).set_opacity(0),
                  FadeIn(labA2), FadeIn(wlab), run_time=1.4)

        rings2 = VGroup(*[dashed_circle(A2, dist(A2, p), C_A) for p in APS])
        self.play(LaggedStart(*[Create(r) for r in rings2], lag_ratio=0.3),
                  run_time=2.0)
        crosses = VGroup(*[
            Cross(stroke_color=C_B, stroke_width=4, scale_factor=0.18)
            .move_to(P((p[0], -p[1]))) for p in APS
        ])
        self.play(FadeIn(crosses), mirrors.animate.set_opacity(0.15), run_time=0.9)
        for s in cands:
            s.generate_target()
            s.target.set_fill(C_INK, opacity=1)
        self.play(*[MoveToTarget(s) for s in cands], run_time=0.8)
        self.step_end()

        # ---------- 8 · a direction to walk + end card ----------
        self.step_start(8)
        cap = self.caption("Step 8 · A direction to walk",
                           ["Three anchors — A₁, B, A₂ — pin the map to the walk direction.",
                            "Bearing to B becomes an angle off your own heading. Cache the APs:",
                            "the next locate is a single scan."], C_OK)
        self.swap_caption(cap)
        self.play(FadeOut(rings2), FadeOut(crosses), FadeOut(mirrors),
                  FadeOut(wlab), FadeOut(chords), run_time=0.8)

        heading_end = (A2[0] + 1.7 * np.cos(WALK_ANG), A2[1] + 1.7 * np.sin(WALK_ANG))
        heading = Arrow(P(A2), P(heading_end), color=C_GHOST, buff=0.05,
                        stroke_width=4)
        hlab = Text("your heading", font_size=18, color=C_GHOST).next_to(
            P(heading_end), UP, buff=0.1)
        bearing = Arrow(P(A2), P(B), color=C_OK, buff=0.1, stroke_width=6)
        self.play(GrowArrow(heading), FadeIn(hlab), run_time=0.9)
        self.play(GrowArrow(bearing), run_time=1.0)

        ang_b = np.arctan2(B[1] - A2[1], B[0] - A2[0])
        arc = Arc(radius=1.0 * SC, start_angle=ang_b, angle=WALK_ANG - ang_b,
                  arc_center=P(A2), color=C_OK, stroke_width=3)
        theta = int(round(np.degrees(abs(WALK_ANG - ang_b))))
        mid = (WALK_ANG + ang_b) / 2
        tlab = Text(f"θ ≈ {theta}°", font_size=22, color=C_OK, font="Menlo").move_to(
            P((A2[0] + 1.9 * np.cos(mid), A2[1] + 1.9 * np.sin(mid))))
        self.play(Create(arc), FadeIn(tlab), run_time=1.0)

        pulse = Circle(radius=0.35, color=C_OK).move_to(P(B))
        self.play(Create(pulse), run_time=0.6)
        self.play(pulse.animate.scale(1.8).set_opacity(0), run_time=0.9)
        self.wait(2.0)

        # end card rides the tail of the last narration
        self.play(*[FadeOut(m) for m in self.mobjects], run_time=0.9)
        e1 = Text("±15–25° expected — vs ±29° from a single body-shadow spin",
                  font_size=26, color=C_INK)
        e2 = Text("APs are static: cache the map, and finding becomes one scan.",
                  font_size=24, color=C_OK)
        end = VGroup(e1, e2).arrange(DOWN, buff=0.4)
        self.play(FadeIn(end), run_time=1.0)
        self.step_end(tail=1.0)
