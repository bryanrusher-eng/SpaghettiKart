#ifndef ONLINE_MENU_H
#define ONLINE_MENU_H

// Menu Online dentro del juego (se abre con Z en GAME SELECT). Dibujado con las fuentes del juego y manejado
// con el mando; la parte de red esta en port/ui/OnlineMenuBridge.cpp.

#ifdef __cplusplus
extern "C" {
#endif

int online_menu_active(void);
void online_menu_open(void);
void online_menu_update(void);
void online_menu_render(void);

#ifdef __cplusplus
}
#endif

#endif // ONLINE_MENU_H
