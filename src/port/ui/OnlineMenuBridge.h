#ifndef ONLINE_MENU_BRIDGE_H
#define ONLINE_MENU_BRIDGE_H

// Puente C <-> C++ para el menu Online que va dentro del juego (src/online_menu.c).
// No incluye cabeceras del juego a proposito.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnlineMenuPeer {
    int slot;
    int ping;
    int isSelf;
    int character;
    int wins;
    char name[32];
} OnlineMenuPeer;

typedef struct OnlineMenuInfo {
    int role;      // 0 = sin sala, 1 = anfitrion, 2 = cliente
    int connected; // 1 si ya esta dentro de la sala
    int relay;     // 1 si la sala pasa por el servidor (hay codigo)
    int raceBusy;  // 1 si hay una carrera cargandose o en marcha
    int maxPlayers;
    char roomCode[16];
    char status[128];
    int cpuWins;
    int numPeers;
    OnlineMenuPeer peers[16];
} OnlineMenuInfo;

void OnlineMenu_GetInfo(OnlineMenuInfo* out);

// Ajustes guardados (CVars gOnline.*).
void OnlineMenu_GetName(char* out, int size);
void OnlineMenu_SetName(const char* name);
int OnlineMenu_ServerConfigured(void);
int OnlineMenu_GetCharacter(void);
void OnlineMenu_SetCharacter(int c);

// Acciones. Devuelven NULL si salio bien o un texto con el motivo del fallo.
const char* OnlineMenu_Host(int maxPlayers);
const char* OnlineMenu_Join(const char* code);
void OnlineMenu_Leave(void);
void OnlineMenu_StartRace(int cup, int track, int cc);

#ifdef __cplusplus
}
#endif

#endif // ONLINE_MENU_BRIDGE_H
