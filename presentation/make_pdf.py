"""Render badge-finder-site.html to a 14-page PDF (one slide per page).

The live site draws its diagrams into a single canvas that travels between
slides, so a naive print would lose them.  Loading the page with `?print`
switches it into print mode: the slides become pages and each one gets its
own static canvas.  This script serves the directory (so relative assets and
a UTF-8 charset are correct), then drives headless Chrome's print-to-PDF.

    python make_pdf.py            ->  badge-finder-slideshow.pdf
"""
import functools
import http.server
import os
import socketserver
import subprocess
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = "badge-finder-site.html"
OUT = os.path.join(HERE, "badge-finder-slideshow.pdf")
PORT = 8749
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# The published artifact supplies its own <head>; for a local render we wrap
# the file so the charset is declared and the em-dashes survive.
WRAPPER = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"></head>
<body style="margin:0">
{body}
</body></html>"""


def main():
    with open(os.path.join(HERE, SRC), encoding="utf-8") as fh:
        body = fh.read()

    tmp = tempfile.NamedTemporaryFile(
        "w", suffix=".html", dir=HERE, delete=False, encoding="utf-8")
    tmp.write(WRAPPER.format(body=body))
    tmp.close()
    page = os.path.basename(tmp.name)

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=HERE)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), handler)
    httpd.allow_reuse_address = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    try:
        subprocess.run([
            CHROME, "--headless", "--disable-gpu",
            "--window-size=1280,720",          # so the two-column media query holds
            "--no-pdf-header-footer",
            "--virtual-time-budget=8000",      # let fonts and the canvases settle
            f"--print-to-pdf={OUT}",
            f"http://127.0.0.1:{PORT}/{page}?print",
        ], check=True, capture_output=True)
        print(OUT)
    finally:
        httpd.shutdown()
        os.unlink(tmp.name)


if __name__ == "__main__":
    main()
