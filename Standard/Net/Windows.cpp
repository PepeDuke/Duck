#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <limits.h>
#include <wchar.h>

// The resolver and connection each hold a reference until they are finished.
// Closing during lookup releases the caller immediately.
struct Lookup {
    LONG references;
    HANDLE done;
    wchar_t *host;
    wchar_t service[6];
    ADDRINFOW *addresses;
};

static void releaseLookup(Lookup *job) {
    if (InterlockedDecrement(&job->references)) return;
    if (job->addresses) FreeAddrInfoW(job->addresses);
    if (job->done) CloseHandle(job->done);
    free(job->host);
    free(job);
    WSACleanup();
}

static void CALLBACK resolve(PTP_CALLBACK_INSTANCE, void *context) {
    auto job = static_cast<Lookup *>(context);
    ADDRINFOW hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    GetAddrInfoW(job->host, job->service, &hints, &job->addresses);
    SetEvent(job->done);
    releaseLookup(job);
}

static Lookup *startLookup(const wchar_t *host, int port) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return nullptr;
    auto job = static_cast<Lookup *>(calloc(1, sizeof(Lookup)));
    if (!job) {
        WSACleanup();
        return nullptr;
    }
    job->references = 2;
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    job->host = _wcsdup(host);
    _itow_s(port, job->service, 6, 10);
    if (job->done && job->host && TrySubmitThreadpoolCallback(resolve, job, nullptr)) return job;
    job->references = 1;
    releaseLookup(job);
    return nullptr;
}

enum WatchKind { TcpReceive = 1, Connected, Incoming, UdpReceive };
struct UdpEndpoint;
struct Handler {
    // DuckNetHandler has three func fields. Only the Duck thunks access them.
    void *closures[3];
    void (*dispatch)(void *, int, void *, int, UdpEndpoint *);
    void (*drop)(void *);
    bool finished;
};
struct ReceiveSettings { int sleepMs, tcpChunkBytes; };
static SRWLOCK settingsLock = SRWLOCK_INIT;
static ReceiveSettings settings{5, 4096};

extern "C" void duck_net_get_settings(ReceiveSettings *out) {
    AcquireSRWLockShared(&settingsLock);
    *out = settings;
    ReleaseSRWLockShared(&settingsLock);
}

extern "C" int duck_net_set_settings(const ReceiveSettings *value) {
    if (!value || value->sleepMs < 1 || value->sleepMs > 60000 ||
        value->tcpChunkBytes < 1 || value->tcpChunkBytes > 65536) return 0;
    AcquireSRWLockExclusive(&settingsLock);
    settings = *value;
    ReleaseSRWLockExclusive(&settingsLock);
    return 1;
}

struct SocketLock {
    SRWLOCK *lock;
    explicit SocketLock(SRWLOCK &value) : lock(&value) { AcquireSRWLockExclusive(lock); }
    ~SocketLock() { ReleaseSRWLockExclusive(lock); }
    SocketLock(const SocketLock &) = delete;
};

// One WSAStartup reference per live handle, including accepted connections.
struct NetSocket {
    SRWLOCK lock;
    LONG references;
    bool closed;
    HANDLE thread, wake;
    Handler handlers[5];
    Lookup *lookup;
    SOCKET socket;
    ADDRINFOW *addresses;
    ADDRINFOW *next;
    bool pending;
};

static NetSocket *allocate() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return nullptr;
    auto tcp = static_cast<NetSocket *>(calloc(1, sizeof(NetSocket)));
    if (tcp) { tcp->socket = INVALID_SOCKET; tcp->references = 1; }
    else WSACleanup();
    return tcp;
}

static thread_local bool onNetWorker = false;

static void releaseSocket(NetSocket *s) {
    if (InterlockedDecrement(&s->references)) return;
    if (s->thread) CloseHandle(s->thread);
    if (s->wake) CloseHandle(s->wake);
    free(s);
    WSACleanup();
}

extern "C" void duck_net_close(NetSocket *s) {
    if (!s) return;
    HANDLE thread;
    {
        SocketLock guard(s->lock);
        if (!s->closed) {
            s->closed = true;
            if (s->lookup) releaseLookup(s->lookup);
            if (s->addresses) FreeAddrInfoW(s->addresses);
            if (s->socket != INVALID_SOCKET) closesocket(s->socket);
            s->lookup = nullptr;
            s->addresses = s->next = nullptr;
            s->socket = INVALID_SOCKET;
            if (s->wake) SetEvent(s->wake);
        }
        thread = s->thread;
    }
    // A callback may close itself or another callback's socket.
    if (thread && !onNetWorker) WaitForSingleObject(thread, INFINITE);
}

