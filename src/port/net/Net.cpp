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
constexpr uint16_t kProtocolVersion = 2;

enum PacketType : uint8_t {
    PKT_HELLO = 1,   // cliente -> host: version, nombre
    PKT_WELCOME = 2, // host -> cliente: slot asignado
    PKT_REJECT = 3,  // host -> cliente: motivo
    PKT_PING = 4,    // ambos: marca de tiempo
    PKT_PONG = 5,    // respuesta al ping, repite la marca
    PKT_ROSTER = 6,  // host -> clientes: lista de jugadores
    PKT_BYE = 7,     // despedida
    PKT_RACE_START = 8,   // host -> clientes: parametros de la carrera
    PKT_RACE_STARTED = 9, // cliente -> host: recibi el inicio
    PKT_INPUT = 10,       // cliente -> host: mandos de los proximos ticks + confirmacion
    PKT_FRAME = 11,       // host -> clientes: mandos de todos para uno o varios ticks
    PKT_CHARACTER = 12,   // cliente -> host: cambio de personaje
    PKT_LEFT = 13,        // host -> clientes: un jugador salio de la carrera
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
constexpr uint64_t kStallMs = 1000; // en carrera: un jugador callado mas de esto deja de bloquear a los demas
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
    int character = 0;
    uint64_t lastInput = 0; // ultima vez que llegaron mandos de este jugador en carrera
};

enum class ClientState { Idle, Connecting, Connected };

sock_t g_sock = kInvalidSock;
Role g_role = Role::None;
bool g_wsaReady = false;

std::string g_status = "Desconectado";
std::vector<std::string> g_log;
std::string g_myName;
int g_myCharacter = 0; // personaje del jugador local

// Avisos "X salio de la partida"
struct LeftEvent {
    int slot;
    std::string name;
    uint64_t at;
};
std::vector<LeftEvent> g_leftEvents;
uint16_t g_droppedMask = 0;
struct PendingLeft {
    int slot;
    std::string name;
    uint64_t nextSend;
    int sends;
};
std::vector<PendingLeft> g_pendingLeft; // host: avisos que se repiten unas veces por si se pierde el paquete
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

// ---------- carrera en red ----------
constexpr int kRing = 256;
struct RaceState {
    bool active = false;
    bool pending = false;
    bool starting = false; // host: reenviando RACE_START hasta que todos confirmen
    bool received = false; // cliente: ya recibio el inicio (ignora duplicados)
    RaceParams params;
    uint16_t humanMask = 0;
    int delay = 4;
    Pad frame[kRing][kMaxPlayers];
    uint32_t frameTick[kRing];
    bool frameReady[kRing];
    // host: mandos recibidos todavia sin completar
    Pad inputs[kRing][kMaxPlayers];
    uint32_t inputTick[kRing][kMaxPlayers];
    uint32_t hostNext = 0;                 // siguiente tick que el host debe completar
    uint32_t peerAck[kMaxPlayers] = { 0 }; // por slot: siguiente tick que ese cliente necesita
    bool started[kMaxPlayers] = { false }; // el cliente confirmo el inicio
    uint64_t lastStartSend = 0;
    // cliente
    uint32_t ackNext = 0;
    Pad sent[kRing];
    uint32_t sentTick[kRing];
    uint32_t lastSubmitted = 0;
    bool anySubmitted = false;
    uint64_t lastInputSend = 0;
    uint64_t lastFrameResend = 0;
} g_race;

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
    // Solo para pruebas: SK_NET_LOSS=20 descarta ~20 % de los paquetes enviados.
    static int lossPct = []() {
        const char* e = std::getenv("SK_NET_LOSS");
        return e ? std::atoi(e) : 0;
    }();
    if (lossPct > 0 && (std::rand() % 100) < lossPct) return;
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
    w.u8(static_cast<uint8_t>(g_myCharacter));
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
    r.push_back({ 0, g_myName, 0, std::string(), true, g_myCharacter });
    std::vector<const Peer*> ps;
    for (auto& kv : g_peers) ps.push_back(&kv.second);
    std::sort(ps.begin(), ps.end(), [](const Peer* a, const Peer* b) { return a->slot < b->slot; });
    for (const Peer* p : ps) r.push_back({ p->slot, p->name, p->ping, std::string(), false, p->character });
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
        w.u8(static_cast<uint8_t>(p.character));
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
    g_race = RaceState();
}

