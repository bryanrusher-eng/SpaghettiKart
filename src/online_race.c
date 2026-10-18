#include <libultraship.h>
#include <macros.h>
#include <defines.h>
#include <common_structs.h>
#include <mk64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "online_race.h"
#include "camera.h"
#include "main.h"
#include "menus.h"
#include "code_80005FD0.h"
#include "port/Game.h"
#include "port/net/NetBridge.h"
#include "online_menu.h"
#include "engine/TrackBrowser.h"
#include "racing/race_logic.h"
#include "racing/collision.h"

extern OSContPad gControllerPads[];
extern u16 gRandomSeed16;
void func_8000F124(void);

s32 gOnlineRace = 0;
s32 gOnlineRaceSetup = 0;

static NetBridgeRace sParams;
static int sLHeld = 0; // el boton L del mando local esta pulsado
static int sExitPrompt = 0;                 // aviso "salir de la partida" abierto (ver online_apply_pads)
static unsigned short sPrevLocal = 0xFFFF;  // botones locales del tick anterior (para detectar pulsaciones)
static unsigned short sLocalButtons = 0; // botones del mando fisico (puerto 1) antes de mezclar los mandos de red

unsigned short online_local_buttons(void) {
    return sLocalButtons;
}
static u32 sHumanMask = 0;
static u32 sTick = 0;
static FILE* sLog = NULL;
static int sDemoCancelled = 0;
static int sResultsTimer = -1; // ticks desde que se muestran los resultados (-1 = aun no)

#define RESULTS_TICKS 450 // 15 s

int online_results_active(void) {
    return gOnlineRace && sResultsTimer >= 0;
}

int online_results_seconds_left(void) {
    int left = (RESULTS_TICKS - sResultsTimer + 29) / 30;
    return left < 0 ? 0 : left;
}

// Todas las PCs llegan a este tick a la vez (la simulacion es la misma): se muestran los resultados 15 s y se
// vuelve juntos a la sala, sin pasar por las pantallas de Grand Prix del juego.
void online_race_finished_tick(void) {
    if (sResultsTimer < 0) {
        int winner = gGPCurrentRacePlayerIdByRank[0];

        sResultsTimer = 0;
        // La victoria se anota una sola vez (el anfitrion la reparte a todos con la lista de jugadores).
        if (online_slot_is_human(winner) && !((NetBridge_DroppedMask() >> winner) & 1)) {
            NetBridge_RaceWinner(winner);
        } else {
            NetBridge_RaceCpuWinner(); // gano un CPU (o alguien que ya se fue): tambien cuenta
        }
    }
    sResultsTimer++;
    if (sResultsTimer >= RESULTS_TICKS) {
        sResultsTimer = -1;
        gOnlineRace = 0;
        NetBridge_RaceEndGraceful();
        if (sLog != NULL) {
            fprintf(sLog, "FIN de carrera en tick %u\n", sTick);
            fclose(sLog);
            sLog = NULL;
        }
        online_menu_after_race(); // el anfitrion va a elegir la siguiente pista; los demas a la sala
    }
}

int online_race_setup_active(void) {
    return gOnlineRaceSetup || gOnlineRace;
}

// Generador aleatorio del DIBUJADO, separado del de la simulacion (ver race_logic_loop).
static u16 sSimRng = 0;
static u16 sRenderRng = 0x5A5A;
static int sRngSwapped = 0;

void online_render_rng_begin(void) {
    if (!gOnlineRace || !online_no_cull()) {
        return;
    }
    sSimRng = gRandomSeed16;
    gRandomSeed16 = sRenderRng;
    sRngSwapped = 1;
}

void online_rng_trace(void) {
    static int en = -1;

    if (en < 0) {
        en = getenv("SK_ONLINE_RNGTRACE") != NULL ? 1 : 0;
    }
    if (en && gOnlineRace && !sRngSwapped) {
        NetBridge_RngTrace(sTick, NULL);
    }
}

int online_rng_tracing(void) {
    return getenv("SK_ONLINE_RNGTRACE") != NULL && gOnlineRace && !sRngSwapped;
}

void online_rng_note(const char* text) {
    if (online_rng_tracing()) {
        NetBridge_RngTrace(sTick, text);
    }
}

void online_render_rng_end(void) {
    if (!sRngSwapped) {
        return;
    }
    sRenderRng = gRandomSeed16;
    gRandomSeed16 = sSimRng;
    sRngSwapped = 0;
}

