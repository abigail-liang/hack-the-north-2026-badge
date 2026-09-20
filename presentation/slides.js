// Badge Finder — presentation deck
// Dark theme matching the animation: blue dot = you (A), coral triangle = friend (B),
// white squares = access points, dashed rings = distance estimates.
const pptxgen = require("pptxgenjs");
const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE"; // 13.33 x 7.5

const BG = "15171B", PANEL = "1E2126", PANEL2 = "23262C";
const INK = "E9E9E4", MUTED = "9BA0A8", GHOST = "5A5D64";
const BLUE = "5B96FF", CORAL = "FF7A5C", GREEN = "4CC98A";
const W = 13.333, H = 7.5;

function bgSlide() {
  const s = pres.addSlide();
  s.background = { color: BG };
  return s;
}

// ---- geometry motif helpers (inches) ----
function dot(s, x, y, r, color) {
  s.addShape("ellipse", { x: x - r, y: y - r, w: 2 * r, h: 2 * r, fill: { color }, line: { type: "none" } });
}
function tri(s, x, y, size, color, filled = true) {
  s.addShape("triangle", {
    x: x - size / 2, y: y - size / 2, w: size, h: size,
    fill: filled ? { color } : { color: BG, transparency: 100 },
    line: filled ? { type: "none" } : { color, width: 1.5 },
  });
}
function sq(s, x, y, size, color, filled = true) {
  s.addShape("rect", {
    x: x - size / 2, y: y - size / 2, w: size, h: size,
    fill: filled ? { color } : { color: BG, transparency: 100 },
    line: filled ? { type: "none" } : { color, width: 1.5 },
  });
}
function ring(s, x, y, r, color, dash = "dash", width = 1.5) {
  s.addShape("ellipse", {
    x: x - r, y: y - r, w: 2 * r, h: 2 * r,
    fill: { color: BG, transparency: 100 },
    line: { color, width, dashType: dash },
  });
}
function seg(s, x1, y1, x2, y2, color, width = 1.5, dash = "solid", arrow = false) {
  const opts = {
    x: Math.min(x1, x2), y: Math.min(y1, y2),
    w: Math.abs(x2 - x1) || 0.001, h: Math.abs(y2 - y1) || 0.001,
    line: { color, width, dashType: dash },
    flipH: x2 < x1, flipV: y2 < y1,
  };
  if (arrow) opts.line.endArrowType = "triangle";
  s.addShape("line", opts);
}
function label(s, text, x, y, w, opts = {}) {
  s.addText(text, Object.assign({
    x, y, w, h: opts.h || 0.3, isTextBox: true, margin: 0,
    fontFace: opts.mono ? "Courier New" : "Arial",
    fontSize: opts.size || 12, color: opts.color || MUTED,
    align: opts.align || "left", bold: opts.bold || false,
  }, opts.extra || {}));
}
function eyebrow(s, text, color = BLUE) {
  s.addText(text, {
    x: 0.7, y: 0.55, w: 8, h: 0.3, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 13, color, bold: true, charSpacing: 4,
  });
}
function title(s, text, opts = {}) {
  s.addText(text, {
    x: 0.7, y: opts.y || 0.92, w: opts.w || 11.9, h: opts.h || 0.85, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: opts.size || 34, color: opts.color || INK, bold: true,
  });
}

// =========================================================
// 1 · TITLE
// =========================================================
{
  const s = bgSlide();
  // motif: ring with A at centre, B on the ring, APs inside
  const cx = 10.0, cy = 3.75, R = 2.7;
  ring(s, cx, cy, R, CORAL);
  ring(s, cx, cy, 1.55, BLUE, "sysDash", 1);
  dot(s, cx, cy, 0.09, BLUE);
  tri(s, cx + R * Math.cos(-0.5), cy + R * Math.sin(-0.5), 0.34, CORAL);
  sq(s, cx - 0.9, cy - 1.25, 0.2, INK);
  sq(s, cx + 0.65, cy + 1.4, 0.2, INK);
  sq(s, cx + 1.35, cy - 0.55, 0.2, INK);

  s.addText("BADGE FINDER", {
    x: 0.7, y: 2.35, w: 7.2, h: 1.0, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 54, color: INK, bold: true, charSpacing: 2,
  });
  s.addText("Finding a friend with borrowed WiFi", {
    x: 0.7, y: 3.45, w: 7.2, h: 0.5, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 22, color: BLUE,
  });
  s.addText("Hack the North 2026  ·  custom ESP32-C3 badge firmware", {
    x: 0.7, y: 4.15, w: 7.2, h: 0.4, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 14, color: MUTED,
  });
}