void WritePad(Writer& w, const Pad& p) {
    w.u16(p.buttons);
    w.u8(static_cast<uint8_t>(p.stickX));
    w.u8(static_cast<uint8_t>(p.stickY));
}

void WriteStart(Writer& w, const RaceParams& p) {
    w.u8(p.cup);
    w.u8(p.track);
    w.u8(p.cc);
    w.u16(p.seed);
    w.u8(p.delay);
    w.u8(static_cast<uint8_t>(p.humans.size()));
    for (const RaceHuman& h : p.humans) {
        w.u8(static_cast<uint8_t>(h.slot));
        w.u8(static_cast<uint8_t>(h.character));
    }
}

Pad ReadPad(Reader& r) {
    Pad p;
    p.buttons = r.u16();
    p.stickX = static_cast<int8_t>(r.u8());
    p.stickY = static_cast<int8_t>(r.u8());
    return p;
}

int HumanCount() {
    int n = 0;
    for (int i = 0; i < kMaxPlayers; i++) {
        if (g_race.humanMask & (1u << i)) n++;
    }
    return n;
}

void StoreFrame(uint32_t tick, const Pad* pads) {
    int i = tick % kRing;
    for (int s = 0; s < kMaxPlayers; s++) g_race.frame[i][s] = pads[s];
    g_race.frameTick[i] = tick;
    g_race.frameReady[i] = true;
}

bool FrameKnown(uint32_t tick) {
    int i = tick % kRing;
    return g_race.frameReady[i] && g_race.frameTick[i] == tick;
}

// Host: manda a un cliente los ticks que le faltan (hasta 8) terminando en 'last'.
void SendFrames(const Peer& p, uint32_t last) {
    // Siempre desde el tick mas antiguo que le falta al cliente (hasta 8 por paquete).
    uint32_t first = g_race.peerAck[p.slot];
    if (first > last) return;
    if (last > first + 7) last = first + 7;
    Writer w = Begin(PKT_FRAME);
    w.u32(first);
    w.u8(static_cast<uint8_t>(last - first + 1));
    for (uint32_t t = first; t <= last; t++) {
        int i = t % kRing;
        for (int s = 0; s < kMaxPlayers; s++) {
            if (g_race.humanMask & (1u << s)) WritePad(w, g_race.frame[i][s]);
        }
    }
    SendTo(p.addr, w);
}

void HostBroadcastFrame(uint32_t tick) {
    for (auto& kv : g_peers) SendFrames(kv.second, tick);
}

// Anota que un jugador salio de la carrera (una sola vez por slot).
void AddLeft(int slot, const std::string& name) {
    if (slot < 0 || slot >= kMaxPlayers) return;
    if (g_droppedMask & (1u << slot)) return;
    g_droppedMask |= static_cast<uint16_t>(1u << slot);
    g_leftEvents.push_back({ slot, name, NowMs() });
    Log(name + " salio de la partida");
}

// Host: un humano se fue (BYE o sin respuesta). Si hay carrera, se avisa a todos.
void HostPlayerLeft(const Peer& p) {
    if (!(g_race.active || g_race.pending) || !(g_race.humanMask & (1u << p.slot))) return;
    AddLeft(p.slot, p.name);
    g_pendingLeft.push_back({ p.slot, p.name, 0, 0 });
}

