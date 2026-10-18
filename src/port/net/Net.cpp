#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "Net.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t kInvalidSock = INVALID_SOCKET;
static inline void CloseSock(sock_t s) { closesocket(s); }
static inline int LastSockError() { return WSAGetLastError(); }
static const int kWouldBlock = WSAEWOULDBLOCK;
static const int kConnReset = WSAECONNRESET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static const sock_t kInvalidSock = -1;
static inline void CloseSock(sock_t s) { close(s); }
static inline int LastSockError() { return errno; }
static const int kWouldBlock = EWOULDBLOCK;
static const int kConnReset = ECONNRESET;
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <map>

namespace Net {
namespace {

constexpr uint32_t kMagic = 0x314E4B4D; // "MKN1"
constexpr uint16_t kProtocolVersion = 1;

enum PacketType : uint8_t {
    PKT_HELLO = 1,   // cliente -> host: version, nombre
    PKT_WELCOME = 2, // host -> cliente: slot asignado
    PKT_REJECT = 3,  // host -> cliente: motivo
    PKT_PING = 4,    // ambos: marca de tiempo
    PKT_PONG = 5,    // respuesta al ping, repite la marca
    PKT_ROSTER = 6,  // host -> clientes: lista de jugadores
    PKT_BYE = 7,     // despedida
};

// Protocolo del servidor de rele (ver server/relay_server.py)
constexpr uint32_t kRelayMagic = 0x3152464D; // "MFR1"
enum RelayType : uint8_t {
    R_CREATE = 1, R_CREATED = 2, R_JOIN = 3, R_JOINED = 4, R_DATA_UP = 5, R_DATA_FROM = 6,
    R_DATA_TO = 7, R_DATA_DOWN = 8, R_KEEPALIVE = 9, R_CLOSE = 10, R_ROOM_CLOSED = 11, R_ERROR = 12,
};
constexpr uint64_t kRelayKeepaliveMs = 2000;
constexpr uint64_t kRelayTimeoutMs = 9000;
constexpr uint64_t kRelayRetryMs = 500;

constexpr uint64_t kPingIntervalMs = 1000;
constexpr uint64_t kTimeoutMs = 6000;
constexpr uint64_t kHelloRetryMs = 500;
constexpr uint64_t kConnectTimeoutMs = 10000;
constexpr size_t kMaxLogLines = 200;
constexpr size_t kMaxName = 24;

// ---------- serializacion ----------
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void str(const std::string& s) {
        size_t n = std::min(s.size(), kMaxName);
        u8(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; i++) u8(static_cast<uint8_t>(s[i]));
    }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t i = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t len) : p(data), n(len) {}
    uint8_t u8() {
        if (i + 1 > n) { ok = false; return 0; }
        return p[i++];
    }
    uint16_t u16() { uint16_t a = u8(); uint16_t b = u8(); return static_cast<uint16_t>(a | (b << 8)); }
    uint32_t u32() { uint32_t a = u16(); uint32_t b = u16(); return a | (b << 16); }
    std::string str() {
        size_t len = u8();
        if (len > kMaxName || i + len > n) { ok = false; return std::string(); }
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
};

// ---------- estado ----------
struct Peer {
    sockaddr_in addr;
    int slot = 0;
    std::string name;
    uint64_t lastRecv = 0;
    uint64_t lastPingSent = 0;
    uint32_t ping = 0;
};

enum class ClientState { Idle, Connecting, Connected };

sock_t g_sock = kInvalidSock;
Role g_role = Role::None;
bool g_wsaReady = false;

std::string g_status = "Desconectado";
std::vector<std::string> g_log;
std::string g_myName;
int g_maxPlayers = kMaxPlayers; // sala: maximo de jugadores (host lo elige; cliente lo recibe)

// Host
std::map<std::string, Peer> g_peers; // clave "ip:puerto"
uint64_t g_lastRoster = 0;
bool g_rosterDirty = false;

// Modo servidor (rele)
enum class RelayPhase { None, Creating, Ready, Joining, Joined };
bool g_viaRelay = false;
sockaddr_in g_relay;
RelayPhase g_rphase = RelayPhase::None;
std::string g_roomCode;
uint64_t g_rStart = 0;
uint64_t g_rLastSend = 0;
uint64_t g_rLastKeep = 0;
uint64_t g_rLastRecv = 0;

// Cliente
ClientState g_cstate = ClientState::Idle;
sockaddr_in g_server;
uint64_t g_cStart = 0;
uint64_t g_lastHello = 0;
uint64_t g_lastRecv = 0;
uint64_t g_lastPingSent = 0;
uint32_t g_myPing = 0;
int g_mySlot = -1;
std::vector<PeerInfo> g_roster;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Log(const std::string& line) {
    g_log.push_back(line);
    if (g_log.size() > kMaxLogLines) {
        g_log.erase(g_log.begin(), g_log.begin() + (g_log.size() - kMaxLogLines));
    }
}

void SetStatus(const std::string& s) {
    g_status = s;
    Log(s);
}

std::string KeyOf(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, const_cast<in_addr*>(&a.sin_addr), ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

std::string SockErrorText(int code) {
    std::string t = "error de red " + std::to_string(code);
#ifdef _WIN32
    if (code == WSAEADDRINUSE) t += " (el puerto ya esta en uso)";
    if (code == WSAEACCES) t += " (acceso denegado)";
#else
    if (code == EADDRINUSE) t += " (el puerto ya esta en uso)";
#endif
    return t;
}

bool InitSockets() {
#ifdef _WIN32
    if (!g_wsaReady) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            return false;
        }
        g_wsaReady = true;
    }
#endif
    return true;
}

