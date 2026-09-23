"""Render badge-finder-site.html to a one-page-per-slide PDF (one slide per page).

The live site draws its diagrams into a single canvas that travels between
slides, so a naive print would lose them.  Loading the page with `?print`
switches it into print mode: the slides become pages and each one gets its
own static canvas.  This script serves the directory -- declaring UTF-8, or
the em-dashes arrive mojibaked -- then drives headless Chrome's print-to-PDF.

    python make_pdf.py            ->  badge-finder-slideshow.pdf
"""
import functools
import http.server
import os
import socketserver
import subprocess
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = "badge-finder-site.html"
OUT = os.path.join(HERE, "badge-finder-slideshow.pdf")
PORT = 8749
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

class Handler(http.server.SimpleHTTPRequestHandler):
    """Serves the page itself, with the charset declared in the header.

    The published artifact supplies its own <head>, so the file has no
    <meta charset>.  Saying it here avoids writing a wrapped copy into the
    directory -- an earlier version did, and left temp files behind when a
    run was interrupted.
    """

    def guess_type(self, path):
        t = super().guess_type(path)
        return "text/html; charset=utf-8" if t == "text/html" else t

    def log_message(self, *a):
        pass


def main():
    page = SRC
    handler = functools.partial(Handler, directory=HERE)
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


if __name__ == "__main__":
    main()