extern "C" void duck_net_release(NetSocket *s) {
    if (!s) return;
    duck_net_close(s);
    releaseSocket(s);
}

static void finishResolution(NetSocket *tcp) {
    FreeAddrInfoW(tcp->addresses);
    tcp->addresses = tcp->next = nullptr;
}

static bool tryAddress(NetSocket *tcp, bool listener) {
    for (auto a = tcp->next; a; a = tcp->next) {
        tcp->next = a->ai_next;
        SOCKET s = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        if (!ioctlsocket(s, FIONBIO, &nonblocking)) {
            int size = static_cast<int>(a->ai_addrlen);
            if (listener ? !bind(s, a->ai_addr, size) && !listen(s, SOMAXCONN)
                         : !connect(s, a->ai_addr, size)) {
                tcp->socket = s;
                finishResolution(tcp);
                return true;
            }
            if (!listener && WSAGetLastError() == WSAEWOULDBLOCK) {
                tcp->socket = s;
                tcp->pending = true;
                return true;
            }
        }
        closesocket(s);
    }
    finishResolution(tcp);
    return false;
}

extern "C" NetSocket *duck_tcp_open(const wchar_t *host, int port, int listener) {
    if (!host || !*host || port < (listener ? 0 : 1) || port > 65535) return nullptr;
    NetSocket *tcp = allocate();
    if (!tcp) return nullptr;
    if (!listener) {
        tcp->lookup = startLookup(host, port);
        if (tcp->lookup) return tcp;
        duck_net_release(tcp);
        return nullptr;
    }
    ADDRINFOW hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    wchar_t service[6];
    _itow_s(port, service, 6, 10);
    if (!GetAddrInfoW(host, service, &hints, &tcp->addresses)) {
        tcp->next = tcp->addresses;
        if (tryAddress(tcp, true)) return tcp;
    }
    duck_net_release(tcp);
    return nullptr;
}

// 1 ready, 0 connecting, -1 closed or failed.
static int tcpStatus(NetSocket *tcp) {
    if (!tcp) return -1;
    if (tcp->lookup) {
        Lookup *job = tcp->lookup;
        if (WaitForSingleObject(job->done, 0) == WAIT_TIMEOUT) return 0;
        tcp->addresses = tcp->next = job->addresses;
        job->addresses = nullptr;
        tcp->lookup = nullptr;
        releaseLookup(job);
        if (!tcp->addresses || !tryAddress(tcp, false)) return -1;
    }
    if (tcp->socket == INVALID_SOCKET) return -1;
    if (!tcp->pending) return 1;
    fd_set writable, errors;
    FD_ZERO(&writable);
    FD_ZERO(&errors);
    FD_SET(tcp->socket, &writable);
    FD_SET(tcp->socket, &errors);
    timeval timeout{};
    int n = select(0, nullptr, &writable, &errors, &timeout);
    if (!n) return 0;
    tcp->pending = false;
    if (n > 0 && !FD_ISSET(tcp->socket, &errors)) {
        finishResolution(tcp);
        return 1;
    }
    closesocket(tcp->socket);
    tcp->socket = INVALID_SOCKET;
    if (!tryAddress(tcp, false)) return -1;
    return tcp->pending ? 0 : 1;
}

extern "C" int duck_tcp_status(NetSocket *tcp) {
    if (!tcp) return -1;
    SocketLock guard(tcp->lock);
    return tcpStatus(tcp);
}

extern "C" int duck_net_port(NetSocket *tcp) {
    if (!tcp) return -1;
    SocketLock guard(tcp->lock);
    sockaddr_storage address{};
    int size = sizeof(address);
    if (getsockname(tcp->socket, reinterpret_cast<sockaddr *>(&address), &size)) return -1;
    if (address.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
    return ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
}

static NetSocket *tcpAccept(NetSocket *listener, int *status) {
    *status = -1;
    if (!listener) return nullptr;
    SOCKET s = accept(listener->socket, nullptr, nullptr);
    if (s == INVALID_SOCKET) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) *status = 0;
        return nullptr;
    }
    NetSocket *client = allocate();
    if (!client) { closesocket(s); return nullptr; }
    client->socket = s;
    *status = 1;
    return client;
}

extern "C" NetSocket *duck_tcp_accept(NetSocket *listener, int *status) {
    *status = -1;
    if (!listener) return nullptr;
    SocketLock guard(listener->lock);
    if (listener->handlers[Incoming].dispatch) return nullptr;
    return tcpAccept(listener, status);
}