bool OpenSocket(uint16_t bindPort, std::string& err) {
    if (!InitSockets()) {
        err = "no se pudo iniciar winsock";
        return false;
    }
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) {
        err = SockErrorText(LastSockError());
        return false;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    // Evita que un ICMP "puerto inalcanzable" haga fallar la lectura del socket UDP.
    DWORD bytes = 0;
    BOOL off = FALSE;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(bindPort);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        err = SockErrorText(LastSockError());
        CloseSock(s);
        return false;
    }
    g_sock = s;
    return true;
}

void SendRaw(const sockaddr_in& to, const std::vector<uint8_t>& b) {
    if (g_sock == kInvalidSock) return;
    sendto(g_sock, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Direccion "virtual" de un cliente que llega por el rele (el puerto guarda su id).
sockaddr_in FakeAddr(uint16_t clientId) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(clientId);
    return a;
}

void SendRelay(RelayType t, const std::string& arg = std::string()) {
    Writer w;
    w.u32(kRelayMagic);
    w.u8(t);
    if (!arg.empty()) w.str(arg);
    SendRaw(g_relay, w.b);
}

void SendTo(const sockaddr_in& to, const Writer& w) {
    if (g_sock == kInvalidSock) return;
    if (g_viaRelay) {
        Writer o;
        o.u32(kRelayMagic);
        if (g_role == Role::Host) {
            o.u8(R_DATA_TO);
            o.u16(ntohs(to.sin_port)); // id del cliente en el rele
        } else {
            o.u8(R_DATA_UP);
        }
        o.b.insert(o.b.end(), w.b.begin(), w.b.end());
        SendRaw(g_relay, o.b);
        return;
    }
    SendRaw(to, w.b);
}

Writer Begin(PacketType t) {
    Writer w;
    w.u32(kMagic);
    w.u8(t);
    return w;
}

void SendPing(const sockaddr_in& to) {
    Writer w = Begin(PKT_PING);
    w.u32(static_cast<uint32_t>(NowMs() & 0xFFFFFFFFu));
    SendTo(to, w);
}

void SendPong(const sockaddr_in& to, uint32_t token) {
    Writer w = Begin(PKT_PONG);
    w.u32(token);
    SendTo(to, w);
}

void SendHello() {
    Writer w = Begin(PKT_HELLO);
    w.u16(kProtocolVersion);
    w.str(g_myName);
    SendTo(g_server, w);
    g_lastHello = NowMs();
}

int FreeSlot() {
    bool used[kMaxPlayers] = { false };
    used[0] = true; // el anfitrion
    for (auto& kv : g_peers) {
        if (kv.second.slot >= 0 && kv.second.slot < kMaxPlayers) used[kv.second.slot] = true;
    }
    for (int i = 1; i < g_maxPlayers && i < kMaxPlayers; i++) {
        if (!used[i]) return i;
    }
    return -1;
}

std::vector<PeerInfo> HostRoster() {
    std::vector<PeerInfo> r;
    r.push_back({ 0, g_myName, 0, std::string(), true });
    std::vector<const Peer*> ps;
    for (auto& kv : g_peers) ps.push_back(&kv.second);
    std::sort(ps.begin(), ps.end(), [](const Peer* a, const Peer* b) { return a->slot < b->slot; });
    for (const Peer* p : ps) r.push_back({ p->slot, p->name, p->ping, std::string(), false });
    return r;
}

void BroadcastRoster() {
    std::vector<PeerInfo> r = HostRoster();
    Writer w = Begin(PKT_ROSTER);
    w.u8(static_cast<uint8_t>(g_maxPlayers));
    w.u8(static_cast<uint8_t>(r.size()));
    for (auto& p : r) {
        w.u8(static_cast<uint8_t>(p.slot));
        w.str(p.name);
        w.u16(static_cast<uint16_t>(std::min<uint32_t>(p.pingMs, 65535)));
    }
    for (auto& kv : g_peers) SendTo(kv.second.addr, w);
    g_lastRoster = NowMs();
    g_rosterDirty = false;
}

void ResetState() {
    g_peers.clear();
    g_roster.clear();
    g_cstate = ClientState::Idle;
    g_mySlot = -1;
    g_myPing = 0;
    g_rosterDirty = false;
    g_role = Role::None;
    g_viaRelay = false;
    g_rphase = RelayPhase::None;
    g_roomCode.clear();
}

void HandleHostPacket(const sockaddr_in& from, uint8_t type, Reader& r, uint64_t now) {
    std::string key = KeyOf(from);
    auto it = g_peers.find(key);

    if (type == PKT_HELLO) {
        uint16_t ver = r.u16();
        std::string name = r.str();
        if (!r.ok) return;
        if (ver != kProtocolVersion) {
            Writer w = Begin(PKT_REJECT);
            w.str("version distinta");
            SendTo(from, w);
            return;
        }
        if (it != g_peers.end()) { // HELLO repetido: el WELCOME se perdio
            it->second.lastRecv = now;
            Writer w = Begin(PKT_WELCOME);
            w.u8(static_cast<uint8_t>(it->second.slot));
            SendTo(from, w);
            return;
        }
        int slot = FreeSlot();
        if (slot < 0) {
            Writer w = Begin(PKT_REJECT);
            w.str("sala llena");
            SendTo(from, w);
            return;
        }
        if (name.empty()) name = "Jugador" + std::to_string(slot);
        Peer p;
        p.addr = from;
        p.slot = slot;
        p.name = name;
        p.lastRecv = now;
        g_peers[key] = p;
        Writer w = Begin(PKT_WELCOME);
        w.u8(static_cast<uint8_t>(slot));
        SendTo(from, w);
        Log(name + " se unio (slot " + std::to_string(slot) + ", " + key + ")");
        g_rosterDirty = true;
        return;
    }

    if (it == g_peers.end()) return; // paquete de alguien que no esta en la sala
    Peer& p = it->second;
    p.lastRecv = now;

    switch (type) {
        case PKT_PING: {
            uint32_t token = r.u32();
            if (r.ok) SendPong(from, token);
            break;
        }
        case PKT_PONG: {
            uint32_t token = r.u32();
            if (r.ok) p.ping = static_cast<uint32_t>(NowMs() & 0xFFFFFFFFu) - token;
            break;
        }
        case PKT_BYE:
            Log(p.name + " salio de la sala");
            g_peers.erase(it);
            g_rosterDirty = true;
            break;
        default:
            break;
    }
}

void HandleClientPacket(const sockaddr_in& from, uint8_t type, Reader& r, uint64_t now) {
    if (g_cstate == ClientState::Idle) return;
    if (from.sin_addr.s_addr != g_server.sin_addr.s_addr || from.sin_port != g_server.sin_port) return;
    g_lastRecv = now;

    switch (type) {
        case PKT_WELCOME: {
            int slot = r.u8();
            if (!r.ok || g_cstate == ClientState::Connected) break;
            g_mySlot = slot;
            g_cstate = ClientState::Connected;
            SetStatus("Conectado a " + KeyOf(g_server) + " como jugador " + std::to_string(slot));
            break;
        }
        case PKT_REJECT: {
            std::string why = r.str();
            SetStatus("El anfitrion rechazo la conexion: " + why);
            CloseSock(g_sock);
            g_sock = kInvalidSock;
            ResetState();
            break;
        }
        case PKT_PING: {
            uint32_t token = r.u32();
            if (r.ok) SendPong(from, token);
            break;
        }
        case PKT_PONG: {
            uint32_t token = r.u32();
            if (r.ok) g_myPing = static_cast<uint32_t>(NowMs() & 0xFFFFFFFFu) - token;
            break;
        }
        case PKT_ROSTER: {
            int mx = r.u8();
            if (r.ok && mx >= 2 && mx <= kMaxPlayers) g_maxPlayers = mx;
            int n = r.u8();
            std::vector<PeerInfo> list;
            for (int i = 0; i < n && r.ok; i++) {
                PeerInfo pi;
                pi.slot = r.u8();
                pi.name = r.str();
                pi.pingMs = r.u16();
                pi.isSelf = (pi.slot == g_mySlot);
                if (pi.isSelf) pi.pingMs = g_myPing;
                list.push_back(pi);
            }
            if (r.ok) g_roster = list;
            break;
        }
        case PKT_BYE:
            SetStatus("El anfitrion cerro la sala");
            CloseSock(g_sock);
            g_sock = kInvalidSock;
            ResetState();
            break;
        default:
            break;
    }
}

bool ResolveV4(const std::string& host, uint16_t port, sockaddr_in& out, std::string& err) {
    if (!InitSockets()) {
        err = "no se pudo iniciar winsock";
        return false;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        err = "no se pudo resolver \"" + host + "\"";
        return false;
    }
    std::memset(&out, 0, sizeof(out));
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

void CloseAndReset() {
    if (g_sock != kInvalidSock) {
        CloseSock(g_sock);
        g_sock = kInvalidSock;
    }
    ResetState();
}

void HandleRelayPacket(uint8_t type, Reader& r, uint64_t now) {
    g_rLastRecv = now;
    switch (type) {
        case R_CREATED: {
            if (g_role != Role::Host || g_rphase != RelayPhase::Creating) break;
            std::string code = r.str();
            if (!r.ok || code.empty()) break;
            g_roomCode = code;
            g_rphase = RelayPhase::Ready;
            g_rLastKeep = now;
            SetStatus("Sala creada en el servidor. Codigo: " + code);
            break;
        }
        case R_JOINED: {
            if (g_role != Role::Client || g_rphase != RelayPhase::Joining) break;
            uint8_t ok = r.u8();
            std::string msg = r.str();
            if (ok) {
                g_rphase = RelayPhase::Joined;
                g_rLastKeep = now;
                g_cStart = now;
                Log("Sala encontrada: " + g_roomCode);
                SendHello();
            } else {
                SetStatus("No se pudo unir: " + msg);
                CloseAndReset();
            }
            break;
        }
        case R_DATA_FROM: {
            if (g_role != Role::Host) break;
            uint16_t cid = r.u16();
            if (!r.ok) break;
            Reader g(r.p + r.i, r.n - r.i);
            if (g.u32() != kMagic) break;
            uint8_t gt = g.u8();
            if (!g.ok) break;
            HandleHostPacket(FakeAddr(cid), gt, g, now);
            break;
        }
        case R_DATA_DOWN: {
            if (g_role != Role::Client) break;
            Reader g(r.p + r.i, r.n - r.i);
            if (g.u32() != kMagic) break;
            uint8_t gt = g.u8();
            if (!g.ok) break;
            HandleClientPacket(g_relay, gt, g, now);
            break;
        }
        case R_ROOM_CLOSED:
            if (g_role == Role::Client) {
                SetStatus("La sala se cerro");
                CloseAndReset();
            }
            break;
        case R_ERROR: {
            std::string m = r.str();
            SetStatus("Error del servidor: " + m);
            CloseAndReset();
            break;
        }
        default:
            break;
    }
}

} // namespace

// ---------- API publica ----------

bool StartHost(uint16_t port, const std::string& name, std::string& err) {
    Disconnect();
    if (!OpenSocket(port, err)) {
        SetStatus("No se pudo crear la sala: " + err);
        return false;
    }
    g_role = Role::Host;
    g_myName = name.empty() ? "Anfitrion" : name.substr(0, kMaxName);
    g_peers.clear();
    g_lastRoster = NowMs();
    SetStatus("Sala creada en el puerto " + std::to_string(port) + " (esperando jugadores)");
    return true;
}

bool StartJoin(const std::string& host, uint16_t port, const std::string& name, std::string& err) {
    Disconnect();

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (!InitSockets()) {
        err = "no se pudo iniciar winsock";
        return false;
    }
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        err = "no se pudo resolver \"" + host + "\"";
        SetStatus("No se pudo unir: " + err);
        return false;
    }
    std::memset(&g_server, 0, sizeof(g_server));
    g_server = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    g_server.sin_port = htons(port);
    freeaddrinfo(res);

    if (!OpenSocket(0, err)) { // puerto de origen cualquiera
        SetStatus("No se pudo unir: " + err);
        return false;
    }
    g_role = Role::Client;
    g_myName = name.empty() ? "Jugador" : name.substr(0, kMaxName);
    g_cstate = ClientState::Connecting;
    g_cStart = NowMs();
    g_lastRecv = g_cStart;
    g_lastPingSent = 0;
    g_mySlot = -1;
    g_roster.clear();
    SetStatus("Conectando a " + KeyOf(g_server) + "...");
    SendHello();
    return true;
}

