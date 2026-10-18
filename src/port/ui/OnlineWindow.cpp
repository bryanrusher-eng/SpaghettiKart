#include "OnlineWindow.h"
#include "port/net/Net.h"

#include <imgui.h>
#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void Online_OpenMenu(void) {
    // Lo llama el menu del juego al pulsar Z en GAME SELECT: muestra la ventana Online.
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    auto window = gui->GetGuiWindow("Online");
    if (window != nullptr) {
        window->Show();
    }
}

namespace Online {

namespace {
constexpr uint16_t kDefaultRelayPort = 7778;

struct Settings {
    char name[32] = "Jugador";
    char relay[96] = "";
    int relayPort = kDefaultRelayPort;
    char code[16] = "";
    char ip[64] = "127.0.0.1";
    int port = Net::kDefaultPort;
    int maxPlayers = Net::kMaxPlayers;
    int mode = 0; // 0 = con servidor (codigo), 1 = directo (IP)
    bool roomVisible = true; // la sala sale en la lista de salas del servidor
    bool loaded = false;
};

void LoadSettings(Settings& s) {
    if (s.loaded) return;
    s.loaded = true;
    std::snprintf(s.name, sizeof(s.name), "%s", CVarGetString("gOnline.Name", "Jugador"));
    std::snprintf(s.relay, sizeof(s.relay), "%s", CVarGetString("gOnline.Relay", ""));
    s.relayPort = CVarGetInteger("gOnline.RelayPort", kDefaultRelayPort);
    std::snprintf(s.ip, sizeof(s.ip), "%s", CVarGetString("gOnline.Ip", "127.0.0.1"));
    s.port = CVarGetInteger("gOnline.Port", Net::kDefaultPort);
    s.mode = CVarGetInteger("gOnline.Mode", 0);
    s.roomVisible = CVarGetInteger("gOnline.RoomVisible", 1) != 0;
}

void SaveSettings(const Settings& s) {
    CVarSetString("gOnline.Name", s.name);
    CVarSetString("gOnline.Relay", s.relay);
    CVarSetInteger("gOnline.RelayPort", s.relayPort);
    CVarSetString("gOnline.Ip", s.ip);
    CVarSetInteger("gOnline.Port", s.port);
    CVarSetInteger("gOnline.Mode", s.mode);
    CVarSetInteger("gOnline.RoomVisible", s.roomVisible ? 1 : 0);
}

uint16_t ClampPort(int p) {
    if (p < 1) p = 1;
    if (p > 65535) p = 65535;
    return static_cast<uint16_t>(p);
}

// Texto segun el idioma elegido en el menu online (0 = espanol, 1 = ingles).
const char* T(const char* es, const char* en) {
    return Net::GetLanguage() == 1 ? en : es;
}
} // namespace

void OnlineWindow::DrawElement() {
    static Settings st;
    static std::string lastError;
    LoadSettings(st);

    Net::Role role = Net::GetRole();

    ImGui::TextUnformatted("Spaghetti Kart Online");
    ImGui::Separator();

    // Dentro de una sala, o en modo directo, no se busca.
    if ((role != Net::Role::None || st.mode != 0) && Net::GetRoomSearchState() != Net::RoomSearchState::Idle) {
        Net::RoomSearchStop();
    }

    if (role == Net::Role::None) {
        ImGui::InputText(T("Tu nombre", "Your name"), st.name, sizeof(st.name));

        ImGui::Spacing();
        ImGui::RadioButton(T("Con servidor (codigo de sala)", "With server (room code)"), &st.mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton(T("Directo (IP y puerto)", "Direct (IP and port)"), &st.mode, 1);
        ImGui::Spacing();

        if (st.mode == 0) {
            ImGui::InputText(T("Servidor", "Server"), st.relay, sizeof(st.relay));
            ImGui::InputInt(T("Puerto del servidor", "Server port"), &st.relayPort, 0, 0);
            if (st.relay[0] == '\0') {
                ImGui::TextDisabled(T("Escribe la direccion del servidor (IP o nombre).", "Enter the server address (IP or name)."));
            }
            ImGui::Separator();

            ImGui::SliderInt(T("Jugadores maximos", "Max players"), &st.maxPlayers, 2, Net::kMaxPlayers);
            Net::SetMaxPlayers(st.maxPlayers);
            ImGui::TextDisabled(T("Cuantos jugadores admite tu sala (de 2 a 16).", "How many players your room accepts (2 to 16)."));
            ImGui::Checkbox(T("Mostrar mi sala en la lista de salas", "Show my room in the room list"), &st.roomVisible);
            Net::SetRoomVisible(st.roomVisible);
            if (ImGui::Button(T("Crear sala", "Create room"))) {
                SaveSettings(st);
                lastError.clear();
                if (st.relay[0] == '\0') {
                    lastError = T("falta la direccion del servidor", "server address missing");
                } else {
                    Net::StartHostRelay(st.relay, ClampPort(st.relayPort), st.name, lastError);
                }
            }
            ImGui::Separator();

            ImGui::InputText(T("Codigo de sala", "Room code"), st.code, sizeof(st.code), ImGuiInputTextFlags_CharsUppercase);
            if (ImGui::Button(T("Unirse", "Join"))) {
                SaveSettings(st);
                lastError.clear();
                if (st.relay[0] == '\0') {
                    lastError = T("falta la direccion del servidor", "server address missing");
                } else {
                    Net::StartJoinRelay(st.relay, ClampPort(st.relayPort), st.code, st.name, lastError);
                }
            }
            ImGui::Separator();

            // Salas que hay ahora mismo en el servidor (se refresca sola mientras esta abierta).
            if (std::getenv("SK_ONLINE_IMGUI_ROOMS") != nullptr) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always); // pruebas: abre la lista de salas sola
            }
            if (ImGui::CollapsingHeader(T("Buscar salas", "Find rooms"))) {
                if (Net::GetRoomSearchState() == Net::RoomSearchState::Idle) {
                    Net::RoomSearchStart(st.relay, ClampPort(st.relayPort));
                }
                Net::RoomSearchState rs = Net::GetRoomSearchState();
                if (rs == Net::RoomSearchState::Searching) {
                    ImGui::TextDisabled(T("Buscando salas...", "Searching rooms..."));
                } else if (rs == Net::RoomSearchState::Failed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), T("No se pudo obtener la lista: %s", "Could not get the list: %s"),
                                       Net::GetRoomSearchError().c_str());
                } else if (Net::GetRooms().empty()) {
                    ImGui::TextDisabled(T("No hay salas disponibles. Crea una o escribe un codigo.", "No rooms available. Create one or enter a code."));
                }
                if (!Net::GetRooms().empty() &&
                    ImGui::BeginTable("##rooms", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn(T("Codigo", "Code"));
                    ImGui::TableSetupColumn(T("Anfitrion", "Host"));
                    ImGui::TableSetupColumn(T("Jugadores", "Players"));
                    ImGui::TableSetupColumn(T("Estado", "Status"));
                    ImGui::TableSetupColumn("##unirse");
                    ImGui::TableHeadersRow();
                    for (const Net::RoomInfo& r : Net::GetRooms()) {
                        const bool full = r.players >= r.maxPlayers;
                        const bool closed = r.racing || full;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(r.code.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(r.name.empty() ? T("(sin nombre)", "(no name)") : r.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%d / %d", r.players, r.maxPlayers);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(r.racing ? T("En carrera", "Racing") : (full ? T("Llena", "Full") : T("Esperando", "Waiting")));
                        ImGui::TableSetColumnIndex(4);
                        ImGui::BeginDisabled(closed);
                        const std::string label = std::string(T("Unirse", "Join")) + "##" + r.code;
                        if (ImGui::SmallButton(label.c_str())) {
                            SaveSettings(st);
                            lastError.clear();
                            std::snprintf(st.code, sizeof(st.code), "%s", r.code.c_str());
                            Net::StartJoinRelay(st.relay, ClampPort(st.relayPort), r.code, st.name, lastError);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndTable();
                }
            } else if (Net::GetRoomSearchState() != Net::RoomSearchState::Idle) {
                Net::RoomSearchStop();
            }
        } else {
            ImGui::InputInt(T("Puerto", "Port"), &st.port, 0, 0);
            ImGui::SliderInt(T("Jugadores maximos", "Max players"), &st.maxPlayers, 2, Net::kMaxPlayers);
            Net::SetMaxPlayers(st.maxPlayers);
            ImGui::TextDisabled(T("Hay que abrir el puerto UDP en tu router, o usar una VPN.", "You must open the UDP port on your router, or use a VPN."));
            if (ImGui::Button(T("Crear sala (Host)", "Create room (Host)"))) {
                SaveSettings(st);
                lastError.clear();
                Net::StartHost(ClampPort(st.port), st.name, lastError);
            }
            ImGui::Separator();
            ImGui::InputText(T("IP del anfitrion", "Host IP"), st.ip, sizeof(st.ip));
            if (ImGui::Button(T("Unirse (Join)", "Join"))) {
                SaveSettings(st);
                lastError.clear();
                Net::StartJoin(st.ip, ClampPort(st.port), st.name, lastError);
            }
            ImGui::TextDisabled(T("Para jugar en la misma PC usa 127.0.0.1", "To play on the same PC use 127.0.0.1"));
        }
        if (!lastError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Error: %s", lastError.c_str());
        }
    } else {
        if (Net::IsRelay() && !Net::GetRoomCode().empty()) {
            ImGui::Text(T("Codigo de sala:", "Room code:"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", Net::GetRoomCode().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Copiar", "Copy"))) {
                ImGui::SetClipboardText(Net::GetRoomCode().c_str());
            }
            if (role == Net::Role::Host) {
                ImGui::TextDisabled(T("Pasale este codigo a tus amigos.", "Give this code to your friends."));
            }
        }
        if (role == Net::Role::Host) {
            static int cup = 0, track = 1, cc = 1;
            const char* cupNames[] = { T("Copa Champinon", "Mushroom Cup"), T("Copa Flor", "Flower Cup"), T("Copa Estrella", "Star Cup"), T("Copa Especial", "Special Cup") };
            static const char* ccNames[] = { "50cc", "100cc", "150cc" };
            ImGui::Separator();
            ImGui::Combo(T("Copa", "Cup"), &cup, cupNames, 4);
            ImGui::SliderInt(T("Pista de la copa", "Track in the cup"), &track, 1, 4);
            ImGui::Combo(T("Cilindrada", "Engine class"), &cc, ccNames, 3);
            bool chaos = CVarGetInteger("gOnline.Chaos", 0) != 0;
            if (ImGui::Checkbox(T("Modo Chaos (sin items en la pista; cada 12 s todos reciben uno)", "Chaos mode (no item boxes; every 12 s everyone gets an item)"), &chaos)) {
                CVarSetInteger("gOnline.Chaos", chaos ? 1 : 0);
                CVarSave();
            }
            bool bots = CVarGetInteger("gOnline.Bots", 1) != 0;
            if (ImGui::Checkbox(T("Bots (CPUs hasta completar 16 karts; sin ellos solo corren los jugadores)", "Bots (CPUs fill up to 16 karts; without them only the players race)"), &bots)) {
                CVarSetInteger("gOnline.Bots", bots ? 1 : 0);
                CVarSave();
            }
            if (!Net::RaceActive()) {
                if (ImGui::Button(T("Iniciar carrera", "Start race"))) {
                    Net::RaceParams p;
                    p.cup = static_cast<uint8_t>(cup);
                    p.track = static_cast<uint8_t>(track - 1);
                    p.cc = static_cast<uint8_t>(cc);
                    p.chaos = chaos ? 1 : 0;
                    p.bots = bots ? 1 : 0;
                    p.delay = static_cast<uint8_t>(Net::SuggestDelay());
                    p.seed = static_cast<uint16_t>(static_cast<int>(ImGui::GetTime() * 1000.0)) | 1;
                    for (const Net::PeerInfo& pi : Net::GetPeers()) p.humans.push_back({ pi.slot, pi.slot % 8 });
                    Net::HostStartRace(p);
                }
            }
        }
        if (ImGui::Button(T("Desconectar", "Disconnect"))) {
            Net::Disconnect();
        }
    }

    ImGui::Spacing();
    ImGui::Text(T("Estado: %s", "Status: %s"), Net::GetStatus().c_str());

    std::vector<Net::PeerInfo> peers = Net::GetPeers();
    if (!peers.empty()) {
        ImGui::Spacing();
        ImGui::Text(T("Jugadores en la sala: %d / %d", "Players in the room: %d / %d"), static_cast<int>(peers.size()), Net::GetMaxPlayers());
        if (ImGui::BeginTable("##peers", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn(T("Nombre", "Name"));
            ImGui::TableSetupColumn("Ping (ms)");
            ImGui::TableHeadersRow();
            for (const Net::PeerInfo& p : peers) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d%s", p.slot, p.slot == 0 ? T(" (anfitrion)", " (host)") : "");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s%s", p.name.c_str(), p.isSelf ? T(" (tu)", " (you)") : "");
                ImGui::TableSetColumnIndex(2);
                if (p.isSelf && p.slot == 0) {
                    ImGui::TextUnformatted("-");
                } else {
                    ImGui::Text("%u", p.pingMs);
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Registro", "Log"))) {
        const std::vector<std::string>& log = Net::GetLog();
        ImGui::BeginChild("##netlog", ImVec2(0, 120), true);
        for (const std::string& line : log) {
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

} // namespace Online
