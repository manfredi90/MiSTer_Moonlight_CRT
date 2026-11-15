#!/usr/bin/env python3
"""
A minimal fake Sunshine to test the menu without a PC: it answers serverinfo and
applist (HTTP 47989 and HTTPS 47984). It does not stream.

Usage: fake_sunshine.py <cert.pem> <key.pem> [--unpaired] [--running ID] [--https-dead]
  (like the real Sunshine, over HTTP it always answers PairStatus 0)
"""
import ssl
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer

cert, key = sys.argv[1], sys.argv[2]
paired = "--unpaired" not in sys.argv
running = 0
if "--running" in sys.argv:
    running = int(sys.argv[sys.argv.index("--running") + 1])

APPS = ["Desktop", "Steam Big Picture", "Retroarch", "Hollow Knight", "Celeste",
        "Hades", "Street Fighter 6", "Forza Horizon 5", "A game with a really very long name"]


def serverinfo(https=True):
    return ("<?xml version=\"1.0\" encoding=\"utf-8\"?><root status_code=\"200\">"
            "<hostname>FAKE</hostname><appversion>7.1.431.-1</appversion><GfeVersion>3.23.0.74</GfeVersion>"
            "<uniqueid>0123456789ABCDEF</uniqueid><HttpsPort>47984</HttpsPort><ExternalPort>47989</ExternalPort>"
            "<mac>00:00:00:00:00:00</mac><MaxLumaPixelsHEVC>0</MaxLumaPixelsHEVC><LocalIP>127.0.0.1</LocalIP>"
            "<ServerCodecModeSupport>1</ServerCodecModeSupport><PairStatus>%d</PairStatus>"
            "<currentgame>%d</currentgame><state>SUNSHINE_SERVER_%s</state><gputype>fake</gputype>"
            "<GsVersion>7.1.431.-1</GsVersion></root>" % (1 if (paired and https) else 0, running, "BUSY" if running else "FREE"))


def applist():
    apps = "".join("<App><IsHdrSupported>0</IsHdrSupported><AppTitle>%s</AppTitle><ID>%d</ID></App>" % (n, i + 1)
                   for i, n in enumerate(APPS))
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><root status_code=\"200\">%s</root>" % apps


class H(BaseHTTPRequestHandler):
    if "--sunshine-close" in sys.argv:
        protocol_version = "HTTP/1.1"  # reply without "Connection: close": curl keeps the connection
    def do_GET(self):
        global running
        path = self.path.split("?")[0]
        print("GET", self.path[:80], flush=True)
        if path == "/serverinfo":
            body = serverinfo(self.server.server_address[1] == 47984)
        elif path == "/applist":
            body = applist()
        elif path == "/cancel":
            running = 0
            body = "<root status_code=\"200\"><cancel>1</cancel></root>"
        elif path == "/pair":
            threading.Event().wait(3600)  # waits for the PIN forever
            return
        else:
            self.send_error(404)
            return
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        if "--sunshine-close" in sys.argv:
            self.close_connection = True  # like Sunshine: closes without saying so in the header

    def log_message(self, *a):
        pass

    def finish(self):
        super().finish()
        # like Sunshine: a single HTTPS thread, and after the reply it sends the
        # TLS close notify and waits for the client's
        if self.server.server_address[1] == 47984 and "--sunshine-close" in sys.argv:
            try:
                self.connection.unwrap()
            except Exception as e:
                print("unwrap:", e, flush=True)


http = ThreadingHTTPServer(("0.0.0.0", 47989), H)
https = (HTTPServer if "--sunshine-close" in sys.argv else ThreadingHTTPServer)(("0.0.0.0", 47984), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
ctx.verify_mode = ssl.CERT_NONE
https.socket = ctx.wrap_socket(https.socket, server_side=True)
threading.Thread(target=http.serve_forever, daemon=True).start()
print("Fake Sunshine on 47989/47984", flush=True)
if "--https-dead" in sys.argv:
    # the HTTPS port accepts connections but nobody answers (thread blocked)
    print("HTTPS blocked", flush=True)
    threading.Event().wait()
https.serve_forever()
