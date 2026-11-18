"""Exercise the built-in Net interface across separately compiled Duck modules."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from queue import Queue
from threading import Thread

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--clang", required=True)
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="duck net ") as directory:
    def run(*command, success=True):
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=30)
        if success and result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result

    root = Path(directory)
    (root / "host.dk").write_text("""
TcpListener* Host()
{
    server : Net.ListenTcp("127.0.0.1", 0)
    return ::server
}
""")
    (root / "app.dk").write_text("""
int ReleasedPort()
{
    local : Net.ListenTcp("127.0.0.1", 0)
    if !local { return -1 }
    return local.Port()
}
Main()
{
    listener : Host()
    if !listener { return }
    missing : listener.Accept()
    Print(!missing && listener.AcceptStatus == 0)
    port : listener.Port()
    client : Net.ConnectTcp("localhost", port)
    if !client { return }
    while client.Status() == 0 { }
    Print(client.Status() == 1)
    while true {
        peer : listener.Accept()
        if listener.AcceptStatus < 0 { return }
        if !peer { continue }
        listener.Close()
        byte [] message = {0, 255, 42}
        byte [] buffer = {11, 22, 33}
        Print(peer.Read(buffer) == -2)
        int sent = 0
        while sent < message.Len {
            n : client.WriteAt(message, sent)
            if n == -1 { return }
            if n > 0 { sent += n }
        }
        int received = 0
        while received < buffer.Len {
            n : peer.ReadAt(buffer, received)
            if n == -1 || n == 0 { return }
            if n > 0 { received += n }
        }
        Print(buffer[0] == 0 && buffer[1] == 255 && buffer[2] == 42)
        peer.Close()
        peer.Close()
        Print(peer.Read(buffer) == -1)
        int eof = -2
        while eof == -2 { eof = client.Read(buffer) }
        Print(eof == 0)
        break
    }
    refused : Net.ConnectTcp("127.0.0.1", port)
    if !refused { return }
    while refused.Status() == 0 { }
    Print(refused.Status() == -1)
    bad : Net.ConnectTcp("localhost", 65536)
    Print(!bad)
    localPort : ReleasedPort()
    reopened : Net.ListenTcp("127.0.0.1", localPort)
    Print(reopened != null)
    pending : Net.ConnectTcp("localhost", port)
    if !pending { return }
    pending.Close()
    Print(pending.Status() == -1)
}
""")
    run(args.compiler, "--library", "--interface", "host.dki", "host.dk", "-o", "host.ll")
    run(args.compiler, "--import", "host.dki", "app.dk", "-o", "app.ll")
    run(args.clang, "host.ll", "app.ll", "-o", "net.exe")
    result = run(str(root / "net.exe"))
    assert result.stdout.splitlines() == ["1"] * 10, result.stdout

    (root / "invalid.dk").write_text("""
Main() {
    listener : Net.ListenTcp("127.0.0.1", 0)
    byte [] buffer = {1}
    listener.Write(buffer)
}
""")
    result = run(args.compiler, "invalid.dk", "-o", "invalid.ll", success=False)
    assert result.returncode and "Write" in result.stderr, result.stderr

    (root / "udp_host.dk").write_text("""
UdpSocket* Bind(string ip)
{
    socket : Net.BindUdp(ip, 0)
    return ::socket
}
""")
    (root / "udp.dk").write_text("""