// =========================================================
// 2 · SCENARIO
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "THE SCENARIO");
  s.addText("You're trying to find your friend\nat Hack the North.", {
    x: 0.7, y: 1.35, w: 6.8, h: 1.9, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 36, color: INK, bold: true,
  });

  // crowd panel: many ghost dots, one coral triangle hiding in it
  s.addShape("roundRect", { x: 8.0, y: 0.9, w: 4.6, h: 5.0, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
  let seed = 7;
  const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
  for (let i = 0; i < 9; i++) {
    for (let j = 0; j < 9; j++) {
      const x = 8.35 + i * 0.49 + (rnd() - 0.5) * 0.22;
      const y = 1.25 + j * 0.49 + (rnd() - 0.5) * 0.22;
      if (i === 6 && j === 2) tri(s, x, y, 0.24, CORAL);
      else dot(s, x, y, 0.05, GHOST);
    }
  }
  label(s, "your friend is in here. somewhere.", 8.0, 6.05, 4.6, { align: "center", size: 12 });

  // three facts
  const facts = [
    ["1,000+", "hackers in one building", 26, 0.7, 2.25],
    ["0 bars", "of cell signal in the hall", 26, 3.1, 2.25],
    ["“by the tables”", "describes every corner of the venue", 20, 5.5, 2.3],
  ];
  facts.forEach(f => {
    s.addText(f[0], { x: f[3], y: 4.35, w: f[4], h: 0.55, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: f[2], bold: true, color: CORAL, valign: "bottom" });
    s.addText(f[1], { x: f[3], y: 4.95, w: f[4], h: 0.8, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 13, color: MUTED });
  });
  s.addText([
    { text: "Your badge can help — ", options: { color: INK } },
    { text: "if we replace its firmware.", options: { color: GREEN, bold: true } },
  ], { x: 0.7, y: 6.3, w: 6.8, h: 0.5, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 18 });
}

// =========================================================
// 3 · THE BADGE
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "THE HARDWARE");
  title(s, "The badge in your hands");

  const cardY = 2.0, cardH = 3.9;
  // HAS
  s.addShape("roundRect", { x: 0.7, y: cardY, w: 5.9, h: cardH, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
  s.addText("WHAT IT HAS", { x: 1.1, y: cardY + 0.35, w: 5.1, h: 0.35, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, bold: true, color: GREEN, charSpacing: 3 });
  s.addText([
    { text: "WiFi — full scan, ESP-NOW, SoftAP", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "802.11mc FTM — time-of-flight ranging", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "BLE — raw advertise + scan", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "Accelerometer — step counting", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "6 RGB LEDs + 320×240 display", options: { bullet: { code: "25AA", indent: 14 } } },
  ], { x: 1.1, y: cardY + 0.85, w: 5.1, h: 2.8, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, color: INK, paraSpaceAfter: 10 });

  // HASN'T
  s.addShape("roundRect", { x: 6.9, y: cardY, w: 5.73, h: cardH, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
  s.addText("WHAT IT DOESN'T", { x: 7.3, y: cardY + 0.35, w: 5.0, h: 0.35, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, bold: true, color: CORAL, charSpacing: 3 });
  s.addText([
    { text: "GPS — useless indoors anyway", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "Magnetometer — no compass", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "Gyroscope — can't track turning", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "UWB / AoA antennas — no AirTag tricks", options: { bullet: { code: "25AA", indent: 14 } } },
  ], { x: 7.3, y: cardY + 0.85, w: 5.0, h: 2.4, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, color: INK, paraSpaceAfter: 10 });

  s.addText([
    { text: "The badge has no idea which way it is pointing. ", options: { color: INK, bold: true } },
    { text: "Everything that follows is about earning a direction anyway.", options: { color: MUTED } },
  ], { x: 0.7, y: 6.3, w: 11.9, h: 0.5, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 17 });
}

// =========================================================
// 4 · A DISTANCE IS ONLY A RING
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "WHAT WE CAN MEASURE");
  title(s, "A distance is only a ring");

  // diagram left
  const cx = 3.4, cy = 4.35, R = 2.35;
  ring(s, cx, cy, R, CORAL, "dash", 1.75);
  dot(s, cx, cy, 0.09, BLUE);
  label(s, "A (you)", cx - 0.5, cy - 0.55, 1.2, { color: BLUE, bold: true, align: "center", size: 13 });
  tri(s, cx + R * Math.cos(-0.45), cy + R * Math.sin(-0.45), 0.3, CORAL);
  [1.1, 2.4, 3.5, -1.9].forEach(a => tri(s, cx + R * Math.cos(a), cy + R * Math.sin(a), 0.26, CORAL, false));
  label(s, "|AB| = 5.0 m", cx - 0.85, cy + 0.35, 1.7, { mono: true, color: MUTED, align: "center", size: 13 });

  // right text
  s.addText([
    { text: "802.11mc FTM", options: { color: BLUE, bold: true } },
    { text: " measures round-trip time of a WiFi frame — distance to the other badge, accurate to a metre or two.", options: { color: INK } },
  ], { x: 7.0, y: 2.3, w: 5.6, h: 1.1, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 17 });
  s.addText([
    { text: "But with no compass and no gyro, that number is only a radius. ", options: { color: INK } },
    { text: "Your friend could be anywhere on the ring.", options: { color: CORAL, bold: true } },
  ], { x: 7.0, y: 3.6, w: 5.6, h: 1.1, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 17 });
  s.addText("What we need is a direction to walk.", {
    x: 7.0, y: 5.1, w: 5.6, h: 0.6, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 20, bold: true, color: INK,
  });
}

// =========================================================
// 5 · KEY INSIGHT
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "THE KEY INSIGHT");
  title(s, "We are not alone in the room");

  // diagram: A, B and shared APs with sight lines (kept clear of the bullets on the left)
  const ax = 5.7, ay = 4.6, bx = 11.9, by = 4.6;
  const aps = [[7.3, 2.7], [10.2, 3.0], [8.5, 6.0]];
  aps.forEach(p => {
    seg(s, ax, ay, p[0], p[1], BLUE, 1, "sysDash");
    seg(s, bx, by, p[0], p[1], CORAL, 1, "sysDash");
  });
  aps.forEach((p, i) => { sq(s, p[0], p[1], 0.24, INK); label(s, "AP" + (i + 1), p[0] - 0.4, p[1] - 0.55, 0.8, { align: "center", size: 12 }); });
  dot(s, ax, ay, 0.1, BLUE);
  label(s, "A", ax - 0.4, ay + 0.25, 0.8, { color: BLUE, bold: true, align: "center", size: 14 });
  tri(s, bx, by, 0.32, CORAL);
  label(s, "B", bx - 0.4, by + 0.3, 0.8, { color: CORAL, bold: true, align: "center", size: 14 });

  const pts = [
    ["5–8 access points are visible from anywhere in the venue", INK],
    ["They never move", INK],
    ["Both badges can see the same ones", INK],
  ];
  pts.forEach((p, i) => {
    s.addText(p[0], { x: 0.7, y: 2.15 + i * 0.9, w: 4.2, h: 0.85, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 16, color: p[1], bullet: { code: "25AA", indent: 14 } });
  });
  s.addText("Free, static reference points — shared by both badges.", {
    x: 0.7, y: 6.45, w: 11.9, h: 0.5, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 20, bold: true, color: GREEN,
  });
}

