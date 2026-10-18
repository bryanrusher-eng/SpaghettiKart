#ifndef NET_BRIDGE_H
#define NET_BRIDGE_H

// Puente C <-> C++ entre el codigo del juego (C) y la capa de red (src/port/net/Net.cpp).
// No incluye cabeceras del juego a proposito.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NetBridgeRace {
    unsigned char cup;
    unsigned char track;
    unsigned char cc;
    unsigned char delay;
    unsigned short seed;
    int numHumans;
    int slot[16];
    int character[16];
} NetBridgeRace;

// Devuelve 1 una sola vez cuando hay una carrera online que cargar.
int NetBridge_PollRaceStart(NetBridgeRace* out);
void NetBridge_RaceBegin(void);
void NetBridge_RaceEnd(void);
// Fin de carrera acordado por todos (el anfitrion sigue sirviendo mandos unos segundos).
void NetBridge_RaceEndGraceful(void);
int NetBridge_RaceActive(void);
int NetBridge_RaceDelay(void);
int NetBridge_LocalSlot(void);
// Atiende la red durante una espera (Net::Tick + 1 ms de pausa).
void NetBridge_Pump(void);
void NetBridge_Submit(unsigned int tick, unsigned short buttons, signed char stickX, signed char stickY);
// Devuelve 1 si ya estan los mandos de todos para ese tick.
int NetBridge_GetFrame(unsigned int tick, unsigned short buttons[16], signed char stickX[16], signed char stickY[16]);
// Nombre del jugador de un slot ("" si no hay nadie).
const char* NetBridge_SlotName(int slot);
// Slots que ya salieron de la carrera (bit por slot).
unsigned NetBridge_DroppedMask(void);

#ifdef __cplusplus
}
#endif

#endif // NET_BRIDGE_H
