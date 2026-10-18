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

const std::string& GetStatus();
const std::vector<std::string>& GetLog();

} // namespace Net