// =========================================================
// 6 · SECTION DIVIDER — THE MATH
// =========================================================
{
  const s = bgSlide();
  ring(s, 11.6, 6.6, 2.8, GHOST, "dash", 1);
  ring(s, 1.4, 0.7, 2.1, GHOST, "sysDash", 1);
  s.addText("HOW THE MATH WORKS", {
    x: 0.9, y: 2.9, w: 11.5, h: 1.1, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 48, bold: true, color: INK, align: "center", charSpacing: 3,
  });
  s.addText("two captures, one short walk", {
    x: 0.9, y: 4.1, w: 11.5, h: 0.5, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 18, color: BLUE, align: "center",
  });
}

// =========================================================
// 7 · STAGE 1: CHOOSE A FRAME
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "STAGE 1 · FIRST CAPTURE");
  title(s, "Choose a frame");

  // diagram: axis with A and B
  const ax = 2.2, bx = 10.9, y = 3.3;
  seg(s, ax, y, bx, y, GHOST, 1.5, "dash");
  dot(s, ax, y, 0.1, BLUE);
  label(s, "A = (0, 0)", ax - 0.8, y + 0.35, 1.6, { mono: true, color: BLUE, align: "center", size: 14 });
  tri(s, bx, y, 0.32, CORAL);
  label(s, "B = (d, 0)", bx - 0.8, y + 0.4, 1.6, { mono: true, color: CORAL, align: "center", size: 14 });
  label(s, "d = 5.0 m from FTM", 5.35, y - 0.5, 2.6, { mono: true, color: MUTED, align: "center", size: 13 });

  s.addText([
    { text: "We know the distance d between the badges — but not the bearing. So we build the only frame we can: ", options: { color: INK } },
    { text: "A at the origin, B on the x-axis, by construction.", options: { color: INK, bold: true } },
  ], { x: 0.7, y: 4.5, w: 11.9, h: 0.9, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 17 });
  s.addText([
    { text: "B scans the same APs and sends its list back over ESP-NOW. ", options: { color: INK } },
    { text: "16 APs × (6 B BSSID + 1 B RSSI) = 114 bytes — one packet.", options: { color: MUTED } },
  ], { x: 0.7, y: 5.5, w: 11.9, h: 0.9, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 17 });
}