// Host: completa todos los ticks para los que ya estan los mandos de cada humano.
void HostTryComplete() {
    if (!g_race.active) return;
    for (int guard = 0; guard < kRing / 2; guard++) {
        uint32_t t = g_race.hostNext;
        int i = t % kRing;
        Pad pads[kMaxPlayers];
        bool ok = true;
        for (int s = 0; s < kMaxPlayers && ok; s++) {
            if (!(g_race.humanMask & (1u << s))) continue;
            bool connected = (s == 0);
            for (auto& kv : g_peers) {
                if (kv.second.slot == s) connected = true;
            }
            if (!connected) continue; // jugador que se fue: mando neutro
            if (g_race.inputTick[i][s] == t) {
                pads[s] = g_race.inputs[i][s];
            } else {
                // Si ya estaba jugando y lleva mas de 1 s sin mandar nada, se sigue sin esperarlo (mando neutro):
                // asi un jugador con mala conexion o que se fue no traba a los demas. Si vuelve, sus mandos
                // nuevos se aceptan otra vez.
                bool stalled = false;
                for (auto& kv : g_peers) {
                    if (kv.second.slot == s && kv.second.lastInput != 0 && NowMs() - kv.second.lastInput > kStallMs) {
                        stalled = true;
                    }
                }
                if (stalled) {
                    pads[s] = Pad();
                } else {
                    ok = false;
                }
            }
        }
        if (!ok) break;
        StoreFrame(t, pads);
        HostBroadcastFrame(t);
        g_race.hostNext++;
    }
}

// Cliente: manda al anfitrion el mando hasta el tick 'tick' (con los 5 anteriores por si se pierde algun paquete).
void SendClientInput(uint32_t tick) {
    uint32_t first = tick >= 5 ? tick - 5 : 0;
    if (first < static_cast<uint32_t>(g_race.delay)) first = static_cast<uint32_t>(g_race.delay);
    if (first > tick) first = tick;
    Writer w = Begin(PKT_INPUT);
    w.u32(g_race.ackNext);
    w.u32(first);
    w.u8(static_cast<uint8_t>(tick - first + 1));
    for (uint32_t t = first; t <= tick; t++) {
        int j = t % kRing;
        WritePad(w, g_race.sentTick[j] == t ? g_race.sent[j] : Pad());
    }
    SendTo(g_server, w);
    g_race.lastInputSend = NowMs();
}

void HandleHostPacket(const sockaddr_in& from, uint8_t type, Reader& r, uint64_t now) {
    std::string key = KeyOf(from);
    auto it = g_peers.find(key);

    if (type == PKT_HELLO) {
        uint16_t ver = r.u16();
        std::string name = r.str();
        int character = r.u8();
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
        p.character = (character >= 0 && character < 8) ? character : slot % 8;
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
            HostPlayerLeft(p);
            g_peers.erase(it);
            g_rosterDirty = true;
            break;
        case PKT_CHARACTER: {
            int c = r.u8();
            if (r.ok && c >= 0 && c < 8 && !g_race.active) {
                p.character = c;
                g_rosterDirty = true;
            }
            break;
        }
        case PKT_RACE_STARTED:
            if (g_race.starting) g_race.started[p.slot] = true;
            break;
        case PKT_INPUT: {
            if (!g_race.active) {
                g_race.started[p.slot] = true;
                break;
            }
            p.lastInput = now;
            uint32_t ack = r.u32();
            uint32_t firstTick = r.u32();
            int count = r.u8();
            if (!r.ok) break;
            g_race.started[p.slot] = true;
            if (ack > g_race.peerAck[p.slot]) g_race.peerAck[p.slot] = ack;
            for (int k = 0; k < count; k++) {
                Pad pad = ReadPad(r);
                if (!r.ok) break;
                uint32_t t = firstTick + k;
                int i = t % kRing;
                if (t < g_race.hostNext) continue; // ya completado
                g_race.inputs[i][p.slot] = pad;
                g_race.inputTick[i][p.slot] = t;
            }
            HostTryComplete();
            break;
        }
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
                pi.character = r.u8();
                pi.isSelf = (pi.slot == g_mySlot);
                if (pi.isSelf) pi.pingMs = g_myPing;
                list.push_back(pi);
            }
            if (r.ok) g_roster = list;
            break;
        }
        case PKT_LEFT: {
            int slot = r.u8();
            std::string name = r.str();
            if (r.ok && g_race.active) AddLeft(slot, name);
            break;
        }
        case PKT_BYE:
            if (g_race.active) {
                std::string hostName = "El anfitrion";
                for (const PeerInfo& pi : g_roster) {
                    if (pi.slot == 0) hostName = pi.name;
                }
                AddLeft(0, hostName);
            }
            SetStatus("El anfitrion cerro la sala");
            CloseSock(g_sock);
            g_sock = kInvalidSock;
            ResetState();
            break;
        case PKT_RACE_START: {
            if (g_cstate != ClientState::Connected) break;
            RaceParams rp;
            rp.cup = r.u8();
            rp.track = r.u8();
            rp.cc = r.u8();
            rp.seed = r.u16();
            rp.delay = r.u8();
            int n = r.u8();
            for (int k = 0; k < n && r.ok; k++) {
                RaceHuman h;
                h.slot = r.u8();
                h.character = r.u8();
                rp.humans.push_back(h);
            }
            if (!r.ok) break;
            if (!g_race.received) {
                g_race.received = true;
                g_race.params = rp;
                g_race.pending = true;
                Log("El anfitrion inicio la carrera");
            }
            Writer w = Begin(PKT_RACE_STARTED);
            SendTo(g_server, w);
            break;
        }
        case PKT_FRAME: {
            if (!g_race.active || g_cstate != ClientState::Connected) break;
            uint32_t first = r.u32();
            int count = r.u8();
            int humans = HumanCount();
            if (!r.ok) break;
            for (int k = 0; k < count && r.ok; k++) {
                Pad pads[kMaxPlayers];
                for (int s = 0; s < kMaxPlayers; s++) {
                    if (g_race.humanMask & (1u << s)) pads[s] = ReadPad(r);
                }
                if (!r.ok) break;
                (void) humans;
                uint32_t t = first + k;
                if (t >= g_race.ackNext) StoreFrame(t, pads);
            }
            while (FrameKnown(g_race.ackNext)) g_race.ackNext++;
            break;
        }
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

