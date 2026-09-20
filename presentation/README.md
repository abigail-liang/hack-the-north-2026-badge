# Presentation materials

Explainers for the AP-triangulation finder design
(see [finder-triangulation-design.md](../finder-triangulation-design.md)).

| File | What it is |
|---|---|
| `badge-finder-site.html` | The presentation website: 14 full-screen slides driven by ↑/↓ keys, wheel, swipe or on-screen buttons. One persistent diagram travels from slide to slide and morphs in place — room → FTM ring → sight lines → circle intersection → parallel chords → the A–B axis ambiguity → the stage-2 walk → found. Self-contained; open in a browser. |
| `ap-triangulation.html` | Interactive step-by-step animation (8 steps, canvas) with an optional neural-TTS voiceover. Serve this directory over HTTP so it can load `narr/*.mp3` (e.g. `python -m http.server`). |
| `narr/*.mp3` | Voiceover clips for `ap-triangulation.html` (Microsoft `en-US-AriaNeural` via edge-tts). |
| `slides.js` | pptxgenjs generator for the PowerPoint deck (`node slides.js`). |
| `badge-finder-slides.pptx` | The generated 15-slide deck. |
| `manim/ap_triangulation_manim.py` | Manim CE scene for the narrated explainer video. Regenerate the voiceover first: `python manim/generate_narration.py` (writes `manim/narr/*.wav`), then `manim -qm ap_triangulation_manim.py APTriangulation` from `manim/`. |
| `badge-finder-triangulation.mp4` | The rendered video (1:55, 720p, narrated). |