// =========================================================
// 8 · TWO CIRCLES, TWO ANSWERS
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "STAGE 1 · FIRST CAPTURE");
  title(s, "Two circles, two answers");

  // diagram: circles about A and B intersecting at two squares
  const ax = 2.9, bx = 5.9, y = 4.3, rA = 1.85, rB = 2.1;
  seg(s, 1.0, y, 7.4, y, GHOST, 1, "dash");
  ring(s, ax, y, rA, BLUE);
  ring(s, bx, y, rB, CORAL);
  // intersection points (computed): xi from ax, yi
  const dAB = bx - ax;
  const xi = (dAB * dAB + rA * rA - rB * rB) / (2 * dAB);
  const yi = Math.sqrt(rA * rA - xi * xi);
  sq(s, ax + xi, y - yi, 0.22, INK, false);
  sq(s, ax + xi, y + yi, 0.22, INK, false);
  seg(s, ax + xi, y - yi, ax + xi, y + yi, GREEN, 1.75, "dash");
  dot(s, ax, y, 0.09, BLUE);
  tri(s, bx, y, 0.28, CORAL);
  label(s, "P₁", ax + xi + 0.2, y - yi - 0.1, 0.6, { color: INK, size: 13, bold: true });
  label(s, "P₂ (mirror)", ax + xi + 0.2, y + yi - 0.1, 1.6, { color: INK, size: 13, bold: true });

  // equations panel
  s.addShape("roundRect", { x: 8.0, y: 2.1, w: 4.63, h: 2.6, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
  s.addText("For each shared AP:", { x: 8.35, y: 2.35, w: 4.0, h: 0.35, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 14, color: MUTED });
  s.addText("x = (d² + dA² − dB²) / 2d\n\ny = ±√(dA² − x²)", {
    x: 8.35, y: 2.8, w: 4.0, h: 1.7, isTextBox: true, margin: 0,
    fontFace: "Courier New", fontSize: 17, color: GREEN, bold: true,
  });

  s.addText([
    { text: "RSSI path loss gives rough distances dA and dB to the AP. Circle about A, circle about B — they cross at ", options: { color: INK } },
    { text: "two points, mirror images across AB.", options: { color: INK, bold: true } },
  ], { x: 8.0, y: 5.0, w: 4.63, h: 1.7, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15 });
}

// =========================================================
// 9 · PARALLEL CHORDS
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "STAGE 1 · GEOMETRY CHECK");
  title(s, "All the chords are parallel");

  // diagram: axis + three candidate pairs with vertical chords
  const ax = 1.7, bx = 6.9, y = 4.2;
  seg(s, 1.2, y, 7.4, y, GHOST, 1, "dash");
  dot(s, ax, y, 0.09, BLUE);
  tri(s, bx, y, 0.28, CORAL);
  const pairs = [[3.0, 1.5], [4.4, 0.95], [5.6, 1.7]];
  pairs.forEach(p => {
    sq(s, p[0], y - p[1], 0.2, INK, false);
    sq(s, p[0], y + p[1], 0.2, INK, false);
    seg(s, p[0], y - p[1], p[0], y + p[1], GREEN, 1.75, "dash");
  });
  label(s, "every chord ⊥ AB", 3.0, y + 2.15, 2.6, { mono: true, color: GREEN, size: 13, align: "center" });

  s.addText([
    { text: "Team hypothesis, verified: ", options: { color: GREEN, bold: true } },
    { text: "the two candidates of every AP are reflections across AB, so every chord is exactly perpendicular to AB — all of them parallel. Checked numerically: 90.00°, every time.", options: { color: INK } },
  ], { x: 8.0, y: 2.2, w: 4.63, h: 2.2, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15 });
  s.addText([
    { text: "The catch: ", options: { color: CORAL, bold: true } },
    { text: "we chose the frame that put AB on the x-axis. In the only frame we can build, “perpendicular to AB” is true by construction — it recovers the axis we picked, not a real-world direction.", options: { color: INK } },
  ], { x: 8.0, y: 4.55, w: 4.63, h: 2.2, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15 });
}