// Un item "expirado" (plátano, caparazon, caja falsa) es el que ninguna camara ve (flags & 0xF). En carrera online eso
// depende de la camara de cada PC y separa la simulacion (un caparazon triple se destruye en una PC y en otra no): se
// usa la cercania a cualquier jugador humano, igual en todas las PCs.
int online_actor_seen(const float pos[3]) {
    int i;

    for (i = 0; i < NUM_PLAYERS; i++) {
        if (online_slot_is_human(i)) {
            float dx = gPlayers[i].pos[0] - pos[0];
            float dz = gPlayers[i].pos[2] - pos[2];

            if ((dx * dx) + (dz * dz) < 2250000.0f) { // 1500 unidades
                return 1;
            }
        }
    }
    return 0;
}

// Opciones del menu del juego que cambian la SIMULACION (CC propio, CPU dificil, numero de trenes y vehiculos, caparazones
// rectos, salto lunar...). Si dos PCs las tienen distintas las partidas se separan: en carrera en red todas valen lo
// mismo (su valor por defecto), tengan lo que tengan configurado.
int online_cvar_int(const char* name, int def) {
    return online_race_setup_active() ? def : CVarGetInteger(name, def);
}

float online_cvar_float(const char* name, float def) {
    return online_race_setup_active() ? def : CVarGetFloat(name, def);
}

int online_no_cull(void) {
    // Pruebas: SK_ONLINE_CULL=1 devuelve el comportamiento original (depende de la camara) para comparar.
    static int off = -1;

    if (off < 0) {
        off = getenv("SK_ONLINE_CULL") != NULL ? 1 : 0;
    }
    return online_race_setup_active() && !off;
}

int online_slot_is_human(int slot) {
    if (slot < 0 || slot >= 16) {
        return 0;
    }
    return (sHumanMask >> slot) & 1;
}

int online_slot_character(int slot) {
    int i;
    for (i = 0; i < sParams.numHumans; i++) {
        if (sParams.slot[i] == slot) {
            return sParams.character[i];
        }
    }
    return 0;
}

int online_local_slot(void) {
    return NetBridge_LocalSlot();
}

int online_view_slot(void) {
    if (gOnlineRaceSetup || gOnlineRace) {
        int slot = NetBridge_LocalSlot();
        if (slot >= 0 && slot < NUM_PLAYERS) {
            return slot;
        }
    }
    return 0;
}

void online_poll_start(void) {
    int i;

    if (!gDemoMode) {
        sDemoCancelled = 0;
    }
    if (gOnlineRaceSetup || gOnlineRace) {
        return;
    }
    // Si esta PC todavia esta saliendo de la carrera anterior (animacion de vuelta al menu), el aviso de inicio se
    // queda pendiente y se atiende cuando termine: cargar la carrera a mitad de esa animacion la deja atascada.
    {
        extern u16 gIsInQuitToMenuTransition;
        if (gIsInQuitToMenuTransition != 0 || gGamestate == 255) {
            return;
        }
    }
    // Si la pantalla de titulo esta en plena demostracion automatica, se cancela: si no, la carrera online tendria que
    // esperar a que termine la demo (casi un minuto).
    if (gDemoMode && gGamestate == RACING && NetBridge_RaceStartPending()) {
        if (!sDemoCancelled) {
            sDemoCancelled = 1;
            func_8028FBD4();
        }
        return;
    }
    if (!NetBridge_PollRaceStart(&sParams)) {
        return;
    }

    sHumanMask = 0;
    for (i = 0; i < sParams.numHumans; i++) {
        sHumanMask |= 1u << sParams.slot[i];
    }

    // Mismos valores que fija el menu al elegir Mario GP en la primera opcion.
    gModeSelection = GRAND_PRIX;
    gScreenModeSelection = SCREEN_MODE_1P;
    gPlayerCount = 1;
    gPlayerCountSelection1 = 1;
    gCCSelection = sParams.cc;
    // Se carga la pista como lo hace el selector del juego (si no, solo la primera pista de la primera copa se ve bien).
    {
        void* cups[4];
        int cup = sParams.cup > 3 ? 0 : sParams.cup;
        int track = sParams.track > 3 ? 0 : sParams.track;

        cups[0] = GetMushroomCup();
        cups[1] = GetFlowerCup();
        cups[2] = GetStarCup();
        cups[3] = GetSpecialCup();
        CM_SetCup(cups[cup]);
        // CM_SetCup no actualiza el indice de copa: sin esto los invitados siempre ven el nombre de la primera copa.
        CM_SetCupIndex((size_t) cup);
        SetCupCursorPosition(track);
        gCupSelection = cup;
        gCourseIndexInCup = track;
        gCurrentCourseId = gCupCourseOrder[cup][track];
        TrackBrowser_SetTrackFromCup();
        D_800DC540 = GetCupIndex();
    }
    gCharacterSelections[0] = online_slot_character(0);
    gDemoMode = 0;
    // El modo debug del juego (Z arranca la carrera, saltar vueltas, salir con R+L+A+B...) cambia la simulacion: si una
    // PC lo tiene activo y otra no, se desincronizan. En carrera en red siempre va apagado.
    gEnableDebugMode = 0;
    gMenuSelection = COURSE_SELECT_MENU; // distinto de START_MENU: asi setup_race() elige la pista de la copa

    sResultsTimer = -1;
    gOnlineRaceSetup = 1;
    gGamestateNext = RACING;
}

