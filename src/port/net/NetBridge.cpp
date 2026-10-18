#include "NetBridge.h"
#include "Net.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {

int NetBridge_PollRaceStart(NetBridgeRace* out) {
    Net::RaceParams p;
    if (!Net::PollRaceStart(p)) return 0;
    std::memset(out, 0, sizeof(*out));
    out->cup = p.cup;
    out->track = p.track;
    out->cc = p.cc;
    out->delay = p.delay;
    out->seed = p.seed;
    out->numHumans = 0;
    for (const Net::RaceHuman& h : p.humans) {
        if (out->numHumans >= 16) break;
        out->slot[out->numHumans] = h.slot;
        out->character[out->numHumans] = h.character;
        out->numHumans++;
    }
    return 1;
}

void NetBridge_RaceBegin(void) { Net::RaceBegin(); }

void NetBridge_RaceEnd(void) { Net::RaceEnd(); }

void NetBridge_RaceEndGraceful(void) { Net::RaceEndGraceful(); }

int NetBridge_RaceActive(void) { return Net::RaceActive() ? 1 : 0; }

int NetBridge_RaceDelay(void) { return Net::RaceDelay(); }

int NetBridge_LocalSlot(void) { return Net::GetLocalSlot(); }

unsigned NetBridge_DroppedMask(void) { return Net::RaceDroppedMask(); }

int NetBridge_Wins(int slot) { return Net::RaceWins(slot); }

void NetBridge_RaceWinner(int slot) { Net::RaceAddWin(slot); }

void NetBridge_Pump(void) {
    Net::Tick();
#ifdef _WIN32
    // Mientras se espera a los demas jugadores el hilo del juego no vuelve al bucle principal: se atienden los
    // mensajes de la ventana para que Windows no la marque como "No responde".
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void NetBridge_Submit(unsigned int tick, unsigned short buttons, signed char stickX, signed char stickY) {
    Net::Pad p;
    p.buttons = buttons;
    p.stickX = stickX;
    p.stickY = stickY;
    Net::RaceSubmitLocal(tick, p);
}

int NetBridge_GetFrame(unsigned int tick, unsigned short buttons[16], signed char stickX[16], signed char stickY[16]) {
    Net::Pad pads[Net::kMaxPlayers];
    if (!Net::RaceGetFrame(tick, pads)) return 0;
    for (int i = 0; i < Net::kMaxPlayers; i++) {
        buttons[i] = pads[i].buttons;
        stickX[i] = pads[i].stickX;
        stickY[i] = pads[i].stickY;
    }
    return 1;
}

const char* NetBridge_SlotName(int slot) {
    static std::string name;
    name.clear();
    for (const Net::PeerInfo& p : Net::GetPeers()) {
        if (p.slot == slot) {
            name = p.name;
            break;
        }
    }
    if (name.empty()) {
        name = Net::RaceName(slot); // ya se fue de la sala: el nombre que tenia al empezar
    }
    return name.c_str();
}

} // extern "C"
