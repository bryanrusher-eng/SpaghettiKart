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

// Flujo con los menus nativos del juego (SELECT PLAYER y eleccion de pista).
int online_menu_flow_active(void);
// Despues de una carrera en red: al volver al GAME SELECT se reabre la sala.
void online_menu_return_to_lobby(void);
// Despues de los resultados: el anfitrion pasa a elegir la siguiente pista (selector nativo); los demas, a la sala.
void online_menu_after_race(void);
// Resultados de la carrera en red (nombres de los jugadores en vez de los personajes).
void online_results_render(void);
void online_menu_flow_tick(void);
void online_menu_player_done(void);
void online_menu_course_done(void);

#ifdef __cplusplus
}
#endif

#endif // ONLINE_MENU_H