void online_setup_begin(void) {
    if (!gOnlineRaceSetup) {
        return;
    }
    // Todas las PCs parten de la misma semilla justo antes de cargar la pista.
    gRandomSeed16 = sParams.seed;
    func_8000F124();
}

void online_setup_end(void) {
    const char* logName;

    if (!gOnlineRaceSetup) {
        return;
    }
    gOnlineRaceSetup = 0;
    NetBridge_RaceBegin();
    gOnlineRace = 1;
    sTick = 0;
    sRngSwapped = 0;
    // Los ticks de simulacion por fotograma tienen que ser iguales en todas las PCs (el ajuste "Game Speed" de las
    // herramientas de desarrollo los cambia y separaria las partidas).
    gTickLogic = 2;
    sExitPrompt = 0;
    sPrevLocal = 0xFFFF;

    // La pantalla de esta PC sigue a su propio kart.
    gScreenContexts[0].player = &gPlayers[online_view_slot()];

    logName = getenv("SK_ONLINE_LOG");
    if (logName != NULL && sLog == NULL) {
        sLog = fopen(logName, "w");
    }
    if (sLog != NULL && getenv("SK_ONLINE_SURFDBG") != NULL) {
        // Diagnostico: altura del suelo a lo largo de la recta de salida (donde se colocan los karts).
        int zi;
        int z0 = (int) gTrackPaths[0][0].z;
        float y0 = (float) gTrackPaths[0][0].y;

        fprintf(sLog, "# path0 x=%d y=%d z=%d minY=%.1f\n", (int) gTrackPaths[0][0].x, (int) y0, z0, (double) gTrackMinY);
        for (zi = -400; zi <= 1000; zi += 25) {
            fprintf(sLog, "# surf z=%d : x-20=%.1f x0=%.1f x20=%.1f\n", z0 + zi,
                    (double) spawn_actor_on_surface(-20.0f, y0 + 50.0f, (float) (z0 + zi)),
                    (double) spawn_actor_on_surface(0.0f, y0 + 50.0f, (float) (z0 + zi)),
                    (double) spawn_actor_on_surface(20.0f, y0 + 50.0f, (float) (z0 + zi)));
        }
        fflush(sLog);
    }
}

static u64 hash_mix(u64 h, u32 v) {
    return (h ^ v) * 1099511628211ull;
}

