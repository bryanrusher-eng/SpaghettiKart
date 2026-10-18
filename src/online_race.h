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
// En carrera online la simulacion NO debe depender de la camara de cada PC (que objetos estan "cerca", "a la vista",
// en la misma seccion...): devuelve 1 cuando hay que simular todo el circuito siempre.
int online_no_cull(void);
// Pruebas (variables de entorno SK_ONLINE_TEST_ITEMS): dar un item justo antes de comprobar si se usa, y anotar cada uso.
void online_test_give_item(int slot, unsigned short buttonPressed);
void online_note_item_use(int slot, int item);
// Cuantos jugadores pueden usar items (hasta 16 en carrera en red; SK_ONLINE_OLD_ITEMS=1 reproduce el limite antiguo de 4).
int online_item_player_count(void);
// Lee una opcion del juego que afecta a la simulacion: en carrera en red devuelve siempre el valor por defecto.
int online_cvar_int(const char* name, int def);
float online_cvar_float(const char* name, float def);
// 1 si algun jugador humano esta cerca de esa posicion (sustituye a "alguna camara lo ve" en la simulacion online).
int online_actor_seen(const float pos[3]);
// El dibujado de cada fotograma usa su propio generador aleatorio para no alterar el de la simulacion.
// Diagnostico: anota quien pide cada numero aleatorio de la simulacion (SK_ONLINE_RNGTRACE).
void online_rng_trace(void);
int online_rng_tracing(void);
void online_rng_note(const char* text);
void online_render_rng_begin(void);
void online_render_rng_end(void);
// El slot (0..15) lo maneja un jugador humano (por red) en esta carrera.
int online_slot_is_human(int slot);
// Personaje elegido por el jugador de ese slot.
int online_slot_character(int slot);
// Slot de esta PC (el kart que maneja el mando del puerto 1).
int online_local_slot(void);
// Indice (0..15) del kart que sigue la camara y el marcador de esta PC: el slot propio en carrera online, 0 si no.
int online_view_slot(void);
// Botones del mando fisico de esta PC en este fotograma, SIN el retraso de red (para controles solo visuales, como la camara).
unsigned short online_local_buttons(void);

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
    int exitPrompt; // 0 = nada; 1 = el anfitrion va a terminar la carrera para todos; 2 = el jugador va a salir solo
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
