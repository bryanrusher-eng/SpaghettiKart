#include "OnlineMenuBridge.h"
#include "port/net/Net.h"

#include <libultraship/libultraship.h>
#include <imgui.h>
#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // GetUserDefaultUILanguage (idioma de Windows para elegir el idioma por defecto)
#endif

namespace {
constexpr uint16_t kDefaultRelayPort = 7778;
std::string g_error;

uint16_t RelayPort() {
    int p = CVarGetInteger("gOnline.RelayPort", kDefaultRelayPort);
    if (p < 1) p = 1;
    if (p > 65535) p = 65535;
    return static_cast<uint16_t>(p);
}
} // namespace

extern "C" {

void OnlineMenu_GetInfo(OnlineMenuInfo* out) {
    Net::SetLanguage(OnlineMenu_GetLang()); // los textos de estado de la red siguen el idioma elegido
    std::memset(out, 0, sizeof(*out));
    Net::Role role = Net::GetRole();
    out->role = (role == Net::Role::Host) ? 1 : (role == Net::Role::Client ? 2 : 0);
    out->connected = Net::IsConnected() ? 1 : 0;
    out->relay = Net::IsRelay() ? 1 : 0;
    out->raceBusy = Net::RaceBusy() ? 1 : 0;
    out->waitingRace = (Net::IsWaitingForRace() || std::getenv("SK_ONLINE_TEST_WAITING") != nullptr) ? 1 : 0; // la variable es solo de pruebas (para ver la pantalla)
    out->maxPlayers = Net::GetMaxPlayers();
    out->cpuWins = Net::RaceCpuWins();
    std::snprintf(out->roomCode, sizeof(out->roomCode), "%s", Net::GetRoomCode().c_str());
    std::snprintf(out->status, sizeof(out->status), "%s", Net::GetStatus().c_str());
    if (out->role != 0) {
        for (const Net::PeerInfo& p : Net::GetPeers()) {
            if (out->numPeers >= 16) break;
            OnlineMenuPeer& d = out->peers[out->numPeers++];
            d.slot = p.slot;
            d.ping = static_cast<int>(p.pingMs);
            d.isSelf = p.isSelf ? 1 : 0;
            d.character = p.character;
            d.wins = p.wins;
            d.quality = p.quality;
            std::snprintf(d.name, sizeof(d.name), "%s", p.name.c_str());
        }
        // Pruebas: SK_ONLINE_TEST_FAKEPEERS=N rellena la lista de la sala con jugadores falsos hasta N (nombres largos, pings 50..400)
        // para ver como queda la disposicion con dos columnas.
        if (const char* f = std::getenv("SK_ONLINE_TEST_FAKEPEERS")) {
            const int want = std::atoi(f);
            while (out->numPeers < want && out->numPeers < 16) {
                OnlineMenuPeer& d = out->peers[out->numPeers];
                d.slot = out->numPeers;
                d.ping = 50 + out->numPeers * 29;
                d.isSelf = 0;
                d.character = out->numPeers % 8;
                d.wins = out->numPeers % 3;
                d.quality = out->numPeers % 3;
                std::snprintf(d.name, sizeof(d.name), "%s", (out->numPeers % 2) ? "MAMARDOXD" : "ZETA141");
                out->numPeers++;
            }
        }
    }
}

void OnlineMenu_RoomsStart(void) {
    Net::RoomSearchStart(CVarGetString("gOnline.Relay", ""), RelayPort());
}

void OnlineMenu_RoomsStop(void) {
    Net::RoomSearchStop();
}

void OnlineMenu_GetRooms(OnlineMenuRooms* out) {
    std::memset(out, 0, sizeof(*out));
    switch (Net::GetRoomSearchState()) {
        case Net::RoomSearchState::Searching: out->state = 1; break;
        case Net::RoomSearchState::Ready: out->state = 2; break;
        case Net::RoomSearchState::Failed: out->state = 3; break;
        default: out->state = 0; break;
    }
    for (const Net::RoomInfo& r : Net::GetRooms()) {
        if (out->count >= 20) break;
        OnlineMenuRoom& d = out->rooms[out->count++];
        std::snprintf(d.code, sizeof(d.code), "%s", r.code.c_str());
        std::snprintf(d.name, sizeof(d.name), "%s", r.name.c_str());
        d.players = r.players;
        d.maxPlayers = r.maxPlayers;
        d.racing = r.racing ? 1 : 0;
    }
    std::snprintf(out->error, sizeof(out->error), "%s", Net::GetRoomSearchError().c_str());
    std::snprintf(out->server, sizeof(out->server), "%s", CVarGetString("gOnline.Relay", ""));
}

int OnlineMenu_GetRoomVisible(void) {
    return CVarGetInteger("gOnline.RoomVisible", 1) != 0 ? 1 : 0;
}

void OnlineMenu_SetRoomVisible(int visible) {
    CVarSetInteger("gOnline.RoomVisible", visible ? 1 : 0);
    CVarSave();
}

int OnlineMenu_GetChaos(void) {
    return CVarGetInteger("gOnline.Chaos", 0) != 0 ? 1 : 0;
}

int OnlineMenu_GetBots(void) {
    return CVarGetInteger("gOnline.Bots", 1) != 0 ? 1 : 0;
}

void OnlineMenu_SetBots(int bots) {
    CVarSetInteger("gOnline.Bots", bots ? 1 : 0);
    CVarSave();
}

// Idioma: 0 = espanol, 1 = ingles. Si el jugador nunca lo eligio, se toma del idioma de Windows (espanol si lo es, ingles si no).
int OnlineMenu_GetLang(void) {
    int v = CVarGetInteger("gOnline.Lang", -1);
    if (v < 0) {
        v = 1;
#ifdef _WIN32
        if ((GetUserDefaultUILanguage() & 0xFF) == 0x0A) { // LANG_SPANISH
            v = 0;
        }
#endif
        CVarSetInteger("gOnline.Lang", v);
    }
    return v == 1 ? 1 : 0;
}

void OnlineMenu_SetLang(int lang) {
    CVarSetInteger("gOnline.Lang", lang == 1 ? 1 : 0);
    Net::SetLanguage(lang == 1 ? 1 : 0);
    CVarSave();
}

int OnlineMenu_GetLite(void) {
    return CVarGetInteger("gOnline.Lite", 0) != 0 ? 1 : 0;
}

// Modo ligero: pone los ajustes graficos mas ligeros (resolucion interna 1x, sin suavizado MSAA y 30 FPS) y, al quitarlo,
// devuelve los que tenia el jugador. Solo cambia el dibujado de ESTA PC: la simulacion es la misma, asi que no separa partidas.
// Medido: la configuracion pesada (1.73x, MSAA 4, 91 FPS) gastaba el doble de procesador que esta (38 % contra 17 % de un nucleo).
void OnlineMenu_SetLite(int on) {
    auto window = Ship::Context::GetRawInstance()->GetWindow();
    if (on) {
        if (CVarGetInteger("gOnline.LiteSaved", 0) == 0) {
            CVarSetFloat("gOnline.LiteResPrev", CVarGetFloat("gInternalResolution", 1.0f));
            CVarSetInteger("gOnline.LiteMsaaPrev", CVarGetInteger("gMSAAValue", 1));
            CVarSetInteger("gOnline.LiteFpsPrev", CVarGetInteger("gInterpolationFPS", 30));
            CVarSetInteger("gOnline.LiteSaved", 1);
        }
        CVarSetFloat("gInternalResolution", 1.0f);
        CVarSetInteger("gMSAAValue", 1);
        CVarSetInteger("gInterpolationFPS", 30);
    } else if (CVarGetInteger("gOnline.LiteSaved", 0) != 0) {
        CVarSetFloat("gInternalResolution", CVarGetFloat("gOnline.LiteResPrev", 1.0f));
        CVarSetInteger("gMSAAValue", CVarGetInteger("gOnline.LiteMsaaPrev", 1));
        CVarSetInteger("gInterpolationFPS", CVarGetInteger("gOnline.LiteFpsPrev", 30));
        CVarSetInteger("gOnline.LiteSaved", 0);
    }
    if (window != nullptr) {
        window->SetResolutionMultiplier(CVarGetFloat("gInternalResolution", 1.0f));
        window->SetMsaaLevel(CVarGetInteger("gMSAAValue", 1));
    }
    CVarSetInteger("gOnline.Lite", on ? 1 : 0);
    CVarSave();
}

int OnlineMenu_GetMaxPing(void) {
    int v = CVarGetInteger("gOnline.MaxPing", 0);
    return v < 0 ? 0 : (v > 2000 ? 2000 : v);
}

void OnlineMenu_SetMaxPing(int ms) {
    CVarSetInteger("gOnline.MaxPing", ms);
    Net::SetPingLimit(ms);
}

void OnlineMenu_SetChaos(int chaos) {
    CVarSetInteger("gOnline.Chaos", chaos ? 1 : 0);
    CVarSave();
}

void OnlineMenu_GetName(char* out, int size) {
    std::snprintf(out, static_cast<size_t>(size), "%s", CVarGetString("gOnline.Name", "Jugador"));
}

void OnlineMenu_SetName(const char* name) {
    CVarSetString("gOnline.Name", name);
    CVarSave(); // se guarda en disco: el nombre queda para la proxima vez que se abra el juego
}

int OnlineMenu_GetCharacter(void) {
    int c = CVarGetInteger("gOnline.Character", 0);
    if (c < 0 || c > 7) c = 0;
    if (Net::GetMyCharacter() != c) Net::SetMyCharacter(c);
    return c;
}

void OnlineMenu_SetCharacter(int c) {
    Net::SetMyCharacter(c);
    CVarSetInteger("gOnline.Character", c);
    CVarSave();
}

int OnlineMenu_ServerConfigured(void) {
    const char* r = CVarGetString("gOnline.Relay", "");
    return (r != nullptr && r[0] != '\0') ? 1 : 0;
}

const char* OnlineMenu_Host(int maxPlayers) {
    if (!OnlineMenu_ServerConfigured()) return "falta la direccion del servidor";
    Net::SetMaxPlayers(maxPlayers);
    Net::SetRoomVisible(OnlineMenu_GetRoomVisible() != 0);
    Net::SetPingLimit(OnlineMenu_GetMaxPing());
    g_error.clear();
    char name[32];
    OnlineMenu_GetName(name, sizeof(name));
    if (!Net::StartHostRelay(CVarGetString("gOnline.Relay", ""), RelayPort(), name, g_error)) {
        if (g_error.empty()) g_error = "no se pudo crear la sala";
        return g_error.c_str();
    }
    return nullptr;
}

const char* OnlineMenu_Join(const char* code) {
    if (!OnlineMenu_ServerConfigured()) return "falta la direccion del servidor";
    g_error.clear();
    char name[32];
    OnlineMenu_GetName(name, sizeof(name));
    if (!Net::StartJoinRelay(CVarGetString("gOnline.Relay", ""), RelayPort(), code, name, g_error)) {
        if (g_error.empty()) g_error = "no se pudo entrar a la sala";
        return g_error.c_str();
    }
    return nullptr;
}

void OnlineMenu_Leave(void) {
    Net::Disconnect();
}

int OnlineMenu_StartRace(int cup, int track, int cc) {
    Net::RaceParams p;
    p.cup = static_cast<uint8_t>(cup);
    p.track = static_cast<uint8_t>(track);
    p.cc = static_cast<uint8_t>(cc);
    p.delay = static_cast<uint8_t>(Net::SuggestDelay());
    p.chaos = static_cast<uint8_t>(OnlineMenu_GetChaos());
    p.bots = static_cast<uint8_t>(OnlineMenu_GetBots());
    p.seed = static_cast<uint16_t>(static_cast<int>(ImGui::GetTime() * 1000.0)) | 1;
    for (const Net::PeerInfo& pi : Net::GetPeers()) p.humans.push_back({ pi.slot, pi.character });
    return Net::HostStartRace(p) ? 1 : 0;
}

} // extern "C"
