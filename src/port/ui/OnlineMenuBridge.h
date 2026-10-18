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
    int quality; // conexion hasta el servidor: 0 = estable, 1 = floja, 2 = muy floja
    char name[32];
} OnlineMenuPeer;

typedef struct OnlineMenuInfo {
    int role;      // 0 = sin sala, 1 = anfitrion, 2 = cliente
    int connected; // 1 si ya esta dentro de la sala
    int relay;     // 1 si la sala pasa por el servidor (hay codigo)
    int raceBusy;  // 1 si hay una carrera cargandose o en marcha
    int waitingRace; // 1 si llego en plena carrera y entrara solo en la siguiente
    int maxPlayers;
    char roomCode[16];
    char status[128];
    int cpuWins;
    int numPeers;
    OnlineMenuPeer peers[16];
} OnlineMenuInfo;

void OnlineMenu_GetInfo(OnlineMenuInfo* out);

// Busqueda de salas en el servidor (pantalla "UNIRSE A SALA").
typedef struct OnlineMenuRoom {
    char code[8];
    char name[32];
    int players;
    int maxPlayers;
    int racing; // carrera en curso
} OnlineMenuRoom;

typedef struct OnlineMenuRooms {
    int state; // 0 = sin busqueda, 1 = buscando, 2 = lista lista, 3 = fallo (ver error)
    int count;
    OnlineMenuRoom rooms[20];
    char error[96];
    char server[64]; // direccion del servidor, para mostrarla
} OnlineMenuRooms;

void OnlineMenu_RoomsStart(void);
void OnlineMenu_RoomsStop(void);
void OnlineMenu_GetRooms(OnlineMenuRooms* out);
// Si la sala que se crea sale en la lista (se guarda en CVars).
int OnlineMenu_GetRoomVisible(void);
void OnlineMenu_SetRoomVisible(int visible);
// Modo Chaos de la proxima carrera (lo decide el anfitrion; se guarda en CVars).
int OnlineMenu_GetChaos(void);
void OnlineMenu_SetChaos(int chaos);
// Bots (CPUs) de la proxima carrera: 1 = los puestos libres hasta 16 karts se llenan con CPUs, 0 = solo karts de jugadores.
int OnlineMenu_GetBots(void);
void OnlineMenu_SetBots(int bots);
// Idioma de los textos online (0 = espanol, 1 = ingles). La primera vez se elige segun el idioma de Windows.
int OnlineMenu_GetLang(void);
void OnlineMenu_SetLang(int lang);
// Modo ligero (solo de esta PC): ajustes graficos ligeros; al quitarlo vuelven los que tenia el jugador.
int OnlineMenu_GetLite(void);
void OnlineMenu_SetLite(int on);
// Ping maximo de la sala en ms (0 = sin limite). Lo decide el anfitrion; se guarda en CVars y se aplica en Net.
int OnlineMenu_GetMaxPing(void);
void OnlineMenu_SetMaxPing(int ms);

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
// Devuelve 1 si la carrera se inicia (o queda en espera a que cierre la anterior) y 0 si no se pudo iniciar.
int OnlineMenu_StartRace(int cup, int track, int cc);

#ifdef __cplusplus
}
#endif

#endif // ONLINE_MENU_BRIDGE_H