// Retraso de mandos (en ticks de 1/30 s) adecuado para el ping actual de los jugadores. Cada mando tiene que
// llegar al anfitrion y volver (por el rele), asi que hace falta cubrir el ping completo mas un colchon; con
// menos retraso la carrera se frena esperando datos.
int SuggestDelay() {
    uint32_t maxPing = 0;
    for (auto& kv : g_peers) maxPing = std::max(maxPing, kv.second.ping);
    int d = static_cast<int>((maxPing + 40) / 33) + 1;
    return std::max(4, std::min(12, d));
}

// Pruebas: SK_ONLINE_RACE_PLAYERS=N hace que el anfitrion inicie la carrera solo cuando haya N jugadores.
// Opcionales: SK_ONLINE_RACE_CUP, SK_ONLINE_RACE_TRACK, SK_ONLINE_RACE_CC, SK_ONLINE_DELAY.
static void AutoRaceFromEnv(uint64_t now) {
    static uint64_t reachedAt = 0;
    const char* n = std::getenv("SK_ONLINE_RACE_PLAYERS");
    if (n == nullptr || g_role != Role::Host || g_race.starting || g_race.active) return;
    int want = std::atoi(n);
    if (want < 2 || static_cast<int>(g_peers.size()) + 1 < want) {
        reachedAt = 0;
        return;
    }
    if (reachedAt == 0) reachedAt = now;
    if (now - reachedAt < 2500) return; // dejar que se estabilice la sala
    RaceParams p;
    const char* e;
    p.cup = static_cast<uint8_t>((e = std::getenv("SK_ONLINE_RACE_CUP")) ? std::atoi(e) : 0);
    p.track = static_cast<uint8_t>((e = std::getenv("SK_ONLINE_RACE_TRACK")) ? std::atoi(e) : 0);
    p.cc = static_cast<uint8_t>((e = std::getenv("SK_ONLINE_RACE_CC")) ? std::atoi(e) : 1);
    p.delay = static_cast<uint8_t>((e = std::getenv("SK_ONLINE_DELAY")) ? std::atoi(e) : SuggestDelay());
    p.seed = 1234;
    std::vector<int> slots;
    slots.push_back(0);
    for (auto& kv : g_peers) slots.push_back(kv.second.slot);
    std::sort(slots.begin(), slots.end());
    for (int s : slots) {
        int ch = s % 8;
        if (s == 0) {
            ch = g_myCharacter;
        } else {
            for (auto& kv : g_peers) {
                if (kv.second.slot == s) ch = kv.second.character;
            }
        }
        p.humans.push_back({ s, ch });
    }
    HostStartRace(p);
}