static int tcpTransfer(NetSocket *tcp, char *bytes, int size, int writing) {
    int state = tcpStatus(tcp);
    if (state <= 0) return state == 0 ? -2 : -1;
    int n = writing ? send(tcp->socket, bytes, size, 0) : recv(tcp->socket, bytes, size, 0);
    if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return -2;
    return n;
}

extern "C" int duck_tcp_transfer(NetSocket *tcp, char *bytes, int size, int writing) {
    if (!tcp) return -1;
    SocketLock guard(tcp->lock);
    if (!writing && tcp->handlers[TcpReceive].dispatch) return -1;
    return tcpTransfer(tcp, bytes, size, writing);
}

// Keep this value layout in sync with UdpEndpoint in Net.dk.
struct UdpEndpoint {
    int family;
    unsigned char address[16];
    int port;
    unsigned int scopeId;
};

static bool decodeAddress(const sockaddr_storage &storage, UdpEndpoint &endpoint) {
    UdpEndpoint value{};
    if (storage.ss_family == AF_INET) {
        const auto &addr = reinterpret_cast<const sockaddr_in &>(storage);
        value.family = 4;
        value.port = ntohs(addr.sin_port);
        memcpy(value.address, &addr.sin_addr, sizeof(addr.sin_addr));
    } else if (storage.ss_family == AF_INET6) {
        const auto &addr = reinterpret_cast<const sockaddr_in6 &>(storage);
        value.family = 6;
        value.port = ntohs(addr.sin6_port);
        value.scopeId = addr.sin6_scope_id;
        memcpy(value.address, &addr.sin6_addr, sizeof(addr.sin6_addr));
    } else return false;
    endpoint = value;
    return true;
}

static int encodeAddress(const UdpEndpoint &endpoint, sockaddr_storage &storage) {
    if (endpoint.port < 0 || endpoint.port > 65535) return 0;
    if (endpoint.family == 4) {
        auto &addr = reinterpret_cast<sockaddr_in &>(storage);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(endpoint.port));
        memcpy(&addr.sin_addr, endpoint.address, sizeof(addr.sin_addr));
        return sizeof(addr);
    }
    if (endpoint.family == 6) {
        auto &addr = reinterpret_cast<sockaddr_in6 &>(storage);
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(static_cast<u_short>(endpoint.port));
        addr.sin6_scope_id = endpoint.scopeId;
        memcpy(&addr.sin6_addr, endpoint.address, sizeof(addr.sin6_addr));
        return sizeof(addr);
    }
    return 0;
}

// InetPtonW works before Winsock startup. Parse numeric scope IDs for IPv6.
extern "C" int duck_udp_endpoint(const wchar_t *text, int port, UdpEndpoint *out) {
    if (!text || !out || port < 0 || port > 65535) return 0;
    UdpEndpoint value{};
    value.port = port;
    if (InetPtonW(AF_INET, text, value.address) == 1) value.family = 4;
    else {
        wchar_t ip[INET6_ADDRSTRLEN];
        const wchar_t *scope = wcschr(text, L'%');
        if (scope) {
            size_t length = scope - text;
            if (length >= INET6_ADDRSTRLEN || !scope[1]) return 0;
            wmemcpy(ip, text, length);
            ip[length] = 0;
            text = ip;
            for (++scope; *scope; ++scope) {
                if (*scope < L'0' || *scope > L'9') return 0;
                unsigned digit = *scope - L'0';
                if (value.scopeId > (UINT_MAX - digit) / 10) return 0;
                value.scopeId = value.scopeId * 10 + digit;
            }
        }
        if (InetPtonW(AF_INET6, text, value.address) != 1) return 0;
        value.family = 6;
    }
    *out = value;
    return 1;
}