bool StartHostRelay(const std::string& relayHost, uint16_t relayPort, const std::string& name, std::string& err) {
    Disconnect();
    if (!ResolveV4(relayHost, relayPort, g_relay, err)) {
        SetStatus("No se pudo crear la sala: " + err);
        return false;
    }
    if (!OpenSocket(0, err)) {
        SetStatus("No se pudo crear la sala: " + err);
        return false;
    }
    g_viaRelay = true;
    g_role = Role::Host;
    g_myName = name.empty() ? "Anfitrion" : name.substr(0, kMaxName);
    g_peers.clear();
    g_rphase = RelayPhase::Creating;
    g_rStart = NowMs();
    g_rLastSend = g_rStart;
    g_rLastRecv = g_rStart;
    g_lastRoster = g_rStart;
    SetStatus("Conectando con el servidor...");
    SendRelay(R_CREATE);
    return true;
}

bool StartJoinRelay(const std::string& relayHost, uint16_t relayPort, const std::string& code,
                    const std::string& name, std::string& err) {
    Disconnect();
    std::string c;
    for (char ch : code) {
        if (ch != ' ') c += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (c.empty()) {
        err = "escribe el codigo de la sala";
        SetStatus("No se pudo unir: " + err);
        return false;
    }
    if (!ResolveV4(relayHost, relayPort, g_relay, err)) {
        SetStatus("No se pudo unir: " + err);
        return false;
    }
    if (!OpenSocket(0, err)) {
        SetStatus("No se pudo unir: " + err);
        return false;
    }
    g_viaRelay = true;
    g_server = g_relay; // el cliente solo acepta paquetes del servidor
    g_role = Role::Client;
    g_myName = name.empty() ? "Jugador" : name.substr(0, kMaxName);
    g_roomCode = c;
    g_cstate = ClientState::Connecting;
    g_rphase = RelayPhase::Joining;
    g_rStart = NowMs();
    g_rLastSend = g_rStart;
    g_rLastRecv = g_rStart;
    g_cStart = g_rStart;
    g_lastRecv = g_rStart;
    g_lastPingSent = 0;
    g_mySlot = -1;
    g_roster.clear();
    SetStatus("Buscando la sala " + c + "...");
    SendRelay(R_JOIN, c);
    return true;
}

const std::string& GetRoomCode() { return g_roomCode; }

bool IsRelay() { return g_viaRelay; }

void Disconnect() {
    if (g_sock != kInvalidSock) {
        if (g_role == Role::Client) {
            Writer w = Begin(PKT_BYE);
            SendTo(g_server, w);
        } else if (g_role == Role::Host) {
            Writer w = Begin(PKT_BYE);
            for (auto& kv : g_peers) SendTo(kv.second.addr, w);
        }
        if (g_viaRelay) {
            SendRelay(R_CLOSE);
        }
        CloseSock(g_sock);
        g_sock = kInvalidSock;
    }
    if (g_role != Role::None) {
        SetStatus("Desconectado");
    }
    ResetState();
}

// Pruebas: si existe SK_ONLINE_AUTO ("host" o "join"), la sala se crea o se une sola poco despues
// de arrancar el juego. Variables: SK_ONLINE_RELAY, SK_ONLINE_PORT, SK_ONLINE_NAME, SK_ONLINE_CODE.
static void AutoStartFromEnv(uint64_t now) {
    static bool done = false;
    static uint64_t first = 0;
    if (done) return;
    const char* mode = std::getenv("SK_ONLINE_AUTO");
    if (mode == nullptr) {
        done = true;
        return;
    }
    if (first == 0) first = now;
    std::string m = mode;
    uint64_t delay = (m == "host") ? 4000 : 9000; // el cliente espera a que el anfitrion cree la sala
    if (now - first < delay) return;
    done = true;
    const char* relay = std::getenv("SK_ONLINE_RELAY");
    const char* port = std::getenv("SK_ONLINE_PORT");
    const char* name = std::getenv("SK_ONLINE_NAME");
    const char* code = std::getenv("SK_ONLINE_CODE");
    std::string err;
    uint16_t p = static_cast<uint16_t>(port ? std::atoi(port) : 7778);
    if (m == "host") {
        StartHostRelay(relay ? relay : "127.0.0.1", p, name ? name : "Anfitrion", err);
    } else if (m == "join") {
        StartJoinRelay(relay ? relay : "127.0.0.1", p, code ? code : "", name ? name : "Jugador", err);
    }
}

void Tick() {
    AutoStartFromEnv(NowMs());
    if (g_sock == kInvalidSock) return;
    uint64_t now = NowMs();

    // 1) leer todo lo que haya llegado
    uint8_t buf[1500];
    for (int n = 0; n < 128; n++) {
        sockaddr_in from;
        socklen_t flen = sizeof(from);
        int got = recvfrom(g_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                           &flen);
        if (got < 0) {
            int e = LastSockError();
            if (e == kConnReset) continue;
            break; // WOULDBLOCK u otro: no hay mas datos
        }
        if (got < 5) continue;
        if (g_viaRelay) {
            if (from.sin_addr.s_addr != g_relay.sin_addr.s_addr || from.sin_port != g_relay.sin_port) continue;
            Reader rr(buf, static_cast<size_t>(got));
            if (rr.u32() != kRelayMagic) continue;
            HandleRelayPacket(rr.u8(), rr, now);
            if (g_sock == kInvalidSock) return;
            continue;
        }
        Reader r(buf, static_cast<size_t>(got));
        if (r.u32() != kMagic) continue;
        uint8_t type = r.u8();
        if (g_role == Role::Host) {
            HandleHostPacket(from, type, r, now);
        } else if (g_role == Role::Client) {
            HandleClientPacket(from, type, r, now);
        }
        if (g_sock == kInvalidSock) return; // la sala se cerro mientras procesabamos
    }

    // 2) temporizadores
    if (g_viaRelay) {
        if (g_rphase == RelayPhase::Creating) {
            if (now - g_rStart > kConnectTimeoutMs) {
                SetStatus("No se pudo contactar con el servidor");
                CloseAndReset();
            } else if (now - g_rLastSend >= kRelayRetryMs) {
                SendRelay(R_CREATE);
                g_rLastSend = now;
            }
            return;
        }
        if (g_rphase == RelayPhase::Joining) {
            if (now - g_rStart > kConnectTimeoutMs) {
                SetStatus("No se pudo contactar con el servidor");
                CloseAndReset();
            } else if (now - g_rLastSend >= kRelayRetryMs) {
                SendRelay(R_JOIN, g_roomCode);
                g_rLastSend = now;
            }
            return;
        }
        if (now - g_rLastKeep >= kRelayKeepaliveMs) {
            SendRelay(R_KEEPALIVE);
            g_rLastKeep = now;
        }
        if (now - g_rLastRecv > kRelayTimeoutMs) {
            SetStatus("Se perdio la conexion con el servidor");
            CloseAndReset();
            return;
        }
    }
    if (g_role == Role::Host) {
        std::vector<std::string> dead;
        for (auto& kv : g_peers) {
            Peer& p = kv.second;
            if (now - p.lastRecv > kTimeoutMs) {
                dead.push_back(kv.first);
                continue;
            }
            if (now - p.lastPingSent >= kPingIntervalMs) {
                SendPing(p.addr);
                p.lastPingSent = now;
            }
        }
        for (auto& k : dead) {
            Log(g_peers[k].name + " perdio la conexion");
            g_peers.erase(k);
            g_rosterDirty = true;
        }
        if (g_rosterDirty || now - g_lastRoster >= 1000) {
            BroadcastRoster();
        }
        if (!dead.empty() || !g_peers.empty()) {
            g_status = (g_viaRelay ? "Sala " + g_roomCode + ": " : std::string("Sala activa: ")) +
                       std::to_string(g_peers.size() + 1) + " jugador(es)";
        } else {
            g_status = g_viaRelay ? "Sala " + g_roomCode + " creada (esperando jugadores)"
                                  : std::string("Sala creada (esperando jugadores)");
        }
    } else if (g_role == Role::Client) {
        if (g_cstate == ClientState::Connecting) {
            if (now - g_cStart > kConnectTimeoutMs) {
                SetStatus("No se pudo conectar: el anfitrion no responde");
                CloseSock(g_sock);
                g_sock = kInvalidSock;
                ResetState();
                return;
            }
            if (now - g_lastHello >= kHelloRetryMs) SendHello();
        } else if (g_cstate == ClientState::Connected) {
            if (now - g_lastRecv > kTimeoutMs) {
                SetStatus("Se perdio la conexion con el anfitrion");
                CloseSock(g_sock);
                g_sock = kInvalidSock;
                ResetState();
                return;
            }
            if (now - g_lastPingSent >= kPingIntervalMs) {
                SendPing(g_server);
                g_lastPingSent = now;
            }
        }
    }
}

Role GetRole() { return g_role; }

bool IsConnected() {
    if (g_role == Role::Host) return true;
    return g_role == Role::Client && g_cstate == ClientState::Connected;
}

int GetLocalSlot() {
    if (g_role == Role::Host) return 0;
    if (g_role == Role::Client) return g_mySlot;
    return -1;
}

std::vector<PeerInfo> GetPeers() {
    if (g_role == Role::Host) {
        return HostRoster();
    }
    if (g_role == Role::Client && g_cstate == ClientState::Connected) {
        if (g_roster.empty()) {
            return { { g_mySlot, g_myName, g_myPing, std::string(), true } };
        }
        return g_roster;
    }
    return {};
}

void SetMaxPlayers(int n) {
    if (g_role == Role::Client) return; // los clientes reciben el valor del anfitrion
    g_maxPlayers = std::max(2, std::min(n, kMaxPlayers));
}

int GetMaxPlayers() { return g_maxPlayers; }

const std::string& GetStatus() { return g_status; }

const std::vector<std::string>& GetLog() { return g_log; }

} // namespace Net