Exchange(string ip, int family)
{
    receiver : Bind(ip)
    sender : Bind(ip)
    if !receiver || !sender { return }
    UdpEndpoint dest = Net.UdpAddress(ip, receiver.Port())
    UdpEndpoint source()
    byte [] data = {0, 255, 42}
    byte [] buffer = {4, 5, 6}
    byte [] empty
    Print(receiver.ReceiveFrom(buffer, source) == -2)
    Print(sender.SendTo(data, dest) == 3)
    Print(sender.SendTo(empty, dest) == 0)
    int n = -2
    while n == -2 { n = receiver.ReceiveFrom(buffer, source) }
    Print(n == 3 && buffer[0] == 0 && buffer[1] == 255 && buffer[2] == 42)
    Print(source.Family == family && source.Port == sender.Port())
    Print(receiver.SendTo(buffer, source) == 3)
    n = -2
    while n == -2 { n = sender.ReceiveFrom(data, source) }
    Print(n == 3 && source.Port == receiver.Port())
    n = -2
    while n == -2 { n = receiver.ReceiveFrom(empty, source) }
    Print(n == 0)
    Print(sender.SendTo(data, dest) == 3)
    byte [] small = {17}
    n = -2
    while n == -2 { n = receiver.ReceiveFrom(small, source) }
    Print(n == -3 && small[0] == 17)
    Print(receiver.ReceiveFrom(small, source) == -2)
    byte [] large
    for i : 0; i < 8192; i++ { large += (byte)(i % 251) }
    Print(sender.SendTo(large, dest) == 8192)
    large[8191] = 0
    n = -2
    while n == -2 { n = receiver.ReceiveFrom(large, source) }
    Print(n == 8192 && large[8191] == (byte)(8191 % 251))
    while large.Len <= 65527 { large += (byte)0 }
    Print(sender.SendTo(large, dest) == -3 && receiver.ReceiveFrom(buffer, source) == -2)
    receiver.Close()
    receiver.Close()
    Print(receiver.Port() == -1 && receiver.ReceiveFrom(buffer, source) == -1)
}
Main()
{
    Exchange("127.0.0.1", 4)
    Exchange("::1", 6)
    UdpEndpoint invalid = Net.UdpAddress("invalid", 123)
    Print(invalid.Family == 0)
    UdpEndpoint scoped = Net.UdpAddress("fe80::1%13", 321)
    Print(scoped.Family == 6 && scoped.ScopeId == 13 && scoped.Port == 321)
}
""")
    run(args.compiler, "--library", "--interface", "udp.dki", "udp_host.dk", "-o", "udp_host.ll")
    run(args.compiler, "--import", "udp.dki", "udp.dk", "-o", "udp.ll")
    run(args.clang, "udp_host.ll", "udp.ll", "-o", "udp.exe")
    result = run(str(root / "udp.exe"))
    assert result.stdout.splitlines() == ["1"] * 32, result.stdout
    (root / "events.dk").write_text("""
