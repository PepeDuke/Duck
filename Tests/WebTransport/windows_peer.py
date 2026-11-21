"""Build a Windows x64 Duck server and run the wasm64 Duck client against it."""
import argparse
import asyncio
from contextlib import asynccontextmanager
import ctypes as c
from functools import partial
import os
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig
import tempfile

from aioquic.asyncio.server import QuicServer
from aioquic.h3.connection import H3_ALPN
from aioquic.h3.events import DatagramReceived
from aioquic.quic.configuration import QuicConfiguration

from browser import Echo, certificate, exercise


# WindowsServer.dk passes this table for the duration of serve_duck. All calls
# below run on one event-loop thread, and no borrowed packet outlives a call.
class TransportApi(c.Structure):
    _fields_ = [
        ("context", c.c_void_p),
        ("receive", c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_void_p, c.POINTER(c.c_int))),
        ("send", c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_void_p, c.c_int, c.c_int)),
        ("respond", c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_int, c.c_int, c.POINTER(c.c_int), c.c_void_p, c.c_int)),
        ("port", c.c_int),
    ]


class DuckProtocol(Echo):
    def __init__(self, *args, api, **kwargs):
        super().__init__(*args, **kwargs)
        self.api = api
        self.sums = {}

    def reply(self, message):
        datagram = isinstance(message, DatagramReceived)
        fin = not datagram and message.stream_ended
        total = c.c_int() if datagram else self.sums.setdefault(message.stream_id, c.c_int())
        data = c.create_string_buffer(message.data)
        count = self.api.respond(self.api.context, datagram, fin, c.byref(total), data, len(message.data))
        if fin:
            del self.sums[message.stream_id]
        if count == -2 and not datagram and not fin:
            return None
        if not 0 <= count <= len(data):
            raise RuntimeError(f"Duck rejected the application payload ({count})")
        return data.raw[:count]


class DuckSocket:
    def __init__(self, api):
        self.api = api
        self.buffer = c.create_string_buffer(65527)

    def sendto(self, data, peer):
        if peer[0] != "127.0.0.1":
            raise ValueError(f"unexpected peer {peer}")
        count = self.api.send(self.api.context, data, len(data), peer[1])
        if count not in (len(data), -2):
            raise OSError(f"Duck UDP send failed ({count})")

    def close(self):
        # Main owns the socket and closes it after the protocol shuts down.
        pass

    def receive(self, server):
        port = c.c_int()
        for _ in range(64):
            count = self.api.receive(self.api.context, self.buffer, c.byref(port))
            if count == -2:
                break
            if count < 0:
                raise OSError(f"Duck UDP receive failed ({count})")
            server.datagram_received(self.buffer[:count], ("127.0.0.1", port.value))


async def run_protocol(api):
    config = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN, max_datagram_frame_size=65536)
    config.certificate, config.private_key, pin = certificate()
    server = QuicServer(configuration=config, create_protocol=partial(DuckProtocol, api=api))
    socket = DuckSocket(api)
    server.connection_made(socket)
    print(f"READY {api.port} {pin}", flush=True)
    stop = asyncio.create_task(asyncio.to_thread(sys.stdin.buffer.readline))
    try:
        while not stop.done():
            socket.receive(server)
            await asyncio.sleep(0.001)
        await stop
    finally:
        server.close()


def serve_duck(address):
    asyncio.run(run_protocol(TransportApi.from_address(address)))


@asynccontextmanager
async def launch_server(exe, _http_port):
    # Load the same CPython DLL and packages as this test's interpreter.
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join((sys.base_prefix, env["PATH"]))
    env["PYTHONHOME"] = sys.base_prefix
    env["PYTHONPATH"] = os.pathsep.join((str(Path(__file__).resolve().parent), sysconfig.get_path("platlib")))
    process = await asyncio.create_subprocess_exec(
        str(exe), cwd=exe.parent, env=env, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
    try:
        line = await asyncio.wait_for(process.stdout.readline(), 15)
        fields = line.decode().split()
        if len(fields) != 3 or fields[0] != "READY":
            raise RuntimeError(f"Windows Duck server failed to start: {line!r}")
        yield int(fields[1]), fields[2]
        output, _ = await asyncio.wait_for(process.communicate(b"stop\n"), 10)
        if process.returncode or output.strip() != b"Windows Duck handled stream FIN and datagram":
            raise RuntimeError(f"Windows Duck server exited {process.returncode}: {output!r}")
    finally:
        if process.returncode is None:
            process.kill()
            await process.wait()


def test(builder, compiler, browser, root):
    fixtures = Path(__file__).resolve().parent
    # Use the matching import library, including for a venv installed elsewhere.
    py_lib = Path(sys.base_prefix) / "libs" / f"python{sys.version_info.major}{sys.version_info.minor}.lib"
    if not py_lib.is_file():
        raise RuntimeError(f"CPython development import library required: {py_lib}")
    shutil.copyfile(fixtures / "WindowsServer.dk", root / "WindowsServer.dk")
    shutil.copyfile(py_lib, root / "python.lib")
    shutil.copyfile(fixtures / "WindowsServer.dkp", root / "WindowsServer.dkp")
    source = fixtures.parents[1] / "Examples/Projects/WebTransport"
    for name in ("Echo.dkp", "Client.dk"):
        shutil.copyfile(source / name, root / name)
    for project, target in (("WindowsServer.dkp", "native"), ("Echo.dkp", "wasm64")):
        subprocess.run([builder, "build", root / project, "--target", target,
                        "--profile", "release", "--compiler", compiler], check=True)
    build = root / ".duck/build"
    native = build / "release/WindowsServer.exe"
    asyncio.run(exercise(build / "wasm64/release", browser, root / "chrome", "echo complete",
                         endpoint=partial(launch_server, native)))
    print("PASS: wasm64 browser client exchanged stream and datagram traffic with WindowsServer.exe")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("builder", "compiler", "browser"):
        parser.add_argument("--" + option, required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "win32" or c.sizeof(c.c_void_p) != 8:
        parser.error("run with x64 Windows CPython and an activated Emscripten SDK")
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        test(args.builder.resolve(), args.compiler.resolve(), args.browser.resolve(), Path(tmp))

