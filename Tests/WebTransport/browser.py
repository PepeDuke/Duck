"""Run with an activated emsdk and a Python environment containing aioquic:
python Tests/WebTransport/browser.py --builder PATH --compiler PATH --browser CHROME
"""
import argparse
import asyncio
import base64
from contextlib import asynccontextmanager
import datetime
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from ipaddress import ip_address
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DatagramReceived, HeadersReceived, WebTransportStreamDataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


class Echo(QuicConnectionProtocol):
    http = None

    def reply(self, message):
        return message.data

    def quic_event_received(self, event):
        if isinstance(event, ProtocolNegotiated):
            self.http = H3Connection(self._quic, enable_webtransport=True)
        if self.http is None:
            return
        for message in self.http.handle_event(event):
            if isinstance(message, HeadersReceived):
                headers = dict(message.headers)
                accepted = headers.get(b":protocol") == b"webtransport" and headers.get(b":path") == b"/echo"
                self.http.send_headers(message.stream_id, [
                    (b":status", b"200" if accepted else b"404"),
                    (b"sec-webtransport-http3-draft", b"draft02"),
                ], end_stream=not accepted)
            elif isinstance(message, WebTransportStreamDataReceived):
                response = self.reply(message)
                if response is not None:
                    self._quic.send_stream_data(message.stream_id, response, end_stream=message.stream_ended)
            elif isinstance(message, DatagramReceived):
                self.http.send_datagram(message.stream_id, self.reply(message))
        self.transmit()


def certificate():
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(days=1))
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ip_address("127.0.0.1"))]), False)
            .sign(key, hashes.SHA256()))
    return cert, key, base64.b64encode(cert.fingerprint(hashes.SHA256())).decode()


@asynccontextmanager
async def reference_server(port):
    config = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN, max_datagram_frame_size=65536)
    config.certificate, config.private_key, pin = certificate()
    server = await serve("127.0.0.1", port, configuration=config, create_protocol=Echo)
    try:
        yield port, pin
    finally:
        server.close()


async def exercise(directory, browser, profile, expected, endpoint=reference_server):
    loop = asyncio.get_running_loop()
    outcome = loop.create_future()

    def report(message):
        if not outcome.done():
            outcome.set_result(message)

    class Page(SimpleHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/":
                return super().do_GET()
            page = """<meta charset="utf-8"><script>
// Pin only this fixture's ephemeral certificate; use the browser's real transport.
window.WebTransport = class extends WebTransport {
  constructor(url, options) {
    super(url, {...options, serverCertificateHashes: [{algorithm: 'sha-256',
      value: Uint8Array.from(atob('TEST_CERTIFICATE'), c => c.charCodeAt(0))}]});
  }
};
const report = message => fetch('/result', {method:'POST', body:String(message)});
window.onerror = message => report(message);
window.onunhandledrejection = event => report(event.reason);
var Module = {arguments: ['https://127.0.0.1:TEST_PORT/echo'], print: report, printErr: console.error};
</script><script src="Echo.js"></script>"""
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(page.replace("TEST_CERTIFICATE", pin).replace("TEST_PORT", str(server_port)).encode())

        def do_POST(self):
            message = self.rfile.read(int(self.headers["Content-Length"])).decode()
            self.send_response(204)
            self.end_headers()
            loop.call_soon_threadsafe(report, message)

    with ThreadingHTTPServer(("127.0.0.1", 0), partial(Page, directory=str(directory))) as http:
        port = http.server_port
        async with endpoint(port) as (server_port, pin):
            threading.Thread(target=http.serve_forever, daemon=True).start()
            try:
                with subprocess.Popen([browser, "--headless", "--disable-gpu", "--no-first-run",
                                       "--user-data-dir=" + str(profile),
                                       f"http://127.0.0.1:{port}/"], stderr=subprocess.DEVNULL) as process:
                    try:
                        result = await asyncio.wait_for(outcome, 30)
                        assert result == expected, result
                    finally:
                        process.terminate()
            finally:
                http.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for option in ("builder", "compiler", "browser"):
        parser.add_argument("--" + option, required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        root = Path(tmp)
        source = Path(__file__).resolve().parents[2] / "Examples/Projects/WebTransport"
        shutil.copyfile(source / "Echo.dkp", root / "Echo.dkp")
        for fixture, profile, expected in [(source / "Client.dk", "debug", "echo complete"),
                                           (Path(__file__).with_name("Events.dk"), "release", "events complete")]:
            shutil.copyfile(fixture, root / "Client.dk")
            subprocess.run([args.builder.resolve(), "build", root / "Echo.dkp", "--target", "wasm64",
                            "--profile", profile, "--compiler", args.compiler.resolve()], check=True)
            asyncio.run(exercise(root / ".duck/build/wasm64" / profile, args.browser, root / profile, expected))
        print("WebTransport stream, datagram and callback lifetime tests passed")