// =========================================================
// 10 · THE AMBIGUITY
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "STAGE 1 · THE LIMIT");
  title(s, "We know the axis — not the direction");

  // diagram: the A-B axis through A, with both senses equally plausible
  const ax = 4.2, y = 4.3;
  seg(s, 1.0, y, 7.4, y, GHOST, 1.5, "dash");
  dot(s, ax, y, 0.1, BLUE);
  label(s, "A", ax - 0.4, y + 0.3, 0.8, { color: BLUE, bold: true, align: "center", size: 14 });
  seg(s, ax + 0.35, y, ax + 1.9, y, MUTED, 2.5, "solid", true);
  seg(s, ax - 0.35, y, ax - 1.9, y, MUTED, 2.5, "solid", true);
  tri(s, 6.9, y, 0.3, CORAL, false);
  label(s, "B?", 6.5, y - 0.75, 0.8, { color: CORAL, bold: true, align: "center", size: 14 });
  tri(s, 1.5, y, 0.3, CORAL, false);
  label(s, "or B?", 1.05, y - 0.75, 0.9, { color: CORAL, bold: true, align: "center", size: 14 });

  s.addText([
    { text: "The chord midpoints pin the axis A should move along — the A–B line is known. ", options: { color: INK } },
    { text: "But both directions along it fit the data equally well: ", options: { color: INK } },
    { text: "toward B, or exactly away from it.", options: { color: CORAL, bold: true } },
  ], { x: 8.0, y: 2.1, w: 4.63, h: 1.9, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 16 });
  s.addText([
    { text: "Found while implementing: ", options: { color: CORAL, bold: true } },
    { text: "mirror B and every AP across the walk axis and every range is unchanged. It is a ", options: { color: INK } },
    { text: "symmetry of the data", options: { color: INK, bold: true } },
    { text: " — more APs cannot break it.", options: { color: INK } },
  ], { x: 8.0, y: 4.05, w: 4.63, h: 1.8, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15 });
  s.addText("Ranges alone can never choose a side.", {
    x: 8.0, y: 5.85, w: 4.63, h: 0.9, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 19, bold: true, color: INK,
  });
}