void Tick() {
    AutoStartFromEnv(NowMs());
    AutoRaceFromEnv(NowMs());
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

    // 1b) inicio de carrera: reenviar hasta que todos confirmen
    if (g_role == Role::Host && g_race.starting && !g_peers.empty()) {
        if (now - g_race.lastStartSend >= 500) {
            g_race.lastStartSend = now;
            Writer w = Begin(PKT_RACE_START);
            WriteStart(w, g_race.params);
            for (auto& kv : g_peers) {
                if (!g_race.started[kv.second.slot]) SendTo(kv.second.addr, w);
            }
        }
    }

    // 1c) reenvios periodicos durante la carrera (si se pierde un paquete, nadie se queda esperando)
    if (g_race.active) {
        if (g_role == Role::Client && g_cstate == ClientState::Connected && g_race.anySubmitted &&
            now - g_race.lastInputSend >= 100) {
            SendClientInput(g_race.lastSubmitted);
        }
        if (g_role == Role::Host && now - g_race.lastFrameResend >= 100) {
            g_race.lastFrameResend = now;
            for (auto& kv : g_peers) {
                if (g_race.hostNext > 0 && g_race.peerAck[kv.second.slot] < g_race.hostNext) {
                    SendFrames(kv.second, g_race.hostNext - 1);
                }
            }
        }
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
            HostPlayerLeft(g_peers[k]);
            g_peers.erase(k);
            g_rosterDirty = true;
        }
        if (g_rosterDirty || now - g_lastRoster >= 1000) {
            BroadcastRoster();
        }
        for (auto it = g_pendingLeft.begin(); it != g_pendingLeft.end();) {
            if (now >= it->nextSend) {
                Writer w = Begin(PKT_LEFT);
                w.u8(static_cast<uint8_t>(it->slot));
                w.str(it->name);
                for (auto& kv : g_peers) SendTo(kv.second.addr, w);
                it->nextSend = now + 400;
                it->sends++;
            }
            it = (it->sends >= 4) ? g_pendingLeft.erase(it) : it + 1;
        }
        if (g_race.active) HostTryComplete(); // por si un jugador callado ya paso de 1 s
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
                if (g_race.active) {
                    std::string hostName = "El anfitrion";
                    for (const PeerInfo& pi : g_roster) {
                        if (pi.slot == 0) hostName = pi.name;
                    }
                    AddLeft(0, hostName);
                }
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
            return { { g_mySlot, g_myName, g_myPing, std::string(), true, g_myCharacter } };
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

void SetMyCharacter(int c) {
    if (c < 0 || c > 7) return;
    g_myCharacter = c;
    if (g_role == Role::Host) {
        g_rosterDirty = true;
    } else if (g_role == Role::Client && g_cstate == ClientState::Connected) {
        Writer w = Begin(PKT_CHARACTER);
        w.u8(static_cast<uint8_t>(c));
        SendTo(g_server, w);
    }
}

int GetMyCharacter() { return g_myCharacter; }

const std::string& GetStatus() { return g_status; }

const std::vector<std::string>& GetLog() { return g_log; }

// ---------- carrera en red ----------

bool HostStartRace(const RaceParams& p) {
    if (g_role != Role::Host || g_race.active || g_race.pending) return false;
    g_race = RaceState();
    g_race.params = p;
    g_race.pending = true;
    g_race.starting = true;
    g_race.lastStartSend = 0;
    Log("Iniciando la carrera para " + std::to_string(g_peers.size() + 1) + " jugador(es)");
    return true;
}

bool PollRaceStart(RaceParams& out) {
    if (!g_race.pending) return false;
    out = g_race.params;
    g_race.pending = false;
    // se conserva el estado hasta RaceBegin()
    return true;
}

void RaceBegin() {
    const RaceParams params = g_race.params;
    uint64_t lastSend = g_race.lastStartSend;
    const bool starting = g_race.starting;
    const bool received = g_race.received;
    bool started[kMaxPlayers];
    std::memcpy(started, g_race.started, sizeof(started));
    g_race = RaceState();
    g_race.params = params;
    g_race.lastStartSend = lastSend;
    g_race.starting = starting;
    g_race.received = received;
    std::memcpy(g_race.started, started, sizeof(started));
    g_leftEvents.clear();
    g_pendingLeft.clear();
    g_droppedMask = 0;
    g_race.active = true;
    g_race.delay = std::max<int>(1, std::min<int>(params.delay, 12));
    g_race.humanMask = 0;
    for (const RaceHuman& h : params.humans) {
        if (h.slot >= 0 && h.slot < kMaxPlayers) g_race.humanMask |= static_cast<uint16_t>(1u << h.slot);
    }
    for (int i = 0; i < kRing; i++) {
        g_race.frameReady[i] = false;
        g_race.frameTick[i] = 0xFFFFFFFFu;
        for (int s = 0; s < kMaxPlayers; s++) g_race.inputTick[i][s] = 0xFFFFFFFFu;
        g_race.sentTick[i] = 0xFFFFFFFFu;
    }
    // los primeros 'delay' ticks salen neutros en todas las PCs (nadie ha podido pulsar aun)
    Pad neutral[kMaxPlayers];
    for (int t = 0; t < g_race.delay; t++) StoreFrame(static_cast<uint32_t>(t), neutral);
    g_race.hostNext = static_cast<uint32_t>(g_race.delay);
    g_race.ackNext = static_cast<uint32_t>(g_race.delay);
    for (int s = 0; s < kMaxPlayers; s++) g_race.peerAck[s] = static_cast<uint32_t>(g_race.delay);
    Log("Carrera cargada: " + std::to_string(HumanCount()) + " humano(s), retraso " + std::to_string(g_race.delay));
}

void RaceEnd() {
    if (g_race.active || g_race.pending) Log("Carrera terminada");
    g_race = RaceState();
}

bool RaceActive() { return g_race.active; }

bool RaceBusy() { return g_race.active || g_race.pending || g_race.starting; }

std::vector<LeftInfo> GetLeftEvents() {
    std::vector<LeftInfo> r;
    uint64_t now = NowMs();
    for (const LeftEvent& e : g_leftEvents) r.push_back({ e.name, now - e.at });
    return r;
}

uint16_t RaceDroppedMask() { return g_droppedMask; }

uint16_t RaceHumanMask() { return g_race.humanMask; }

int RaceDelay() { return g_race.delay; }

void RaceSubmitLocal(uint32_t tick, const Pad& pad) {
    if (!g_race.active) return;
    int slot = GetLocalSlot();
    if (slot < 0) return;
    int i = tick % kRing;
    g_race.sent[i] = pad;
    g_race.sentTick[i] = tick;
    g_race.lastSubmitted = tick;
    g_race.anySubmitted = true;
    if (g_role == Role::Host) {
        g_race.inputs[i][0] = pad;
        g_race.inputTick[i][0] = tick;
        HostTryComplete();
    }
    // el cliente manda su mando al instante (con los 5 anteriores por si se pierde algun paquete)
    if (g_role == Role::Client && g_cstate == ClientState::Connected) {
        SendClientInput(tick);
    }
}

bool RaceGetFrame(uint32_t tick, Pad out[kMaxPlayers]) {
    if (!g_race.active) return false;
    if (!FrameKnown(tick)) return false;
    int i = tick % kRing;
    for (int s = 0; s < kMaxPlayers; s++) out[s] = g_race.frame[i][s];
    return true;
}

} // namespace Net