static u32 float_bits(f32 f) {
    u32 v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

// Registro de la "huella" del estado cada segundo: si dos PCs no coinciden, hay desincronizacion.
static void online_log_state(void) {
    u64 h = 1469598103934665603ull;
    int i;

    if (sLog == NULL) {
        return;
    }
    for (i = 0; i < NUM_PLAYERS; i++) {
        Player* p = &gPlayers[i];
        h = hash_mix(h, float_bits(p->pos[0]));
        h = hash_mix(h, float_bits(p->pos[1]));
        h = hash_mix(h, float_bits(p->pos[2]));
        h = hash_mix(h, float_bits(p->speed));
        h = hash_mix(h, (u32) (u16) p->rotation[1]);
        h = hash_mix(h, (u32) gGPCurrentRaceRankByPlayerId[i]);
    }
    {
        // Mas huellas para localizar de donde viene una separacion: objetos del circuito, actores (items, plátanos...) y
        // el generador de numeros aleatorios. Tienen que ser identicas en todas las PCs, este donde este cada camara.
        u64 ho = 1469598103934665603ull;
        u64 ha = 1469598103934665603ull;

        char blocks[80];
        u64 hb = 1469598103934665603ull;
        int bpos = 0;

        blocks[0] = 0;
        for (i = 0; i < OBJECT_LIST_SIZE; i++) {
            u64 before = ho;

            ho = hash_mix(ho, float_bits(gObjectList[i].pos[0]));
            ho = hash_mix(ho, float_bits(gObjectList[i].pos[1]));
            ho = hash_mix(ho, float_bits(gObjectList[i].pos[2]));
            ho = hash_mix(ho, (u32) gObjectList[i].state);
            // huella por bloques de 64 objetos: sirve para localizar que objetos se separan
            hb = hash_mix(hb, (u32) (ho ^ before));
            if ((i % 64) == 63 || i == OBJECT_LIST_SIZE - 1) {
                bpos += snprintf(blocks + bpos, sizeof(blocks) - bpos, "%04x", (unsigned) (hb & 0xFFFF));
                hb = 1469598103934665603ull;
            }
        }
        for (i = 0; i < (int) CM_GetActorSize(); i++) {
            struct Actor* a = CM_GetActor((size_t) i);

            if (a == NULL) {
                continue;
            }
            ha = hash_mix(ha, float_bits(a->pos[0]));
            ha = hash_mix(ha, float_bits(a->pos[1]));
            ha = hash_mix(ha, float_bits(a->pos[2]));
            ha = hash_mix(ha, (u32) (u16) a->flags);
        }
        {
            // Diagnostico (SK_ONLINE_ACTDUMP=ruta): lista de todos los actores en cada registro, para ver cual se separa.
            static FILE* sActLog = NULL;
            static int sActLogTried = 0;

            if (!sActLogTried) {
                const char* path = getenv("SK_ONLINE_ACTDUMP");

                sActLogTried = 1;
                sActLog = path != NULL ? fopen(path, "w") : NULL;
            }
            if (sActLog != NULL) {
                for (i = 0; i < (int) CM_GetActorSize(); i++) {
                    struct Actor* a = CM_GetActor((size_t) i);

                    if (a != NULL) {
                        fprintf(sActLog, "t%u a%d type=%d fl=%x st=%d pos=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f\n", sTick, i,
                                (int) a->type, (unsigned) (u16) a->flags, (int) a->state, a->pos[0], a->pos[1],
                                a->pos[2], a->velocity[0], a->velocity[1], a->velocity[2]);
                    }
                }
                fflush(sActLog);
            }
        }
        fprintf(sLog, "tick %u hash %016llx p0=(%.2f,%.2f) p1=(%.2f,%.2f) rng=%04x obj=%016llx act=%016llx blk=%s\n",
                sTick, (unsigned long long) h, gPlayers[0].pos[0], gPlayers[0].pos[2], gPlayers[1].pos[0],
                gPlayers[1].pos[2], (unsigned) gRandomSeed16, (unsigned long long) ho, (unsigned long long) ha, blocks);
    }
    fflush(sLog);
}

// Depuracion de desincronizaciones: SK_ONLINE_LOG_FULL=desde-hasta registra todos los jugadores en cada tick.
static void online_log_full(void) {
    static int from = -2, to = -2;
    int i;

    if (sLog == NULL) {
        return;
    }
    if (from == -2) {
        const char* e = getenv("SK_ONLINE_LOG_FULL");
        from = -1;
        to = -1;
        if (e != NULL) {
            sscanf(e, "%d-%d", &from, &to);
        }
    }
    if (from < 0 || (int) sTick < from || (int) sTick > to) {
        return;
    }
    fprintf(sLog, "T%u", sTick);
    // mandos aplicados a los slots 0 y 1 en este tick (botones, palanca X, palanca Y)
    fprintf(sLog, " P0:%04x,%d,%d P1:%04x,%d,%d", (unsigned) gControllerPads[0].button, (int) gControllerPads[0].stick_x,
            (int) gControllerPads[0].stick_y, (unsigned) gControllerPads[1].button, (int) gControllerPads[1].stick_x,
            (int) gControllerPads[1].stick_y);
    for (i = 0; i < NUM_PLAYERS; i++) {
        fprintf(sLog, " %d:%.2f,%.2f,%.3f,%d", i, gPlayers[i].pos[0], gPlayers[i].pos[2], gPlayers[i].speed,
                (int) gGPCurrentRaceRankByPlayerId[i]);
    }
    fprintf(sLog, "\n");
}

void online_apply_pads(void) {
    u16 buttons[16];
    s8 stickX[16];
    s8 stickY[16];
    int delay = NetBridge_RaceDelay();
    int spins = 0;
    int i;
    unsigned long long waitStartUs;

    {
        // El mando de esta PC puede estar en cualquier puerto (1..4): se juntan los botones de todos y se toma el
        // stick del que lo tenga mas inclinado, asi no importa en que puerto este configurado.
        u16 localButtons = 0;
        s8 localX = 0;
        s8 localY = 0;
        int best = -1;
        int p;

        for (p = 0; p < 4; p++) {
            int mag = abs((int) gControllerPads[p].stick_x) + abs((int) gControllerPads[p].stick_y);

            localButtons |= gControllerPads[p].button;
            if (mag > best) {
                best = mag;
                localX = gControllerPads[p].stick_x;
                localY = gControllerPads[p].stick_y;
            }
        }
        sLHeld = (localButtons & 0x0020) != 0; // CONT_L
        sLocalButtons = localButtons;

        // START abre el aviso de salida: A confirma; B o START cancelan. Mientras esta abierto el kart no recibe mando.
        {
            unsigned short edge;
            // Pruebas: SK_ONLINE_TEST_EXIT=N simula START en el tick N y A tres ticks despues.
            static int testExit = -2;

            if (testExit == -2) {
                const char* e = getenv("SK_ONLINE_TEST_EXIT");
                testExit = e ? atoi(e) : -1;
            }
            {
                // Pruebas: SK_ONLINE_TEST_CUP=N pulsa C-arriba (cambia la distancia de la camara) durante 3 ticks desde el N.
                static int testCup = -2;

                if (testCup == -2) {
                    const char* e = getenv("SK_ONLINE_TEST_CUP");
                    testCup = e ? atoi(e) : -1;
                }
                if (testCup >= 0 && sTick >= (u32) testCup && sTick < (u32) testCup + 3) {
                    localButtons |= 0x0008;
                }
            }
            {
                // Pruebas: SK_ONLINE_TEST_DRIVE=N mantiene A pulsado desde el tick N (el kart de esta PC acelera sin
                // parar) para separar los karts y comprobar que la simulacion no depende de donde este cada camara.
                static int testDrive = -2;

                if (testDrive == -2) {
                    const char* e = getenv("SK_ONLINE_TEST_DRIVE");
                    testDrive = e != NULL ? (atoi(e) > 1 ? atoi(e) : 150) : -1;
                }
                if (testDrive >= 0 && sTick > (u32) testDrive) {
                    localButtons |= 0x8000;
                }
            }
            {
                // Pruebas: SK_ONLINE_TEST_ITEMS=1 -> el slot 0 pulsa Z cada 30 ticks durante 4 pulsaciones por cada
                // ronda de 240 ticks (al aplicarse el mando se le da un item distinto, ver mas abajo).
                static int testItems = -2;

                if (testItems == -2) {
                    testItems = getenv("SK_ONLINE_TEST_ITEMS") != NULL;
                }
                if (testItems && sTick >= 240 && NetBridge_LocalSlot() == 0) {
                    u32 phase = sTick % 240;

                    if (phase == 10 || phase == 40 || phase == 70 || phase == 100) {
                        localButtons |= 0x2000;
                    }
                }
            }
            if (testExit >= 0) {
                if (sTick == (u32) testExit) {
                    localButtons |= 0x1000;
                } else if (sTick == (u32) testExit + 3 && getenv("SK_ONLINE_TEST_EXIT_HOLD") == NULL) {
                    localButtons |= 0x8000; // con SK_ONLINE_TEST_EXIT_HOLD no se confirma: el aviso queda abierto
                }
            }
            edge = (unsigned short) (localButtons & ~sPrevLocal);

            sPrevLocal = localButtons;
            if (sExitPrompt == 0) {
                if ((edge & 0x1000) && !online_results_active()) {
                    sExitPrompt = (NetBridge_LocalSlot() == 0) ? 1 : 2;
                }
            } else if (edge & 0x8000) {
                int all = (sExitPrompt == 1);

                sExitPrompt = 0;
                if (all) {
                    NetBridge_RaceAbortAll(); // el anfitrion termina la carrera para todos
                } else {
                    NetBridge_RaceLeaveSelf(); // el jugador sale solo; los demas siguen
                }
                gOnlineRace = 0;
                if (sLog != NULL) {
                    fprintf(sLog, "SALIDA voluntaria en tick %u\n", sTick);
                    fclose(sLog);
                    sLog = NULL;
                }
                online_menu_after_race(); // anfitrion: selector de pista; los demas: lobby
                return;
            } else if (edge & (0x4000 | 0x1000)) {
                sExitPrompt = 0;
            }
        }
        // Mando local para dentro de 'delay' ticks.
        if (sExitPrompt != 0) {
            NetBridge_Submit(sTick + (u32) delay, 0, 0, 0);
        } else {
            NetBridge_Submit(sTick + (u32) delay, localButtons, localX, localY);
        }
    }

    {
        // Diagnostico de camara: SK_ONLINE_CAMDBG=archivo anota cada segundo el angulo de vision y el modo de distancia.
        static const char* camDbg = (const char*) 1;

        if (camDbg == (const char*) 1) {
            camDbg = getenv("SK_ONLINE_CAMDBG");
        }
        if (camDbg != NULL && (sTick % 30) == 0 && gScreenContexts[0].camera != NULL) {
            FILE* f = fopen(camDbg, "a");

            if (f != NULL) {
                fprintf(f, "slot %d tick %u fov %.1f modoDistancia %d\n", online_view_slot(), sTick,
                        gScreenContexts[0].camera->fieldOfView, (int) D_80164678[0]);
                fclose(f);
            }
        }
    }

    // Esperar a tener los mandos de todos los humanos para este tick.
    waitStartUs = NetBridge_NowUs();
    while (!NetBridge_GetFrame(sTick, buttons, (signed char*) stickX, (signed char*) stickY)) {
        NetBridge_Pump();
        if (!NetBridge_RaceActive()) {
            // el anfitrion se fue o se cerro la sala: se sale de la carrera en red al instante
            spins = 1000000;
        }
        // Al principio hay que dar tiempo a que todas las PCs terminen de cargar la pista (hasta ~2 min);
        // ya en carrera, ~20 s sin noticias y se abandona la carrera en red.
        if (spins == 3000 && sLog != NULL) {
            char dbg[420];
            NetBridge_DebugRace(sTick, dbg, sizeof(dbg));
            fprintf(sLog, "%s\n", dbg);
            fflush(sLog);
        }
        if (++spins > (sTick < 120 ? 120000 : 20000)) {
            gOnlineRace = 0;
            NetBridge_RaceEnd();
            if (sLog != NULL) {
                fprintf(sLog, "ABANDONADA en tick %u\n", sTick);
                fclose(sLog);
                sLog = NULL;
            }
            online_menu_after_race(); // no se queda suelto en una carrera local: vuelve a la sala / al menu
            return;
        }
    }

    NetBridge_StatsApplied(sTick, (unsigned int) (NetBridge_NowUs() - waitStartUs));

    for (i = 0; i < 16; i++) {
        gControllerPads[i].button = buttons[i];
        gControllerPads[i].stick_x = stickX[i];
        gControllerPads[i].stick_y = stickY[i];
        gControllerPads[i].right_stick_x = 0;
        gControllerPads[i].right_stick_y = 0;
    }

    if ((buttons[0] & 0x2000) && getenv("SK_ONLINE_TEST_ITEMS") != NULL) {
        // Pruebas: al llegar la Z del slot 0 (mismo tick en todas las PCs) se le da un item distinto cada ronda; el marcador
        // de items reescribe currentItemCopy en cada tick, asi que solo vale justo antes de usarlo.
        static const s8 itemList[] = { ITEM_TRIPLE_GREEN_SHELL, ITEM_BANANA, ITEM_RED_SHELL, ITEM_FAKE_ITEM_BOX,
                                       ITEM_GREEN_SHELL, ITEM_BANANA_BUNCH, ITEM_BLUE_SPINY_SHELL };

        gPlayers[0].currentItemCopy = itemList[(sTick / 240) % (sizeof(itemList) / sizeof(itemList[0]))];
    }
    if ((sTick % 30) == 0) {
        online_log_state();
        if (sLog != NULL && getenv("SK_ONLINE_LAKDBG") != NULL) {
            // Diagnostico: estado del Lakitu de salida y de la carrera.
            extern s32 D_8018D160;
            extern s32 gLakituListCount;
            int k;

            fprintf(sLog, "# lakitus=%d D160=%d racestate=%d", (int) gLakituListCount, (int) D_8018D160, (int) gRaceState);
            for (k = 0; k < gLakituListCount && k < 4; k++) {
                fprintf(sLog, " [k%d plr=%d obj=%d st=%d type=%d]", k, (int) gLakituPlayerList[k], (int) gIndexLakituList[k],
                        (int) gObjectList[gIndexLakituList[k]].state, (int) gObjectList[gIndexLakituList[k]].unk_0D8);
            }
            for (k = 0; k < 2; k++) {
                Player* pl = &gPlayers[k];

                fprintf(sLog, " {p%d lakProps=%x surf=%d eff=%x u222=%d y=%.1f sd2=%.1f mesh=%d d0D6=%d}", k,
                        (unsigned) pl->lakituProps, (int) pl->surfaceType, (unsigned) pl->effects, (int) pl->unk_222,
                        pl->pos[1], pl->collision.surfaceDistance[2], (int) pl->collision.meshIndexZX,
                        (int) gObjectList[gIndexLakituList[k < gLakituListCount ? k : 0]].unk_0D6);
            }
            fprintf(sLog, "\n");
            fflush(sLog);
        }
    }
    online_log_full();
    {
        // Pruebas: SK_ONLINE_END_AT=N termina la carrera en el tick N (igual en todas las PCs) para encadenar carreras.
        extern s32 D_802BA038;
        static int endAt = -2;
        if (endAt == -2) {
            const char* e = getenv("SK_ONLINE_END_AT");
            endAt = e ? atoi(e) : -1;
        }
        if (endAt >= 0 && sTick == (u32) endAt && gRaceState == RACE_IN_PROGRESS) {
            gRaceState = RACE_FINISHED;
            D_802BA038 = 0;
        }
    }
    sTick++;
}

void online_check_end(void) {
    if (gOnlineRace && gGamestate != RACING) {
        gOnlineRace = 0;
        NetBridge_RaceEnd();
        if (sLog != NULL) {
            fprintf(sLog, "FIN en tick %u\n", sTick);
            fclose(sLog);
            sLog = NULL;
        }
    }
}

static float sProjF[4][4], sViewF[4][4];
static int sMatValid = 0;

void online_store_view(const float proj[4][4], const float view[4][4]) {
    memcpy(sProjF, proj, sizeof(sProjF));
    memcpy(sViewF, view, sizeof(sViewF));
    sMatValid = 1;
}

int online_get_view_data(OnlineViewData* out) {
    Camera* camera;
    int i;

    if (!gOnlineRace || gScreenContexts[0].camera == NULL) {
        return 0;
    }
    camera = gScreenContexts[0].camera;
    memset(out, 0, sizeof(*out));
    out->localSlot = online_view_slot();
    out->lHeld = sLHeld;
    for (i = 0; i < 3; i++) {
        out->camPos[i] = camera->pos[i];
        out->camLook[i] = camera->lookAt[i];
        out->camUp[i] = camera->up[i];
    }
    out->fov = camera->fieldOfView;
    out->aspect = gScreenAspect;
    out->matValid = sMatValid;
    out->exitPrompt = sExitPrompt;
    memcpy(out->proj, sProjF, sizeof(sProjF));
    memcpy(out->view, sViewF, sizeof(sViewF));
    out->numKarts = NUM_PLAYERS;
    for (i = 0; i < NUM_PLAYERS; i++) {
        Player* p = &gPlayers[i];
        int lap = gLapCountByPlayerId[i] + 1;
        out->rankPlayer[i] = gGPCurrentRacePlayerIdByRank[i];
        out->kart[i].human = online_slot_is_human(i) && !((NetBridge_DroppedMask() >> i) & 1);
        out->kart[i].character = p->characterId;
        out->kart[i].rank = gGPCurrentRaceRankByPlayerId[i];
        out->kart[i].lap = lap < 1 ? 1 : (lap > 3 ? 3 : lap);
        out->kart[i].pos[0] = p->pos[0];
        out->kart[i].pos[1] = p->pos[1];
        out->kart[i].pos[2] = p->pos[2];
    }
    return 1;
}