// =========================================================
// 11 · STAGE 2: WALK
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "STAGE 2 · SECOND CAPTURE");
  title(s, "Walk a few steps, measure again");

  // diagram: A1 -> A2 arrow, B, true APs solid, mirrors crossed
  const a1x = 2.0, a1y = 5.3, a2x = 2.9, a2y = 3.9, bx = 6.8, by = 4.6;
  seg(s, a1x, a1y, bx, by, GHOST, 1, "dash");
  dot(s, a1x, a1y, 0.07, GHOST);
  label(s, "A₁", a1x - 0.5, a1y + 0.2, 0.8, { color: MUTED, align: "center", size: 13 });
  seg(s, a1x, a1y, a2x, a2y, BLUE, 2.5, "solid", true);
  dot(s, a2x, a2y, 0.1, BLUE);
  label(s, "A₂", a2x - 0.7, a2y - 0.5, 0.8, { color: BLUE, bold: true, align: "center", size: 14 });
  tri(s, bx, by, 0.3, CORAL);
  label(s, "B", bx - 0.4, by + 0.3, 0.8, { color: CORAL, bold: true, align: "center", size: 14 });
  const truePts = [[3.9, 2.9], [5.6, 3.3], [4.6, 6.0]];
  const mirrPts = [[3.9, 6.3], [5.6, 5.9], [4.6, 3.2]];
  truePts.forEach(p => sq(s, p[0], p[1], 0.2, INK));
  mirrPts.forEach(p => {
    sq(s, p[0], p[1], 0.2, GHOST, false);
    label(s, "×", p[0] - 0.15, p[1] - 0.17, 0.3, { color: CORAL, bold: true, size: 15, align: "center" });
  });
  seg(s, a2x, a2y, bx, by, GREEN, 2.5, "solid", true);

  const bl = [
    ["Screen prompts WALK ~5 STEPS; the accelerometer accumulates the baseline s", INK],
    ["Direction is “straight ahead” in your own body frame — a direction you can feel", INK],
    ["A₂ re-captures: ranges to B and a fresh AP scan", INK],
    ["Solve, then score: each common AP's predicted range to the solved B is compared with what B reported", INK],
  ];
  bl.forEach((p, i) => {
    s.addText(p[0], { x: 8.0, y: 2.1 + i * 0.95, w: 4.63, h: 0.9, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 14.5, color: p[1], bullet: { code: "25AA", indent: 12 } });
  });
  s.addText("x = (d₁² − d₂² + s²) / 2s        θ = atan2(√(d₁² − x²), x)        walk direction = +x",
    { x: 0.7, y: 6.6, w: 11.9, h: 0.45, isTextBox: true, margin: 0,
      fontFace: "Courier New", fontSize: 14, color: GREEN });
}