extern "C" NetSocket *duck_udp_bind(const wchar_t *address, int port) {
    UdpEndpoint local{};
    if (!duck_udp_endpoint(address, port, &local)) return nullptr;
    NetSocket *handle = allocate();
    if (!handle) return nullptr;
    sockaddr_storage storage{};
    int size = encodeAddress(local, storage);
    handle->socket = socket(storage.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    u_long nonblocking = 1;
    if (handle->socket != INVALID_SOCKET &&
        !ioctlsocket(handle->socket, FIONBIO, &nonblocking) &&
        !bind(handle->socket, reinterpret_cast<sockaddr *>(&storage), size)) return handle;
    duck_net_release(handle);
    return nullptr;
}

static int udpResult(int count) {
    if (count != SOCKET_ERROR) return count;
    switch (WSAGetLastError()) {
        case WSAEWOULDBLOCK: return -2;
        case WSAEMSGSIZE: return -3;
        default: return -1;
    }
}

extern "C" int duck_udp_send(NetSocket *handle, char *data, int size, const UdpEndpoint *peer) {
    if (!handle || !peer || peer->port == 0) return -1;
    SocketLock guard(handle->lock);
    sockaddr_storage storage{};
    int length = encodeAddress(*peer, storage);
    if (!length) return -1;
    return udpResult(sendto(handle->socket, data, size, 0, reinterpret_cast<sockaddr *>(&storage), length));
}

static int udpReceive(NetSocket *handle, char *data, int size, UdpEndpoint *peer) {
    if (!handle || !peer) return -1;
    sockaddr_storage storage{};
    int length = sizeof(storage);
    int n = udpResult(recvfrom(handle->socket, data, size, 0, reinterpret_cast<sockaddr *>(&storage), &length));
    if (n >= 0 && !decodeAddress(storage, *peer)) return -1;
    return n;
}

extern "C" int duck_udp_receive(NetSocket *handle, char *data, int size, UdpEndpoint *peer) {
    if (!handle) return -1;
    SocketLock guard(handle->lock);
    if (handle->handlers[UdpReceive].dispatch) return -1;
    return udpReceive(handle, data, size, peer);
}

extern "C" void duck_net_copy(const char *data, int offset, char *dest, int count) {
    memcpy(dest, data + offset, count);
}

static bool active(const Handler &h) { return h.dispatch && !h.finished; }

// One worker per subscribed socket. Its reference owns all callback storage
// until dispatch and cleanup finish, including when a callback closes it.
static DWORD WINAPI receiveLoop(void *context) {
    onNetWorker = true;
    auto s = static_cast<NetSocket *>(context);
    char packet[65536];
    for (;;) {
        ReceiveSettings config;
        duck_net_get_settings(&config);
        int kind = 0, count = 0;
        void *data = packet;
        UdpEndpoint sender{};
        DWORD delay = INFINITE;
        {
            SocketLock guard(s->lock);
            if (s->closed) break;
            if (active(s->handlers[Connected])) {
                delay = config.sleepMs;
                count = tcpStatus(s);
                if (count != 0) {
                    kind = Connected;
                    s->handlers[kind].finished = true;
                }
            }
            if (!kind && active(s->handlers[Incoming])) {
                delay = config.sleepMs;
                data = tcpAccept(s, &count);
                if (data) kind = Incoming;
                else if (count < 0) s->handlers[Incoming].finished = true;
            }
            if (!kind && active(s->handlers[TcpReceive])) {
                delay = config.sleepMs;
                count = tcpTransfer(s, packet, config.tcpChunkBytes, 0);
                if (count != -2) {
                    kind = TcpReceive;
                    if (count <= 0) s->handlers[kind].finished = true;
                }
            }
            if (!kind && active(s->handlers[UdpReceive])) {
                delay = config.sleepMs;
                count = udpReceive(s, packet, sizeof(packet), &sender);
                if (count != -2) {
                    kind = UdpReceive;
                    if (count < 0) s->handlers[kind].finished = true;
                }
            }
        }
        if (kind) {
            Handler &h = s->handlers[kind];
            h.dispatch(h.closures, kind, data, count, kind == UdpReceive ? &sender : nullptr);
        } else WaitForSingleObject(s->wake, delay);
    }
    for (auto &h : s->handlers)
        if (h.drop) h.drop(h.closures);
    releaseSocket(s);
    return 0;
}

extern "C" int duck_net_watch(NetSocket *s, int kind, void *source,
                              void (*copy)(void *, void *),
                              void (*dispatch)(void *, int, void *, int, UdpEndpoint *),
                              void (*drop)(void *)) {
    if (!s || kind < TcpReceive || kind > UdpReceive) return 0;
    SocketLock guard(s->lock);
    Handler &h = s->handlers[kind];
    if (s->closed || h.dispatch) return 0;
    if (!s->thread) {
        if (!s->wake) s->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s->wake) return 0;
        InterlockedIncrement(&s->references);
        s->thread = CreateThread(nullptr, 0, receiveLoop, s, 0, nullptr);
        if (!s->thread) { releaseSocket(s); return 0; }
    }
    copy(h.closures, source);
    h.drop = drop;
    h.dispatch = dispatch;
    SetEvent(s->wake);
    return 1;
}
