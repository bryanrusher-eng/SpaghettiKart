#include "OnlineOverlay.h"
#include "port/net/Net.h"
#include "port/Engine.h"
#include "online_race.h"

#include <imgui.h>
#include <fast/interpreter.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Online {

namespace {

const char* kCharacterNames[8] = { "Mario", "Luigi", "Yoshi", "Toad", "DK", "Wario", "Peach", "Bowser" };

void Normalize(float v[3]) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.00001f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Proyecta un punto del mundo a pixeles de la ventana con la camara de la pantalla de esta PC.
bool Project(const OnlineViewData& v, const float world[3], float& sx, float& sy, float& depth) {
    auto vp = GetInterpreter()->mGameWindowViewport;
    float vw = static_cast<float>(OTRGetGameViewportWidth());
    float vh = static_cast<float>(OTRGetGameViewportHeight());
    if (vw <= 1.0f || vh <= 1.0f || !v.matValid) {
        return false;
    }
    // Mismas matrices con las que se dibujo el cuadro (vector fila: p * vista * proyeccion).
    float p4[4] = { world[0], world[1], world[2], 1.0f };
    float e[4], c[4];
    for (int j = 0; j < 4; j++) {
        e[j] = p4[0] * v.view[0][j] + p4[1] * v.view[1][j] + p4[2] * v.view[2][j] + p4[3] * v.view[3][j];
    }
    for (int j = 0; j < 4; j++) {
        c[j] = e[0] * v.proj[0][j] + e[1] * v.proj[1][j] + e[2] * v.proj[2][j] + e[3] * v.proj[3][j];
    }
    if (c[3] < 1.0f) {
        return false; // detras de la camara
    }
    float zc = c[3];
    float ndcX = c[0] / c[3];
    float ndcY = c[1] / c[3];
    if (std::fabs(ndcX) > 2.5f || std::fabs(ndcY) > 2.5f) {
        return false; // muy lejos del cuadro: no se dibuja
    }
    sx = static_cast<float>(vp.x) + (ndcX * 0.5f + 0.5f) * vw;
    sy = static_cast<float>(vp.y) + (0.5f - ndcY * 0.5f) * vh;
    depth = zc;
    return true;
}

std::string NameOf(const OnlineViewData& v, int slot, const std::vector<Net::PeerInfo>& peers) {
    for (const Net::PeerInfo& p : peers) {
        if (p.slot == slot) return p.name;
    }
    if (v.kart[slot].human) return "Jugador " + std::to_string(slot + 1);
    int c = v.kart[slot].character;
    return std::string("CPU ") + kCharacterNames[(c >= 0 && c < 8) ? c : 0];
}

void DrawShadowText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(font, size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), IM_COL32(0, 0, 0, 230), text);
    dl->AddText(font, size, pos, col, text);
}

} // namespace

