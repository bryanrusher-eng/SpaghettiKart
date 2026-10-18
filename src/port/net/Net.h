#pragma once

// Capa de red de Spaghetti Kart Online (UDP directo, estilo sm64coopdx):
// un jugador hace de anfitrion (Host) y los demas se unen por IP y puerto.
// Este modulo no incluye cabeceras del juego a proposito (evita choques con winsock).

#include <cstdint>
#include <string>
#include <vector>

namespace Net {

enum class Role { None, Host, Client };

constexpr int kMaxPlayers = 16;
constexpr uint16_t kDefaultPort = 7777;

struct PeerInfo {
    int slot;           // 0 = anfitrion, 1..15 = clientes
    std::string name;
    uint32_t pingMs;    // 0 si es el propio jugador
    std::string addr;   // "ip:puerto" (vacio para el propio)
    bool isSelf;
    int character = 0;  // personaje elegido (0..7); varios jugadores pueden repetirlo
    int wins = 0;       // carreras ganadas en esta sala
};

// Crea una sala. Devuelve false y rellena err si falla (puerto ocupado, etc.).
bool StartHost(uint16_t port, const std::string& name, std::string& err);

// Se une a una sala. La conexion se completa de forma asincrona: mira GetStatus().
bool StartJoin(const std::string& host, uint16_t port, const std::string& name, std::string& err);

// Modo servidor (sin abrir puertos ni VPN): el servidor de rele da un codigo de sala.
bool StartHostRelay(const std::string& relayHost, uint16_t relayPort, const std::string& name, std::string& err);
bool StartJoinRelay(const std::string& relayHost, uint16_t relayPort, const std::string& code,
                    const std::string& name, std::string& err);
const std::string& GetRoomCode(); // codigo de la sala (vacio hasta que el servidor lo da)
bool IsRelay();                   // true si la sesion pasa por el servidor

void Disconnect();

// ---------------------------------------------------------------------------------------------
// Busqueda de salas en el servidor: no hace falta estar en una sala. Se pide la lista y se refresca sola
// cada pocos segundos hasta llamar a RoomSearchStop().
// ---------------------------------------------------------------------------------------------
struct RoomInfo {
    std::string code;    // codigo de la sala
    std::string name;    // nombre del anfitrion ("" si el anfitrion no lo manda)
    int players = 0;     // jugadores dentro (con el anfitrion)
    int maxPlayers = 16;
    bool racing = false; // carrera en curso: no admite a nadie
};
enum class RoomSearchState { Idle, Searching, Ready, Failed };
void RoomSearchStart(const std::string& relayHost, uint16_t relayPort);
void RoomSearchStop();
RoomSearchState GetRoomSearchState();
const std::vector<RoomInfo>& GetRooms();
const std::string& GetRoomSearchError();
// El anfitrion elige si su sala sale en la lista (false = oculta: solo se entra con el codigo). Antes de crearla.
void SetRoomVisible(bool visible);
bool GetRoomVisible();

// Hay que llamarla una vez por fotograma (lee el socket, envia latidos, gestiona timeouts).
void Tick();

Role GetRole();
bool IsConnected();   // Host: siempre true; Cliente: true cuando el anfitrion ya acepto
int GetLocalSlot();   // -1 si no hay sesion

// Lista de jugadores de la sala, incluido uno mismo.
std::vector<PeerInfo> GetPeers();

// Maximo de jugadores de la sala (2..16). El anfitrion lo elige antes de crearla;
// los clientes lo reciben del anfitrion.
void SetMaxPlayers(int n);
int GetMaxPlayers();

// Personaje del jugador local (0..7). En una sala se avisa al anfitrion; se usa al iniciar la carrera.
void SetMyCharacter(int c);
int GetMyCharacter();

const std::string& GetStatus();
const std::vector<std::string>& GetLog();

// ---------------------------------------------------------------------------------------------
// Carrera en red (lockstep): todas las PCs simulan la misma carrera y solo se intercambian los
// mandos. Cada fotograma (tick) de la carrera necesita los mandos de todos los humanos.
// ---------------------------------------------------------------------------------------------

struct Pad {
    uint16_t buttons = 0; // mismos bits que OSContPad.button
    int8_t stickX = 0;
    int8_t stickY = 0;
};

struct RaceHuman {
    int slot;      // 0..15
    int character; // personaje (0..7)
};

struct RaceParams {
    uint8_t cup = 0;     // copa (0..3)
    uint8_t track = 0;   // pista dentro de la copa (0..3)
    uint8_t cc = 1;      // 0=50cc, 1=100cc, 2=150cc, 3=extra
    uint16_t seed = 1;   // semilla del generador aleatorio del juego
    uint8_t delay = 4;   // retraso de mandos en fotogramas (a 30 fps, 4 = ~133 ms)
    std::vector<RaceHuman> humans;
};

// Anfitrion: avisa a todos los clientes y deja la carrera pendiente tambien en esta PC.
int SuggestDelay(); // retraso de mandos recomendado segun el ping de los jugadores
bool HostStartRace(const RaceParams& p);
// El juego la consulta cada fotograma: devuelve true una sola vez cuando hay una carrera que cargar.
bool PollRaceStart(RaceParams& out);
// El juego ya cargo la pista: arranca el intercambio de mandos (ticks 0..delay-1 salen neutros).
void RaceBegin();
void RaceEnd();
bool RaceActive();
// ---- Diagnostico (opcional: variable de entorno SK_NET_STATS=carpeta) ----
// Cada 5 s de carrera escribe una linea en <carpeta>/net_<rol>_slot<N>.csv con RTT, jitter, perdida, esperas, etc.
uint64_t NowUs();
// El juego la llama al aplicar cada tick de la carrera: 'waitUs' = tiempo que estuvo esperando los mandos de los demas.
void StatsApplied(uint32_t tick, uint32_t waitUs);

// Modo de latencia que pide el anfitrion al servidor: 0 = estable (margen amplio), 1 = equilibrada, 2 = rapida.
void SetLatencyMode(int mode);

// El anfitrion termina la carrera para todos (los demas vuelven al lobby) / un jugador sale solo de la carrera.
void RaceAbortAll();
void RaceLeaveSelf();

// Diagnostico: descripcion del estado de la carrera (fotograma esperado, ack, anillo, agregador).
std::string DebugRace(uint32_t wantedTick);

// Verdadero mientras se espera el retraso de entrada que calcula el rele (justo al empezar la carrera).
bool AggWaiting();

bool RaceBusy(); // carrera cargandose o en marcha
bool RacePending(); // hay un aviso de carrera por atender (aun no consumido por el juego)
// Fin de carrera acordado por todos: el anfitrion sigue sirviendo mandos unos segundos por si alguien va atrasado.
void RaceEndGraceful();
// Anfitrion: suma una victoria al jugador de ese slot (se reparte a todos con la lista de jugadores).
void RaceAddWin(int slot);
int RaceWins(int slot);
// Victorias de los CPU (todos juntos) en esta sala.
void RaceAddCpuWin();
int RaceCpuWins();
// Nombre que tenia el jugador de ese slot al empezar la carrera ("" si no era humano).
std::string RaceName(int slot);

// Jugadores que salieron durante la carrera (para la leyenda "X salio de la partida").
struct LeftInfo {
    std::string name;
    uint64_t ageMs; // hace cuanto salio
};
std::vector<LeftInfo> GetLeftEvents();
uint16_t RaceDroppedMask(); // bit por slot de los que ya no estan
uint16_t RaceHumanMask();
int RaceDelay();
// Mando local para el tick indicado (tick actual + delay).
void RaceSubmitLocal(uint32_t tick, const Pad& p);
// Devuelve true si ya estan los mandos de todos para ese tick; rellena out[0..15].
bool RaceGetFrame(uint32_t tick, Pad out[kMaxPlayers]);

} // namespace Net
