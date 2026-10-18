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
        ImGui::InputText("Tu nombre", st.name, sizeof(st.name));

        ImGui::Spacing();
        ImGui::RadioButton("Con servidor (codigo de sala)", &st.mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Directo (IP y puerto)", &st.mode, 1);
        ImGui::Spacing();

        if (st.mode == 0) {
            ImGui::InputText("Servidor", st.relay, sizeof(st.relay));
            ImGui::InputInt("Puerto del servidor", &st.relayPort, 0, 0);
            if (st.relay[0] == '\0') {
                ImGui::TextDisabled("Escribe la direccion del servidor (IP o nombre).");
            }
            ImGui::Separator();

            ImGui::SliderInt("Jugadores maximos", &st.maxPlayers, 2, Net::kMaxPlayers);
            Net::SetMaxPlayers(st.maxPlayers);
            ImGui::TextDisabled("Cuantos jugadores admite tu sala (de 2 a 16).");
            ImGui::Checkbox("Mostrar mi sala en la lista de salas", &st.roomVisible);
            Net::SetRoomVisible(st.roomVisible);
            if (ImGui::Button("Crear sala")) {
                SaveSettings(st);
                lastError.clear();
                if (st.relay[0] == '\0') {
                    lastError = "falta la direccion del servidor";
                } else {
                    Net::StartHostRelay(st.relay, ClampPort(st.relayPort), st.name, lastError);
                }
            }
            ImGui::Separator();

            ImGui::InputText("Codigo de sala", st.code, sizeof(st.code), ImGuiInputTextFlags_CharsUppercase);
            if (ImGui::Button("Unirse")) {
                SaveSettings(st);
                lastError.clear();
                if (st.relay[0] == '\0') {
                    lastError = "falta la direccion del servidor";
                } else {
                    Net::StartJoinRelay(st.relay, ClampPort(st.relayPort), st.code, st.name, lastError);
                }
            }
            ImGui::Separator();

            // Salas que hay ahora mismo en el servidor (se refresca sola mientras esta abierta).
            if (std::getenv("SK_ONLINE_IMGUI_ROOMS") != nullptr) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always); // pruebas: abre la lista de salas sola
            }
            if (ImGui::CollapsingHeader("Buscar salas")) {
                if (Net::GetRoomSearchState() == Net::RoomSearchState::Idle) {
                    Net::RoomSearchStart(st.relay, ClampPort(st.relayPort));
                }
                Net::RoomSearchState rs = Net::GetRoomSearchState();
                if (rs == Net::RoomSearchState::Searching) {
                    ImGui::TextDisabled("Buscando salas...");
                } else if (rs == Net::RoomSearchState::Failed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "No se pudo obtener la lista: %s",
                                       Net::GetRoomSearchError().c_str());
                } else if (Net::GetRooms().empty()) {
                    ImGui::TextDisabled("No hay salas disponibles. Crea una o escribe un codigo.");
                }
                if (!Net::GetRooms().empty() &&
                    ImGui::BeginTable("##rooms", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Codigo");
                    ImGui::TableSetupColumn("Anfitrion");
                    ImGui::TableSetupColumn("Jugadores");
                    ImGui::TableSetupColumn("Estado");
                    ImGui::TableSetupColumn("##unirse");
                    ImGui::TableHeadersRow();
                    for (const Net::RoomInfo& r : Net::GetRooms()) {
                        const bool full = r.players >= r.maxPlayers;
                        const bool closed = r.racing || full;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(r.code.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(r.name.empty() ? "(sin nombre)" : r.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%d / %d", r.players, r.maxPlayers);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(r.racing ? "En carrera" : (full ? "Llena" : "Esperando"));
                        ImGui::TableSetColumnIndex(4);
                        ImGui::BeginDisabled(closed);
                        const std::string label = "Unirse##" + r.code;
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
            ImGui::InputInt("Puerto", &st.port, 0, 0);
            ImGui::SliderInt("Jugadores maximos", &st.maxPlayers, 2, Net::kMaxPlayers);
            Net::SetMaxPlayers(st.maxPlayers);
            ImGui::TextDisabled("Hay que abrir el puerto UDP en tu router, o usar una VPN.");
            if (ImGui::Button("Crear sala (Host)")) {
                SaveSettings(st);
                lastError.clear();
                Net::StartHost(ClampPort(st.port), st.name, lastError);
            }
            ImGui::Separator();
            ImGui::InputText("IP del anfitrion", st.ip, sizeof(st.ip));
            if (ImGui::Button("Unirse (Join)")) {
                SaveSettings(st);
                lastError.clear();
                Net::StartJoin(st.ip, ClampPort(st.port), st.name, lastError);
            }
            ImGui::TextDisabled("Para jugar en la misma PC usa 127.0.0.1");
        }
        if (!lastError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Error: %s", lastError.c_str());
        }
    } else {
        if (Net::IsRelay() && !Net::GetRoomCode().empty()) {
            ImGui::Text("Codigo de sala:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", Net::GetRoomCode().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copiar")) {
                ImGui::SetClipboardText(Net::GetRoomCode().c_str());
            }
            if (role == Net::Role::Host) {
                ImGui::TextDisabled("Pasale este codigo a tus amigos.");
            }
        }
        if (role == Net::Role::Host) {
            static int cup = 0, track = 1, cc = 1;
            static const char* cupNames[] = { "Copa Champinon", "Copa Flor", "Copa Estrella", "Copa Especial" };
            static const char* ccNames[] = { "50cc", "100cc", "150cc" };
            ImGui::Separator();
            ImGui::Combo("Copa", &cup, cupNames, 4);
            ImGui::SliderInt("Pista de la copa", &track, 1, 4);
            ImGui::Combo("Cilindrada", &cc, ccNames, 3);
            if (!Net::RaceActive()) {
                if (ImGui::Button("Iniciar carrera")) {
                    Net::RaceParams p;
                    p.cup = static_cast<uint8_t>(cup);
                    p.track = static_cast<uint8_t>(track - 1);
                    p.cc = static_cast<uint8_t>(cc);
                    p.delay = static_cast<uint8_t>(Net::SuggestDelay());
                    p.seed = static_cast<uint16_t>(static_cast<int>(ImGui::GetTime() * 1000.0)) | 1;
                    for (const Net::PeerInfo& pi : Net::GetPeers()) p.humans.push_back({ pi.slot, pi.slot % 8 });
                    Net::HostStartRace(p);
                }
            }
        }
        if (ImGui::Button("Desconectar")) {
            Net::Disconnect();
        }
    }

    ImGui::Spacing();
    ImGui::Text("Estado: %s", Net::GetStatus().c_str());

    std::vector<Net::PeerInfo> peers = Net::GetPeers();
    if (!peers.empty()) {
        ImGui::Spacing();
        ImGui::Text("Jugadores en la sala: %d / %d", static_cast<int>(peers.size()), Net::GetMaxPlayers());
        if (ImGui::BeginTable("##peers", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Nombre");
            ImGui::TableSetupColumn("Ping (ms)");
            ImGui::TableHeadersRow();
            for (const Net::PeerInfo& p : peers) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d%s", p.slot, p.slot == 0 ? " (anfitrion)" : "");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s%s", p.name.c_str(), p.isSelf ? " (tu)" : "");
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
    if (ImGui::CollapsingHeader("Registro")) {
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