void OverlayWindow::DrawElement() {
    }
    OnlineViewData v;
    int have = online_get_view_data(&v);
    std::vector<Net::PeerInfo> peers = Net::GetPeers();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float baseSize = ImGui::GetFontSize();
    // Con viewports multiples, ImGui dibuja en coordenadas de PANTALLA: hay que sumar la posicion de la ventana.
    const ImVec2 org = ImGui::GetMainViewport()->Pos;

    // --- leyenda: "X salio de la partida" (unos segundos) ---
    {
        std::vector<Net::LeftInfo> left = Net::GetLeftEvents();
        auto vpl = GetInterpreter()->mGameWindowViewport;
        float vwl = static_cast<float>(OTRGetGameViewportWidth());
        float vhl = static_cast<float>(OTRGetGameViewportHeight());
        float sizeL = baseSize * 1.3f;
        float yL = org.y + static_cast<float>(vpl.y) + vhl * 0.22f;
        for (const Net::LeftInfo& e : left) {
            if (e.ageMs > 7000) continue;
            std::string msg = e.name + " sali\xC3\xB3 de la partida";
            ImVec2 tsL = font->CalcTextSizeA(sizeL, 10000.0f, 0.0f, msg.c_str());
            float xL = org.x + static_cast<float>(vpl.x) + (vwl - tsL.x) * 0.5f;
            float alpha = e.ageMs > 6000 ? (7000 - e.ageMs) / 1000.0f : 1.0f;
            dl->AddRectFilled(ImVec2(xL - 10, yL - 4), ImVec2(xL + tsL.x + 10, yL + tsL.y + 4),
                              IM_COL32(0, 0, 0, static_cast<int>(170 * alpha)), 6.0f);
            dl->AddText(font, sizeL, ImVec2(xL, yL), IM_COL32(255, 120, 100, static_cast<int>(255 * alpha)), msg.c_str());
            yL += tsL.y + 10;
        }
    }
    if (!have) {
        return; // sin carrera en red no hay nombres ni tabla
    }
    // --- nombre sobre cada kart humano (menos el propio) ---
    for (int slot = 0; slot < v.numKarts; slot++) {
        if (slot == v.localSlot || !v.kart[slot].human) continue;
        // Punto central del kart; el nombre se coloca unos pixeles por encima (mas arriba cuanto mas cerca).
        float world[3] = { v.kart[slot].pos[0], v.kart[slot].pos[1] + 3.0f, v.kart[slot].pos[2] };
        float sx, sy, depth;
        bool projOk = Project(v, world, sx, sy, depth);
        if (!projOk) continue;
        auto vpn = GetInterpreter()->mGameWindowViewport;
        float vwn = static_cast<float>(OTRGetGameViewportWidth());
        float vhn = static_cast<float>(OTRGetGameViewportHeight());
        // Fuera del cuadro (p. ej. el otro kart a un lado en la parrilla): no se dibuja el nombre.
        if (sx < static_cast<float>(vpn.x) - 30.0f || sx > static_cast<float>(vpn.x) + vwn + 30.0f) continue;
        float size = baseSize * (1.45f - std::min(depth, 2500.0f) / 2500.0f * 0.6f);
        std::string name = NameOf(v, slot, peers);
        ImVec2 ts = font->CalcTextSizeA(size, 10000.0f, 0.0f, name.c_str());
        float lift = 8.0f * vhn / 480.0f; // el punto proyectado ya es la parte alta del kart
        float tx = sx - ts.x * 0.5f;
        float ty = sy - lift - ts.y;
        // Si se sale del cuadro visible, se pega al borde en vez de desaparecer.
        tx = std::min(std::max(tx, static_cast<float>(vpn.x) + 6.0f), static_cast<float>(vpn.x) + vwn - ts.x - 6.0f);
        ty = std::min(std::max(ty, static_cast<float>(vpn.y) + 6.0f), static_cast<float>(vpn.y) + vhn - ts.y - 6.0f);
        DrawShadowText(dl, font, size, ImVec2(org.x + tx, org.y + ty), IM_COL32(255, 235, 120, 255), name.c_str());
    }

    // --- tabla de posiciones mientras se mantiene L ---
    if (v.lHeld) {
        auto vp = GetInterpreter()->mGameWindowViewport;
        float vw = static_cast<float>(OTRGetGameViewportWidth());
        float vh = static_cast<float>(OTRGetGameViewportHeight());
        float rowH = std::min(baseSize * 1.25f, vh / 21.0f);
        float size = rowH * 0.86f;
        float panelW = std::max(vw * 0.36f, 220.0f);
        float panelH = rowH * (v.numKarts + 1.6f);
        ImVec2 p0(org.x + static_cast<float>(vp.x) + vw * 0.5f - panelW * 0.5f,
                  org.y + static_cast<float>(vp.y) + vh * 0.5f - panelH * 0.5f);
        dl->AddRectFilled(p0, ImVec2(p0.x + panelW, p0.y + panelH), IM_COL32(8, 8, 24, 205), 8.0f);
        dl->AddRect(p0, ImVec2(p0.x + panelW, p0.y + panelH), IM_COL32(120, 160, 255, 255), 8.0f, 0, 2.0f);
        DrawShadowText(dl, font, size, ImVec2(p0.x + 10.0f, p0.y + rowH * 0.2f), IM_COL32(255, 255, 255, 255),
                       "POSICIONES");
        for (int rank = 0; rank < v.numKarts; rank++) {
            int slot = v.rankPlayer[rank];
            if (slot < 0 || slot >= v.numKarts) continue;
            float y = p0.y + rowH * (rank + 1.4f);
            bool me = (slot == v.localSlot);
            ImU32 col = me ? IM_COL32(120, 255, 140, 255)
                           : (v.kart[slot].human ? IM_COL32(255, 235, 120, 255) : IM_COL32(210, 210, 220, 255));
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", rank + 1);
            DrawShadowText(dl, font, size, ImVec2(p0.x + 10.0f, y), col, buf);
            DrawShadowText(dl, font, size, ImVec2(p0.x + 10.0f + size * 2.2f, y), col,
                           NameOf(v, slot, peers).c_str());
            std::snprintf(buf, sizeof(buf), "V %d/3", v.kart[slot].lap);
            ImVec2 ts = font->CalcTextSizeA(size, 10000.0f, 0.0f, buf);
            DrawShadowText(dl, font, size, ImVec2(p0.x + panelW - ts.x - 10.0f, y), col, buf);
        }
    }
}

} // namespace Online
