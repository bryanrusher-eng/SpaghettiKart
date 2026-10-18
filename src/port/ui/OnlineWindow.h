#pragma once

#include <libultraship/libultraship.h>

namespace Online {
// Ventana ImGui para crear una sala (Host) o unirse a una (Join).
class OnlineWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    ~OnlineWindow() = default;

  protected:
    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override {};
};
} // namespace Online
