#include "OnlineMenuBridge.h"
#include "port/net/Net.h"

#include <libultraship/libultraship.h>
#include <imgui.h>
#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include <cstdio>
#include <cstring>
#include <string>

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
    std::memset(out, 0, sizeof(*out));
    Net::Role role = Net::GetRole();
    out->role = (role == Net::Role::Host) ? 1 : (role == Net::Role::Client ? 2 : 0);
    out->connected = Net::IsConnected() ? 1 : 0;
    out->relay = Net::IsRelay() ? 1 : 0;
    out->raceBusy = Net::RaceBusy() ? 1 : 0;
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
            std::snprintf(d.name, sizeof(d.name), "%s", p.name.c_str());
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

void OnlineMenu_StartRace(int cup, int track, int cc) {
    Net::RaceParams p;
    p.cup = static_cast<uint8_t>(cup);
    p.track = static_cast<uint8_t>(track);
    p.cc = static_cast<uint8_t>(cc);
    p.delay = static_cast<uint8_t>(Net::SuggestDelay());
    p.seed = static_cast<uint16_t>(static_cast<int>(ImGui::GetTime() * 1000.0)) | 1;
    for (const Net::PeerInfo& pi : Net::GetPeers()) p.humans.push_back({ pi.slot, pi.character });
    Net::HostStartRace(p);
}

} // extern "C"
