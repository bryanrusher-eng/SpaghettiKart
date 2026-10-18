#pragma once

#include <libultraship/libultraship.h>

namespace Online {
// Capa transparente sobre el juego durante una carrera en red:
//  - el nombre de cada jugador humano sobre su kart,
//  - la tabla de posiciones mientras se mantiene el boton L.
class OverlayWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    ~OverlayWindow() = default;

  protected:
    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override {};
};
} // namespace Online
