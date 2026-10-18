#ifndef ONLINE_RACE_H
#define ONLINE_RACE_H

#include <libultraship.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 mientras dura una carrera en red ya cargada.
extern s32 gOnlineRace;
// 1 desde que llega el inicio de carrera hasta que termina de cargarse la pista.
extern s32 gOnlineRaceSetup;

// Hay una carrera en red en preparacion o en marcha.
int online_race_setup_active(void);
// El slot (0..15) lo maneja un jugador humano (por red) en esta carrera.
int online_slot_is_human(int slot);
// Personaje elegido por el jugador de ese slot.
int online_slot_character(int slot);
// Slot de esta PC (el kart que maneja el mando del puerto 1).
int online_local_slot(void);
// Indice (0..15) del kart que sigue la camara y el marcador de esta PC: el slot propio en carrera online, 0 si no.
int online_view_slot(void);

// Se llama cada fotograma, aunque no haya carrera: recoge el aviso de inicio del anfitrion.
void online_poll_start(void);
// Fin de la carrera en red: la llama la logica de carrera en cada tick cuando acabo la cuenta atras de resultados.
void online_race_finished_tick(void);
int online_results_active(void);
int online_results_seconds_left(void);
// Dentro de setup_race(): fija la semilla aleatoria y los valores previos a cargar la pista.
void online_setup_begin(void);
// Al final de setup_race(): arranca el intercambio de mandos.
void online_setup_end(void);
// En read_controllers(): intercambia mandos con los demas y rellena los pads de todos los slots.
void online_apply_pads(void);
// Cierra la carrera en red si el juego salio de la carrera.
void online_check_end(void);

// Datos para dibujar encima del juego (nombres sobre los karts y tabla de posiciones).
typedef struct OnlineKartView {
    int human;     // 1 si lo maneja un jugador humano
    int character; // 0..7 (Mario, Luigi, Yoshi, Toad, DK, Wario, Peach, Bowser)
    int rank;      // 0 = primero
    int lap;       // vuelta actual (1..3)
    float pos[3];  // posicion en el mundo
} OnlineKartView;

typedef struct OnlineViewData {
    int localSlot;
    int lHeld;     // 1 mientras se mantiene el boton L
    float camPos[3];
    float camLook[3];
    float camUp[3];
    float fov;     // vertical, en grados
    float aspect;  // gScreenAspect (relacion que usa el juego al proyectar)
    int matValid;  // 1 si proj/view son las matrices exactas con que se dibujo el cuadro
    float proj[4][4];
    float view[4][4];
    int numKarts;
    int rankPlayer[16]; // rankPlayer[0] = slot que va primero
    OnlineKartView kart[16];
} OnlineViewData;

// Devuelve 1 y rellena 'out' si hay una carrera en red en marcha; 0 si no.
int online_get_view_data(OnlineViewData* out);

// Guarda las matrices de la camara de la pantalla 0 tal como se usaron al dibujar (para colocar los nombres).
void online_store_view(const float proj[4][4], const float view[4][4]);

#ifdef __cplusplus
}
#endif

#endif // ONLINE_RACE_H
