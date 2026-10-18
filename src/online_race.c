#include <libultraship.h>
#include <macros.h>
#include <defines.h>
#include <common_structs.h>
#include <mk64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

extern OSContPad gControllerPads[];
extern u16 gRandomSeed16;
void func_8000F124(void);

s32 gOnlineRace = 0;
s32 gOnlineRaceSetup = 0;

static NetBridgeRace sParams;
static int sLHeld = 0; // el boton L del mando local esta pulsado
static u32 sHumanMask = 0;
static u32 sTick = 0;
static FILE* sLog = NULL;
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
        SetCupCursorPosition(track);
        gCupSelection = cup;
        gCourseIndexInCup = track;
        gCurrentCourseId = gCupCourseOrder[cup][track];
        TrackBrowser_SetTrackFromCup();
        D_800DC540 = GetCupIndex();
    }
    gCharacterSelections[0] = online_slot_character(0);
    gDemoMode = 0;
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

    // La pantalla de esta PC sigue a su propio kart.
    gScreenContexts[0].player = &gPlayers[online_view_slot()];

    logName = getenv("SK_ONLINE_LOG");
    if (logName != NULL && sLog == NULL) {
        sLog = fopen(logName, "w");
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
    fprintf(sLog, "tick %u hash %016llx p0=(%.2f,%.2f) p1=(%.2f,%.2f)\n", sTick, (unsigned long long) h,
            gPlayers[0].pos[0], gPlayers[0].pos[2], gPlayers[1].pos[0], gPlayers[1].pos[2]);
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

    sLHeld = (gControllerPads[0].button & 0x0020) != 0; // CONT_L
    // Mando local (puerto 1) para dentro de 'delay' ticks.
    NetBridge_Submit(sTick + (u32) delay, gControllerPads[0].button, gControllerPads[0].stick_x,
                     gControllerPads[0].stick_y);

    // Esperar a tener los mandos de todos los humanos para este tick.
    while (!NetBridge_GetFrame(sTick, buttons, (signed char*) stickX, (signed char*) stickY)) {
        NetBridge_Pump();
        if (!NetBridge_RaceActive()) {
            // el anfitrion se fue o se cerro la sala: se sale de la carrera en red al instante
            spins = 1000000;
        }
        // Al principio hay que dar tiempo a que todas las PCs terminen de cargar la pista (hasta ~2 min);
        // ya en carrera, ~20 s sin noticias y se abandona la carrera en red.
        if (++spins > (sTick < 120 ? 120000 : 20000)) {
            gOnlineRace = 0;
            NetBridge_RaceEnd();
            if (sLog != NULL) {
                fprintf(sLog, "ABANDONADA en tick %u\n", sTick);
                fclose(sLog);
                sLog = NULL;
            }
            return;
        }
    }

    for (i = 0; i < 16; i++) {
        gControllerPads[i].button = buttons[i];
        gControllerPads[i].stick_x = stickX[i];
        gControllerPads[i].stick_y = stickY[i];
        gControllerPads[i].right_stick_x = 0;
        gControllerPads[i].right_stick_y = 0;
    }

    if ((sTick % 30) == 0) {
        online_log_state();
    }
    online_log_full();
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
