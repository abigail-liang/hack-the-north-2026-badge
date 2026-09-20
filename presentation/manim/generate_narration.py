"""Generate the voiceover clips for ap_triangulation_manim.py.

Writes narr/{0..8}.wav (22.05 kHz mono) next to the Manim scene, which reads
their durations to pace each step.  Uses Microsoft's neural TTS via edge-tts
(free, needs network):  pip install edge-tts

Run from this directory:  python generate_narration.py
"""
import asyncio
import os
import subprocess

import edge_tts

VOICE = "en-US-AriaNeural"
RATE = "+15%"

TEXTS = [
    "The badge finder: turning a distance into a direction, with no compass and no gyro.",
    "Here's the room. You hold badge A. Your teammate, badge B, is five meters away. "
    "Around you are the building's WiFi access points. They never move, and both badges can see them.",
    "Badge A measures the distance to B precisely, using WiFi time of flight. But a distance "
    "is only a ring. B could be anywhere on it. What we need is a direction.",
    "So badge A scans. Received signal strength gives a rough distance to each access point, "
    "putting every access point somewhere on a dashed circle around A.",
    "Badge B scans too, and sends its list back over ESP-NOW. Now both sets of distances live "
    "in one picture: rough circles around A, and rough circles around B. But nothing yet "
    "connects the two badges.",
    "For each shared access point, the two circles intersect at two points. Mirror images of "
    "each other. Join each pair, and a pattern appears: the chords are all parallel, and their "
    "midpoints fall on a single line. That line is the line connecting A and B.",
    "But here's the catch. The reconstruction tells A which axis to move along: the A-B line. "
    "What it cannot tell is which of the two directions. This way, or exactly the opposite. "
    "Both fit the data equally well. Stage one alone cannot tell you which way to walk.",
    "Stage two: walk a few steps in any direction. The accelerometer counts your steps, and the "
    "direction is simply straight ahead. A direction you can feel. Measuring again from the new "
    "position, only one candidate in each mirror pair fits.",
    "Three anchors pin the map to your walk. The bearing to B becomes an angle off your own "
    "heading. About eighty degrees to your right. And since access points never move, the map "
    "can be cached. Next time, finding your friend takes a single scan.",
]


async def main():
    os.makedirs("narr", exist_ok=True)
    for i, text in enumerate(TEXTS):
        raw = f"narr/{i}.raw.mp3"
        await edge_tts.Communicate(text, VOICE, rate=RATE).save(raw)
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", raw,
             "-ar", "22050", "-ac", "1", f"narr/{i}.wav"],
            check=True,
        )
        os.remove(raw)
        print(f"narr/{i}.wav")


asyncio.run(main())
