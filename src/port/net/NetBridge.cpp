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

int NetBridge_RaceStartPending(void) { return Net::RacePending() ? 1 : 0; }

void NetBridge_RaceBegin(void) { Net::RaceBegin(); }

void NetBridge_RaceEnd(void) { Net::RaceEnd(); }

void NetBridge_RaceEndGraceful(void) { Net::RaceEndGraceful(); }

int NetBridge_RaceActive(void) { return Net::RaceActive() ? 1 : 0; }

int NetBridge_RaceDelay(void) {
    // justo al empezar, el rele manda el retraso de entrada de este jugador: se espera (milisegundos) antes del 1er mando
    for (int i = 0; i < 3000 && Net::AggWaiting(); i++) {
        Net::Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Net::RaceDelay();
}

int NetBridge_LocalSlot(void) { return Net::GetLocalSlot(); }

unsigned NetBridge_DroppedMask(void) { return Net::RaceDroppedMask(); }

unsigned long long NetBridge_NowUs(void) { return Net::NowUs(); }

#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
// Diagnostico: SK_ONLINE_RNGTRACE=archivo anota, por cada numero aleatorio de la SIMULACION, el tick y las funciones que
// lo pidieron. Comparando el archivo de dos PCs se ve en que funcion empiezan a diferir.
void NetBridge_RngTrace(unsigned int tick, const char* note) {
    static FILE* f = nullptr;
    static bool init = false;
    static HANDLE proc = nullptr;
    static unsigned long long calls = 0;

    if (!init) {
        init = true;
        const char* p = std::getenv("SK_ONLINE_RNGTRACE");
        if (p != nullptr) {
            f = std::fopen(p, "w");
            if (f != nullptr) {
                proc = GetCurrentProcess();
                SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
                SymInitialize(proc, nullptr, TRUE);
            }
        }
    }
    if (f == nullptr) {
        return;
    }
    if (note != nullptr) {
        std::fprintf(f, "# t%u %s\n", tick, note);
        return;
    }
    void* frames[8];
    USHORT n = CaptureStackBackTrace(2, 6, frames, nullptr);
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    std::fprintf(f, "%llu t%u", ++calls, tick);
    for (USHORT i = 0; i < n && i < 4; i++) {
        std::memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, reinterpret_cast<DWORD64>(frames[i]), &disp, sym)) {
            std::fprintf(f, " %s", sym->Name);
        } else {
            std::fprintf(f, " ?");
        }
    }
    std::fprintf(f, "\n");
    if ((calls % 64) == 0) {
        std::fflush(f);
    }
}
#else
void NetBridge_RngTrace(unsigned int tick, const char* note) { (void) tick; (void) note; }
#endif

void NetBridge_StatsApplied(unsigned int tick, unsigned int waitUs) { Net::StatsApplied(tick, waitUs); }

void NetBridge_RaceAbortAll(void) { Net::RaceAbortAll(); }

void NetBridge_RaceLeaveSelf(void) { Net::RaceLeaveSelf(); }

void NetBridge_DebugRace(unsigned int wantedTick, char* out, int outSize) {
    std::string s = Net::DebugRace(wantedTick);
    std::snprintf(out, static_cast<size_t>(outSize), "%s", s.c_str());
}

int NetBridge_Wins(int slot) { return Net::RaceWins(slot); }

void NetBridge_RaceWinner(int slot) { Net::RaceAddWin(slot); }

int NetBridge_CpuWins(void) { return Net::RaceCpuWins(); }

void NetBridge_RaceCpuWinner(void) { Net::RaceAddCpuWin(); }

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