Signals
#{
    extern "C" void* CreateEventW(void* security, int manual, int initial, void* eventName)
    extern "C" int SetEvent(void* event)
    extern "C" int WaitForSingleObject(void* event, int timeout)
    extern "C" int CloseHandle(void* event)
    extern "C" uint GetCurrentThreadId()
    void* Connected
    void* Received
    void* Datagram
    void* Large
    void* Failed
    int Bytes
    uint Owner
}
Accepted(NetIncoming& incoming)
{
    client :: incoming.Connection
    byte [] bytes = {1, 2, 3, 4}
    client.Write(bytes)
}
Connected(int status)
{
    if status == 1 && Signals.GetCurrentThreadId() != Signals.Owner {
        Signals.SetEvent(Signals.Connected)
    }
}
Received(NetReceiveEvent& event)
{
    if event.Status == 2 && event.Data.Len == 2 { Signals.Bytes += event.Data[0] + event.Data[1] }
    if event.Status == 0 && Signals.Bytes == 10 { Signals.SetEvent(Signals.Received) }
}
UdpSocket* MakeReceiver(int expectedPort)
{
    receiver : Net.BindUdp("127.0.0.1", 0)
    int marker = 77
    receiver.OnReceive(@ (NetReceiveEvent& packet) {
        if packet.Status == 2049 && packet.Data[512] == marker && packet.Data[2048] == marker && packet.Sender.Port == expectedPort {
            Signals.SetEvent(Signals.Large)
        }
    })
    return ::receiver
}
Main()
{
    Signals.Owner = Signals.GetCurrentThreadId()
    Signals.Connected = Signals.CreateEventW(null, 1, 0, null)
    Signals.Received = Signals.CreateEventW(null, 1, 0, null)
    Signals.Datagram = Signals.CreateEventW(null, 1, 0, null)
    Signals.Large = Signals.CreateEventW(null, 1, 0, null)
    Signals.Failed = Signals.CreateEventW(null, 1, 0, null)
    config : Net.ReceiveSettings()
    config.TcpChunkBytes = 2
    config.SleepMs = 1
    Print(Net.SetReceiveSettings(config))
    config.SleepMs = 0
    Print(!Net.SetReceiveSettings(config))
    listener : Net.ListenTcp("127.0.0.1", 0)
    Print(listener.OnConnect(Accepted))
    client : Net.ConnectTcp("127.0.0.1", listener.Port())
    Print(client.OnConnect(Connected))
    Print(client.OnReceive(Received))
    Print(Signals.WaitForSingleObject(Signals.Connected, 5000) == 0)
    Print(Signals.WaitForSingleObject(Signals.Received, 5000) == 0)
    port : listener.Port()
    listener.Close()
    refused : Net.ConnectTcp("127.0.0.1", port)
    Print(refused.OnConnect(@ (int status) {
        if status == -1 { Signals.SetEvent(Signals.Failed) }
    }))
    Print(Signals.WaitForSingleObject(Signals.Failed, 5000) == 0)
    refused.Close()
    client.Close()
    socket : Net.BindUdp("127.0.0.1", 0)
    sender : Net.BindUdp("127.0.0.1", 0)
    receiver : MakeReceiver(sender.Port())
    UdpEndpoint target = Net.UdpAddress("127.0.0.1", receiver.Port())
    byte [] large
    for i : 0; i < 2049; i++ { large += (byte)77 }
    Print(sender.SendTo(large, target) == 2049)
    Print(Signals.WaitForSingleObject(Signals.Large, 5000) == 0)
    Print(receiver.ReceiveFrom(large, target) == -1)
    receiver.Close()
    int expected = sender.Port()
    Print(socket.OnReceive(@ (NetReceiveEvent& packet) {
        if packet.Sender.Port == expected && packet.Status == 0 && packet.Data.Len == 0 {
            socket.Close()
            Signals.SetEvent(Signals.Datagram)
        }
    }))
    Print(!socket.OnReceive(Received))
    byte [] empty
    UdpEndpoint dest = Net.UdpAddress("127.0.0.1", socket.Port())
    Print(sender.SendTo(empty, dest) == 0)
    Print(Signals.WaitForSingleObject(Signals.Datagram, 5000) == 0)
    socket.Close()
    Print(socket.Port() == -1 && !socket.OnReceive(Received))
    config.SleepMs = 60000
    Print(Net.SetReceiveSettings(config))
    idle : Net.BindUdp("127.0.0.1", 0)
    Print(idle.OnReceive(Received))
    idle.Close()
    Signals.CloseHandle(Signals.Large)
    Signals.CloseHandle(Signals.Failed)
    Signals.CloseHandle(Signals.Datagram)
    Signals.CloseHandle(Signals.Received)
    Signals.CloseHandle(Signals.Connected)
}
""")
    run(args.compiler, "events.dk", "-o", "events.ll")
    run(args.clang, "events.ll", "-o", "events.exe")
    result = run(str(root / "events.exe"))
    assert result.stdout.splitlines() == ["1"] * 19, result.stdout

    examples = Path(__file__).resolve().parents[1] / "Examples" / "Networking"
    for role in ("Server", "Client"):
        run(args.compiler, examples / "Ping.dk", examples / f"Ping{role}.dk", "-o", f"{role}.ll")
        run(args.clang, f"{role}.ll", "-o", f"{role}.exe")

    # Start the actual server first. Its first flushed line gives the OS-assigned port.
    with subprocess.Popen([root / "Server.exe"], cwd=root, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as server:
        try:
            ready = Queue()
            Thread(target=lambda: ready.put(server.stdout.readline()), daemon=True).start()
            port = int(ready.get(timeout=10))
            assert 0 < port <= 65535
            client = run(str(root / "Client.exe"), str(port))
            server_out, server_err = server.communicate(timeout=15)
            assert server.returncode == 0, server_err
            assert server_out.splitlines() == ["server received ping and sent pong"], server_out
            assert client.stdout.splitlines() == ["client sent ping", "client received pong"], client.stdout
            print(f"TCP loopback on port {port}: {server_out.strip()}; {client.stdout.strip()}")
        finally:
            if server.poll() is None:
                server.kill()