// =========================================================
// 12 · WHY THIS WINS
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "HONEST NUMBERS");
  title(s, "Why this beats spinning in place");

  const stats = [
    ["±29° → ±15–25°", "predicted bearing error vs a single spin — ground-truth run still pending", GREEN],
    ["114 bytes", "16 APs in one ESP-NOW packet", CORAL],
    ["< 4 m", "mean AP residual on the badge ⇒ the geometry is consistent", BLUE],
  ];
  stats.forEach((st, i) => {
    const x = 0.7 + i * 4.12;
    s.addShape("roundRect", { x, y: 2.15, w: 3.82, h: 2.3, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
    s.addText(st[0], { x: x + 0.3, y: 2.5, w: 3.22, h: 0.7, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 30, bold: true, color: st[2] });
    s.addText(st[1], { x: x + 0.3, y: 3.35, w: 3.22, h: 0.9, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 13.5, color: MUTED });
  });

  s.addText([
    { text: "Running on both badges — the TRIANGULATE page. ", options: { color: INK, bold: true } },
    { text: "Whether it beats the closing-rate compass already running is ", options: { color: INK } },
    { text: "not measured yet", options: { color: CORAL, bold: true } },
    { text: " — that is the next test, and the honest caveat.", options: { color: INK } },
  ], { x: 0.7, y: 4.85, w: 11.9, h: 0.8, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15.5 });
  s.addText([
    { text: "And the endgame: APs are static. ", options: { color: INK, bold: true } },
    { text: "Solve their positions once, cache the map — every later locate is a single scan. No walking, no spinning.", options: { color: GREEN } },
  ], { x: 0.7, y: 5.85, w: 11.9, h: 0.8, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 16.5 });
}

// =========================================================
// 13 · ERRORS & ASSUMPTIONS
// =========================================================
{
  const s = bgSlide();
  eyebrow(s, "ERRORS & ASSUMPTIONS");
  title(s, "What can go wrong");

  const cardY = 2.0, cardH = 4.35;
  // THE ERROR
  s.addShape("roundRect", { x: 0.7, y: cardY, w: 5.9, h: cardH, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
  s.addText("THE ERROR", { x: 1.1, y: cardY + 0.35, w: 5.1, h: 0.35, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, bold: true, color: CORAL, charSpacing: 3 });
  s.addText("30–50%", { x: 1.1, y: cardY + 0.8, w: 5.1, h: 0.75, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 44, bold: true, color: CORAL });
  s.addText("typical error in a single WiFi distance estimate — signals bounce off walls, fade, and interfere.", {
    x: 1.1, y: cardY + 1.7, w: 5.1, h: 0.85, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, color: INK,
  });
  s.addText([
    { text: "The fix: a rolling average. ", options: { color: GREEN, bold: true } },
    { text: "Each scan is noisy, but the noise is different every time — averaging successive scans smooths the jitter into a stable estimate.", options: { color: INK } },
  ], { x: 1.1, y: cardY + 2.7, w: 5.1, h: 1.4, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15 });

  // THE ASSUMPTIONS
  s.addShape("roundRect", { x: 6.9, y: cardY, w: 5.73, h: cardH, rectRadius: 0.12, fill: { color: PANEL }, line: { type: "none" } });
  s.addText("THE ASSUMPTIONS", { x: 7.3, y: cardY + 0.35, w: 5.0, h: 0.35, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, bold: true, color: BLUE, charSpacing: 3 });
  s.addText([
    { text: "No bodies in the way — a person between the badges soaks up 10–20 dB and reads as extra distance", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "Not too much obstruction between them — walls and dense crowd stretch every apparent range", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "You walk roughly straight — a curved walk breaks the baseline", options: { bullet: { code: "25AA", indent: 14 }, breakLine: true } },
    { text: "Hallway-scale, not an AirTag — the screen says “LEFT or RIGHT: walk one way, warmer = correct”", options: { bullet: { code: "25AA", indent: 14 } } },
  ], { x: 7.3, y: cardY + 0.9, w: 5.0, h: 3.2, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15, color: INK, paraSpaceAfter: 14 });

  s.addText([
    { text: "When the assumptions break, the arrow drifts — the rolling average keeps it from lying confidently.", options: { color: MUTED } },
  ], { x: 0.7, y: 6.65, w: 11.9, h: 0.5, isTextBox: true, margin: 0, fontFace: "Arial", fontSize: 15.5 });
}

// =========================================================
// 14 · DEMO
// =========================================================
{
  const s = bgSlide();
  ring(s, W / 2, H / 2, 3.1, GREEN, "dash", 2);
  dot(s, W / 2 - 3.1, H / 2, 0.1, BLUE);
  tri(s, W / 2 + 3.1, H / 2, 0.3, CORAL);
  s.addText("DEMO", {
    x: 1.5, y: 2.65, w: 10.33, h: 2.2, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 110, bold: true, color: INK, align: "center", charSpacing: 10,
  });
  s.addText("live, on two badges", {
    x: 1.5, y: 4.9, w: 10.33, h: 0.5, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 18, color: GREEN, align: "center",
  });
}

// =========================================================
// 14 · QUESTIONS
// =========================================================
{
  const s = bgSlide();
  // motif: A finds B
  const y = 5.9;
  dot(s, 4.6, y, 0.1, BLUE);
  seg(s, 4.75, y, 8.35, y, GREEN, 2.5, "solid", true);
  tri(s, 8.6, y, 0.32, CORAL);
  s.addText("Questions?", {
    x: 1.5, y: 2.55, w: 10.33, h: 1.6, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 72, bold: true, color: INK, align: "center",
  });
  s.addText("Hack the North 2026 badge · custom ESP-IDF firmware · TRIANGULATE page, live on two badges", {
    x: 1.5, y: 6.55, w: 10.33, h: 0.4, isTextBox: true, margin: 0,
    fontFace: "Arial", fontSize: 13, color: MUTED, align: "center",
  });
}

pres.writeFile({ fileName: "badge-finder-slides.pptx" }).then(() => console.log("written"));
