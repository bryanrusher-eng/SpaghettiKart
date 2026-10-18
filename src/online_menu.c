#include <libultraship.h>
#include <macros.h>
#include <defines.h>
#include <common_structs.h>
#include <mk64.h>
#include <sounds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/external.h"
#include "main.h"
#include "menu_items.h"
#include "online_menu.h"
#include "online_race.h"
#include "menus.h"
#include "code_800029B0.h"
#include "port/Game.h"
#include "port/ui/OnlineMenuBridge.h"
#include "port/net/NetBridge.h"
#include "math_util_2.h"
#include "render_objects.h"
#include "racing/race_logic.h"

// Menu Online nativo. Todo el texto va en MAYUSCULAS sin acentos porque la fuente del juego solo trae A-Z, 0-9
// y unos pocos signos.

enum { ST_MAIN, ST_NAME, ST_CODE, ST_LOBBY, ST_BROWSE };

enum { ROW_CREATE, ROW_JOIN, ROW_NAME, ROW_PLAYERS, ROW_VISIBLE, ROW_EXIT, ROW_COUNT };
enum { LOBBY_PICK, LOBBY_CC, LOBBY_ROWS };

#define NAME_MAX 12
#define CODE_LEN 5
#define BROWSE_ROWS 7 // filas de la lista de salas que caben en pantalla

static const char kNameChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
static const char kCodeChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static const char* kCcNames[] = { "50CC", "100CC", "150CC" };
static const char* kCharNames[] = { "MARIO", "LUIGI", "YOSHI", "TOAD", "DK", "WARIO", "PEACH", "BOWSER" };

static int sOpen = 0;
static int sState = ST_MAIN;
static int sCursor = 0;
static int sMaxPlayers = 16;
static char sName[NAME_MAX + 1];
static int sNamePos = 0;
static int sCode[CODE_LEN];
static int sCodePos = 0;
static int sCc = 1;
static int sChar = 0;
static int sVisible = 1;          // la sala que se crea sale en la lista de salas
static int sBrowseCursor = 0;     // 0 = "escribir codigo"; 1.. = salas de la lista
static int sBrowseTop = 0;
static int sJoinedFromBrowse = 0; // si la sala se cierra, se vuelve a la lista
// Flujo nativo: 0 = ninguno, 1 = anfitrion (SELECT PLAYER y luego elegir pista), 2 = cliente (solo SELECT PLAYER).
static int sFlow = 0;
static int sPicking = 0; // despues de una carrera: el anfitrion esta eligiendo la siguiente pista
static int sFlowLeft = 0; // ya salimos del menu GAME SELECT (para saber cuando volvemos)
static char sMsg[96];

static int char_index(const char* set, char c) {
    const char* p = strchr(set, c);
    return (p != NULL && c != 0) ? (int) (p - set) : 0;
}

// Pasa un texto cualquiera a lo que la fuente del juego sabe dibujar.
static void clean(char* dst, const char* src, int size) {
    int n = 0;
    while (*src != 0 && n < size - 1) {
        unsigned char c = (unsigned char) *src++;
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char) (c - 'a' + 'A');
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-') {
            dst[n++] = (char) c;
        } else if (c < 0x80) {
            dst[n++] = ' ';
        }
    }
    dst[n] = 0;
}

static void set_msg(const char* text) {
    clean(sMsg, text, sizeof(sMsg));
}

int online_menu_active(void) {
    return sOpen;
}

void online_menu_open(void) {
    char raw[32];

    sOpen = 1;
    sChar = OnlineMenu_GetCharacter();
    sVisible = OnlineMenu_GetRoomVisible();
    sState = ST_MAIN;
    sCursor = 0;
    sMsg[0] = 0;
    OnlineMenu_GetName(raw, sizeof(raw));
    clean(sName, raw, sizeof(sName));
    if (sName[0] == 0) {
        strcpy(sName, "JUGADOR");
    }
    // La primera vez (todavia con el nombre de fabrica) se pide el nombre antes que nada.
    if (strcmp(raw, "Jugador") == 0) {
        sName[0] = 0;
        sNamePos = 0;
        sState = ST_NAME;
    }
}

static void reload_name(void) {
    char raw[32];

    OnlineMenu_GetName(raw, sizeof(raw));
    clean(sName, raw, sizeof(sName));
    if (sName[0] == 0) {
        strcpy(sName, "JUGADOR");
    }
}

static void save_name(void) {
    int len = (int) strlen(sName);
    while (len > 0 && sName[len - 1] == ' ') {
        sName[--len] = 0;
    }
    if (len == 0) {
        strcpy(sName, "JUGADOR");
    }
    OnlineMenu_SetName(sName);
}

static void sfx_move(void) {
    play_sound2(SOUND_MENU_CURSOR_MOVE);
}

static void update_main(u16 b) {
    const char* err;

    if (b & D_JPAD) {
        sCursor = (sCursor + 1) % ROW_COUNT;
        sfx_move();
    }
    if (b & U_JPAD) {
        sCursor = (sCursor + ROW_COUNT - 1) % ROW_COUNT;
        sfx_move();
    }
    if (sCursor == ROW_PLAYERS) {
        if ((b & R_JPAD) && sMaxPlayers < 16) {
            sMaxPlayers++;
            sfx_move();
        }
        if ((b & L_JPAD) && sMaxPlayers > 2) {
            sMaxPlayers--;
            sfx_move();
        }
    }
    if (sCursor == ROW_VISIBLE && (b & (L_JPAD | R_JPAD))) {
        sVisible = !sVisible;
        OnlineMenu_SetRoomVisible(sVisible);
        sfx_move();
    }
    if (b & B_BUTTON) {
        play_sound2(SOUND_MENU_GO_BACK);
        sOpen = 0;
        return;
    }
    if (b & A_BUTTON) {
        switch (sCursor) {
            case ROW_CREATE:
                sMsg[0] = 0;
                sJoinedFromBrowse = 0;
                err = OnlineMenu_Host(sMaxPlayers);
                if (err != NULL) {
                    set_msg(err);
                    play_sound2(SOUND_MENU_GO_BACK);
                } else {
                    play_sound2(SOUND_MENU_SELECT);
                    sState = ST_LOBBY;
                    sCursor = 0;
                }
                break;
            case ROW_JOIN:
                // Primero la lista de salas del servidor; desde ahi tambien se puede escribir un codigo.
                play_sound2(SOUND_MENU_SELECT);
                sState = ST_BROWSE;
                sBrowseCursor = 0;
                sBrowseTop = 0;
                sMsg[0] = 0;
                OnlineMenu_RoomsStart();
                break;
            case ROW_NAME:
                play_sound2(SOUND_MENU_SELECT);
                sState = ST_NAME;
                sNamePos = (int) strlen(sName);
                if (sNamePos >= NAME_MAX) {
                    sNamePos = NAME_MAX - 1;
                }
                break;
            case ROW_PLAYERS:
                break;
            case ROW_VISIBLE:
                sVisible = !sVisible;
                OnlineMenu_SetRoomVisible(sVisible);
                sfx_move();
                break;
            case ROW_EXIT:
                play_sound2(SOUND_MENU_GO_BACK);
                sOpen = 0;
                break;
        }
    }
}

static void update_name(u16 b) {
    int idx;
    int len = (int) strlen(sName);

    // si el cursor esta al final se anade un espacio para poder editarlo
    if (sNamePos >= len && sNamePos < NAME_MAX) {
        while (len <= sNamePos) {
            sName[len++] = ' ';
        }
        sName[len] = 0;
    }
    idx = char_index(kNameChars, sName[sNamePos]);
    if (b & U_JPAD) {
        idx = (idx + 1) % (int) (sizeof(kNameChars) - 1);
        sName[sNamePos] = kNameChars[idx];
        sfx_move();
    }
    if (b & D_JPAD) {
        idx = (idx + (int) (sizeof(kNameChars) - 1) - 1) % (int) (sizeof(kNameChars) - 1);
        sName[sNamePos] = kNameChars[idx];
        sfx_move();
    }
    if ((b & R_JPAD) && sNamePos < NAME_MAX - 1) {
        sNamePos++;
        sfx_move();
    }
    if ((b & L_JPAD) && sNamePos > 0) {
        sNamePos--;
        sfx_move();
    }
    if (b & A_BUTTON) {
        save_name();
        play_sound2(SOUND_MENU_SELECT);
        sState = ST_MAIN;
    } else if (b & B_BUTTON) {
        play_sound2(SOUND_MENU_GO_BACK);
        reload_name(); // descarta los cambios
        sState = ST_MAIN;
        sCursor = ROW_NAME;
    }
}

static void update_code(u16 b) {
    char code[CODE_LEN + 1];
    const char* err;
    int i;
    int n = (int) (sizeof(kCodeChars) - 1);

    if (b & U_JPAD) {
        sCode[sCodePos] = (sCode[sCodePos] + 1) % n;
        sfx_move();
    }
    if (b & D_JPAD) {
        sCode[sCodePos] = (sCode[sCodePos] + n - 1) % n;
        sfx_move();
    }
    if ((b & R_JPAD) && sCodePos < CODE_LEN - 1) {
        sCodePos++;
        sfx_move();
    }
    if ((b & L_JPAD) && sCodePos > 0) {
        sCodePos--;
        sfx_move();
    }
    if (b & B_BUTTON) {
        play_sound2(SOUND_MENU_GO_BACK);
        sState = ST_BROWSE;
        sBrowseCursor = 0;
        sBrowseTop = 0;
        sMsg[0] = 0;
        OnlineMenu_RoomsStart();
        return;
    }
    if (b & A_BUTTON) {
        if (sCodePos < CODE_LEN - 1) {
            sCodePos++;
            sfx_move();
            return;
        }
        for (i = 0; i < CODE_LEN; i++) {
            code[i] = kCodeChars[sCode[i]];
        }
        code[CODE_LEN] = 0;
        sJoinedFromBrowse = 0;
        err = OnlineMenu_Join(code);
        if (err != NULL) {
            set_msg(err);
            play_sound2(SOUND_MENU_GO_BACK);
        } else {
            play_sound2(SOUND_MENU_SELECT);
            sMsg[0] = 0;
            sState = ST_LOBBY;
            sCursor = 0;
        }
    }
}

static void update_browse(u16 b) {
    OnlineMenuRooms rooms;
    const OnlineMenuRoom* room;
    const char* err;
    int total;

    OnlineMenu_GetRooms(&rooms);
    total = rooms.count + 1; // la fila 0 es "escribir codigo"
    {
        // Pruebas: SK_ONLINE_MENU=browsejoin entra solo a la primera sala libre de la lista.
        static int autoJoinDone = 0;
        const char* mode = getenv("SK_ONLINE_MENU");
        int k;

        if (!autoJoinDone && mode != NULL && strcmp(mode, "browsejoin") == 0 && rooms.state == 2) {
            for (k = 0; k < rooms.count; k++) {
                if (!rooms.rooms[k].racing && rooms.rooms[k].players < rooms.rooms[k].maxPlayers) {
                    sBrowseCursor = k + 1;
                    b |= A_BUTTON;
                    autoJoinDone = 1;
                    break;
                }
            }
        }
    }
    if (sBrowseCursor >= total) {
        sBrowseCursor = total - 1;
    }
    if (b & D_JPAD) {
        sBrowseCursor = (sBrowseCursor + 1) % total;
        sfx_move();
    }
    if (b & U_JPAD) {
        sBrowseCursor = (sBrowseCursor + total - 1) % total;
        sfx_move();
    }
    if (sBrowseCursor < sBrowseTop) {
        sBrowseTop = sBrowseCursor;
    }
    if (sBrowseCursor >= sBrowseTop + BROWSE_ROWS) {
        sBrowseTop = sBrowseCursor - BROWSE_ROWS + 1;
    }
    if (b & B_BUTTON) {
        play_sound2(SOUND_MENU_GO_BACK);
        OnlineMenu_RoomsStop();
        sState = ST_MAIN;
        sCursor = ROW_JOIN;
        sMsg[0] = 0;
        return;
    }
    if (!(b & A_BUTTON)) {
        return;
    }
    if (sBrowseCursor == 0) {
        play_sound2(SOUND_MENU_SELECT);
        OnlineMenu_RoomsStop();
        sState = ST_CODE;
        sCodePos = 0;
        sMsg[0] = 0;
        return;
    }
    room = &rooms.rooms[sBrowseCursor - 1];
    if (room->racing) {
        set_msg("ESA SALA ESTA EN CARRERA");
        play_sound2(SOUND_MENU_GO_BACK);
    } else if (room->players >= room->maxPlayers) {
        set_msg("ESA SALA ESTA LLENA");
        play_sound2(SOUND_MENU_GO_BACK);
    } else {
        sJoinedFromBrowse = 1;
        err = OnlineMenu_Join(room->code);
        if (err != NULL) {
            set_msg(err);
            play_sound2(SOUND_MENU_GO_BACK);
            OnlineMenu_RoomsStart(); // unirse detiene la busqueda: se retoma
        } else {
            play_sound2(SOUND_MENU_SELECT);
            sMsg[0] = 0;
            sState = ST_LOBBY;
            sCursor = 0;
        }
    }
}

static void launch_native(int host) {
    // Entra al SELECT PLAYER del juego (y luego a la eleccion de pista si eres el anfitrion), como en el modo VS.
    sFlow = host ? 1 : 2;
    sFlowLeft = 0;
    sOpen = 0;
    gModeSelection = VERSUS;
    gPlayerCount = 1;
    gPlayerCountSelection1 = 1;
    gScreenModeSelection = SCREEN_MODE_1P;
    gCCSelection = sCc;
    gPlaceItemBoxes = 1;
    set_mirror_mode(0);
    gMainMenuSelection = MAIN_MENU_OK_SELECT;
    func_8009E1C0();
    play_sound2(SOUND_MENU_OK_CLICKED);
}

static void update_lobby(u16 b, const OnlineMenuInfo* info) {
    if (info->connected) {
        int host = (info->role == 1);
        int rows = host ? LOBBY_ROWS : 1; // el cliente solo elige su personaje
        if (sCursor >= rows) {
            sCursor = 0;
        }
        if (b & D_JPAD) {
            sCursor = (sCursor + 1) % rows;
            sfx_move();
        }
        if (b & U_JPAD) {
            sCursor = (sCursor + rows - 1) % rows;
            sfx_move();
        }
        if ((b & (L_JPAD | R_JPAD)) && host && sCursor == LOBBY_CC) {
            sCc = (sCc + 3 + ((b & R_JPAD) ? 1 : -1)) % 3;
            sfx_move();
        }
        if ((b & A_BUTTON) && sCursor == LOBBY_PICK) {
            launch_native(host);
            return;
        }
    }
    if (b & B_BUTTON) {
        play_sound2(SOUND_MENU_GO_BACK);
        OnlineMenu_Leave();
        sPicking = 0;
        sState = ST_MAIN;
        sCursor = 0;
    }
}

// Se llama cada fotograma desde update_menus: al volver al GAME SELECT desde el flujo nativo reabre la sala.
void online_menu_flow_tick(void) {
    OnlineMenuInfo info;

    {
        // Pruebas: SK_ONLINE_MENU=main o browse abre el menu Online solo al llegar al GAME SELECT.
        static int testDone = 0;
        const char* mode = getenv("SK_ONLINE_MENU");

        if (!testDone && mode != NULL && gMenuSelection == MAIN_MENU && !sOpen && !is_screen_being_faded()) {
            testDone = 1;
            online_menu_open();
            if (strcmp(mode, "imgui") == 0) {
                extern void Online_OpenMenu(void);

                sOpen = 0;
                Online_OpenMenu(); // pruebas: ventana ImGui "Online"
            } else if (strncmp(mode, "browse", 6) == 0) {
                sState = ST_BROWSE;
                sBrowseCursor = 0;
                sBrowseTop = 0;
                sMsg[0] = 0;
                OnlineMenu_RoomsStart();
            } else {
                sState = ST_MAIN;
            }
        }
    }
    if (sFlow == 0) {
        return;
    }
    if (online_race_setup_active()) {
        sFlow = 0;
        sFlowLeft = 0;
        return;
    }
    if (gMenuSelection != MAIN_MENU && gMenuSelection != MAIN_MENU_FROM_QUIT) {
        sFlowLeft = 1;
        return;
    }
    if (sFlowLeft && !is_screen_being_faded()) {
        sFlow = 0;
        sFlowLeft = 0;
        OnlineMenu_GetInfo(&info);
        if (info.role != 0) {
            sOpen = 1;
            sState = ST_LOBBY;
            sCursor = 0;
            sChar = OnlineMenu_GetCharacter();
        }
    }
}

void online_menu_after_race(void) {
    OnlineMenuInfo info;

    OnlineMenu_GetInfo(&info);
    if (info.role == 1) {
        // Anfitrion: directo al selector de copas y pistas del juego (modo VS), sin pasar por GAME SELECT.
        sFlow = 1;
        sFlowLeft = 0;
        sOpen = 0;
        gModeSelection = VERSUS;
        gPlayerCount = 1;
        gPlayerCountSelection1 = 1;
        gScreenModeSelection = SCREEN_MODE_1P;
        gCCSelection = sCc;
        gPlaceItemBoxes = 1;
        func_80290388(); // como "cambiar de pista" del menu de pausa
    } else {
        sPicking = 1;
        sFlow = 3;
        sFlowLeft = 1; // ya salimos de los menus (estamos en carrera): al llegar al GAME SELECT se reabre la sala
        func_80290338(); // salir al menu principal; ahi se reabre la sala a esperar al anfitrion
    }
}

void online_menu_return_to_lobby(void) {
    sFlow = 3;
    sFlowLeft = 0;
}

int online_menu_flow_active(void) {
    return sFlow == 1 || sFlow == 2;
}

// El jugador confirmo su personaje en el SELECT PLAYER del juego.
void online_menu_player_done(void) {
    sChar = gCharacterSelections[0];
    OnlineMenu_SetCharacter(sChar);
    if (sFlow == 1) {
        // anfitrion: sigue a la eleccion de pista
        func_8009E1C0();
        play_sound2(SOUND_MENU_OK_CLICKED);
    } else {
        // cliente: vuelve a la sala a esperar
        func_8009E208();
        play_sound2(SOUND_MENU_OK_CLICKED);
    }
}

// El anfitrion confirmo la pista en el selector de copas del juego: se inicia la carrera online.
void online_menu_course_done(void) {
    OnlineMenu_StartRace((int) GetCupIndex(), (int) GetCupCursorPosition(), sCc);
    sFlow = 0;
    sFlowLeft = 0;
}

void online_menu_update(void) {
    struct Controller* c = &gControllers[0];
    OnlineMenuInfo info;
    u16 b = c->buttonPressed | c->stickPressed;

    if (!sOpen) {
        return;
    }
    if (b & START_BUTTON) {
        b |= A_BUTTON;
    }
    OnlineMenu_GetInfo(&info);
    if (info.raceBusy) {
        sOpen = 0; // la carrera ya se esta cargando
        sPicking = 0;
        return;
    }
    if (info.role != 0 && sState != ST_LOBBY) {
        sState = ST_LOBBY;
        sCursor = 0;
    } else if (info.role == 0 && sState == ST_LOBBY) {
        // la sala se cerro o se perdio la conexion
        if (sJoinedFromBrowse) {
            sState = ST_BROWSE;
            sBrowseCursor = 0;
            sBrowseTop = 0;
            OnlineMenu_RoomsStart();
        } else {
            sState = ST_MAIN;
            sCursor = 0;
        }
        if (info.status[0] != 0) {
            set_msg(info.status);
        }
    }

    switch (sState) {
        case ST_MAIN:
            update_main(b);
            break;
        case ST_NAME:
            update_name(b);
            break;
        case ST_CODE:
            update_code(b);
            break;
        case ST_LOBBY:
            update_lobby(b, &info);
            break;
        case ST_BROWSE:
            update_browse(b);
            break;
    }
}

/* ---------------------------------- dibujado ---------------------------------- */

static void text(int x, int y, const char* s, int color, f32 scale) {
    set_text_color(color);
    print_text_mode_1(x, y, (char*) s, 0, scale, scale);
}

static void text_center(int y, const char* s, int color, f32 scale) {
    int w = (int) (get_string_width((char*) s) * scale);
    text(160 - w / 2, y, s, color, scale);
}

static void text_center_at(int cx, int y, const char* s, int color, f32 scale) {
    int w = (int) (get_string_width((char*) s) * scale);
    text(cx - w / 2, y, s, color, scale);
}

static int sel_color(int selected) {
    return selected ? TEXT_BLUE_GREEN_RED_CYCLE_1 : TEXT_BLUE;
}

static void panel(void) {
    gDisplayListHead = draw_box(gDisplayListHead, 0, 0, 320, 240, 0, 0, 0, 170);
    gDisplayListHead = draw_box(gDisplayListHead, 18, 14, 302, 226, 40, 70, 140, 255);
    gDisplayListHead = draw_box(gDisplayListHead, 21, 17, 299, 223, 10, 16, 44, 255);
    text_center(22, "ONLINE", TEXT_YELLOW, 0.9f);
}

static void render_main(void) {
    char line[64];
    int y = 56;

    text_center(y, "CREAR SALA", sel_color(sCursor == ROW_CREATE), 0.8f);
    y += 22;
    text_center(y, "UNIRSE A SALA", sel_color(sCursor == ROW_JOIN), 0.8f);
    y += 22;
    snprintf(line, sizeof(line), "NOMBRE  %s", sName);
    text_center(y, line, sel_color(sCursor == ROW_NAME), 0.8f);
    y += 22;
    snprintf(line, sizeof(line), "JUGADORES  - %d +", sMaxPlayers);
    text_center(y, line, sel_color(sCursor == ROW_PLAYERS), 0.8f);
    y += 22;
    snprintf(line, sizeof(line), "TU SALA  %s", sVisible ? "VISIBLE" : "OCULTA");
    text_center(y, line, sel_color(sCursor == ROW_VISIBLE), 0.8f);
    y += 22;
    text_center(y, "SALIR", sel_color(sCursor == ROW_EXIT), 0.8f);

    if (!OnlineMenu_ServerConfigured()) {
        text_center(180, "FALTA LA DIRECCION DEL SERVIDOR", TEXT_RED, 0.5f);
        text_center(190, "ESCRIBELA EN EL MENU DE AJUSTES F1", TEXT_RED, 0.5f);
    }
    if (sMsg[0] != 0) {
        text_center(196, sMsg, TEXT_RED, 0.5f);
    }
    text_center(210, "ARRIBA ABAJO ELEGIR   A ACEPTAR   B VOLVER", TEXT_BLUE, 0.42f);
}

static void render_name(void) {
    char one[2] = { 0, 0 };
    int i;
    int len = (int) strlen(sName);
    int x0 = 160 - (NAME_MAX * 16) / 2;

    text_center(52, "TU NOMBRE", TEXT_GREEN, 0.8f);
    for (i = 0; i < NAME_MAX; i++) {
        one[0] = (i < len && sName[i] != ' ') ? sName[i] : ' ';
        gDisplayListHead =
            draw_box(gDisplayListHead, x0 + i * 16, 96, x0 + i * 16 + 14, 114, i == sNamePos ? 70 : 30, 90, 150, 255);
        if (one[0] != ' ') {
            text(x0 + i * 16 + 2, 111, one, i == sNamePos ? TEXT_YELLOW : TEXT_BLUE, 0.75f);
        }
    }
    text_center(140, "ARRIBA ABAJO CAMBIA LA LETRA", TEXT_BLUE, 0.5f);
    text_center(152, "IZQUIERDA DERECHA MUEVE", TEXT_BLUE, 0.5f);
    text_center(210, "A GUARDAR   B CANCELAR", TEXT_BLUE, 0.5f);
}

static void render_code(void) {
    char one[2] = { 0, 0 };
    int i;
    int x0 = 160 - (CODE_LEN * 28) / 2;

    text_center(52, "CODIGO DE LA SALA", TEXT_GREEN, 0.8f);
    for (i = 0; i < CODE_LEN; i++) {
        one[0] = kCodeChars[sCode[i]];
        gDisplayListHead =
            draw_box(gDisplayListHead, x0 + i * 28, 90, x0 + i * 28 + 24, 122, i == sCodePos ? 70 : 30, 90, 150, 255);
        text(x0 + i * 28 + 4, 118, one, i == sCodePos ? TEXT_YELLOW : TEXT_BLUE, 1.0f);
    }
    text_center(140, "ARRIBA ABAJO CAMBIA LA LETRA", TEXT_BLUE, 0.5f);
    text_center(152, "IZQUIERDA DERECHA MUEVE", TEXT_BLUE, 0.5f);
    if (sMsg[0] != 0) {
        text_center(176, sMsg, TEXT_RED, 0.5f);
    }
    text_center(210, "A ENTRAR   B VOLVER", TEXT_BLUE, 0.5f);
}

static void render_browse(void) {
    OnlineMenuRooms rooms;
    char line[64];
    char name[20];
    int total;
    int i;

    OnlineMenu_GetRooms(&rooms);
    total = rooms.count + 1;
    text_center(38, "SALAS DISPONIBLES", TEXT_GREEN, 0.8f);
    for (i = sBrowseTop; i < total && i < sBrowseTop + BROWSE_ROWS; i++) {
        int y = 62 + (i - sBrowseTop) * 16;
        int sel = (i == sBrowseCursor);

        if (i == 0) {
            text(34, y, "ESCRIBIR UN CODIGO", sel_color(sel), 0.6f);
        } else {
            const OnlineMenuRoom* r = &rooms.rooms[i - 1];
            int closed = r->racing || r->players >= r->maxPlayers;
            int col = sel ? TEXT_BLUE_GREEN_RED_CYCLE_1 : (closed ? TEXT_RED : TEXT_BLUE);

            clean(name, r->name, 13);
            if (name[0] == 0) {
                strcpy(name, "SIN NOMBRE");
            }
            text(34, y, r->code, col, 0.6f);
            text(86, y, name, col, 0.6f);
            snprintf(line, sizeof(line), "%d-%d", r->players, r->maxPlayers);
            text(190, y, line, col, 0.6f);
            if (r->racing) {
                text(228, y, "EN CARRERA", col, 0.5f);
            } else if (r->players >= r->maxPlayers) {
                text(228, y, "LLENA", col, 0.5f);
            }
        }
    }
    if (sBrowseTop > 0) {
        text_center(54, "...", TEXT_BLUE, 0.5f);
    }
    if (sBrowseTop + BROWSE_ROWS < total) {
        text_center(176, "...", TEXT_BLUE, 0.5f);
    }
    if (!OnlineMenu_ServerConfigured()) {
        text_center(152, "FALTA LA DIRECCION DEL SERVIDOR", TEXT_RED, 0.5f);
    } else if (rooms.state == 1) {
        text_center(188, "BUSCANDO SALAS...", TEXT_YELLOW, 0.5f);
    } else if (rooms.state == 3) {
        clean(line, rooms.error, sizeof(line));
        text_center(188, line, TEXT_RED, 0.45f);
    } else if (rooms.state == 2 && rooms.count == 0) {
        text_center(188, "NO HAY SALAS DISPONIBLES", TEXT_YELLOW, 0.5f);
    }
    if (sMsg[0] != 0) {
        text_center(198, sMsg, TEXT_RED, 0.5f);
    }
    text_center(212, "ARRIBA ABAJO ELEGIR   A UNIRSE   B VOLVER", TEXT_BLUE, 0.42f);
}

static void render_lobby(const OnlineMenuInfo* info) {
    char line[96];
    char name[40];
    int i;
    int y;
    int host = (info->role == 1 && info->connected);

    if (info->relay && info->roomCode[0] != 0) {
        snprintf(line, sizeof(line), "SALA  %s", info->roomCode);
        text_center(40, line, TEXT_GREEN, 0.9f);
    } else {
        text_center(40, info->connected ? "SALA" : "CONECTANDO", TEXT_GREEN, 0.9f);
    }
    clean(line, info->status, sizeof(line));
    text_center(52, line, TEXT_BLUE, 0.42f);
    // cabecera de la columna de victorias
    text(34 + 108, 61, "VICT", TEXT_GREEN, 0.4f);
    if (info->numPeers > 8) {
        text(34 + 138 + 108, 61, "VICT", TEXT_GREEN, 0.4f);
    }

    for (i = 0; i < info->numPeers && i < 16; i++) {
        const OnlineMenuPeer* p = &info->peers[i];
        int col = (i < 8) ? 0 : 1;
        int x = 34 + col * 138;
        y = 70 + (i % 8) * 10;
        clean(name, p->name, 11);
        {
            int ch = (p->character >= 0 && p->character < 8) ? p->character : 0;
            char wins[16];

            snprintf(line, sizeof(line), "%d %s  %s", p->slot + 1, name, kCharNames[ch]);
            text(x, y, line, p->isSelf ? TEXT_GREEN : TEXT_BLUE, 0.5f);
            // Victorias: columna propia a la derecha de cada jugador, en verde (amarillo si va ganando)
            snprintf(wins, sizeof(wins), "%d", p->wins);
            text(x + 112, y, wins, p->wins > 0 ? TEXT_YELLOW : TEXT_BLUE, 0.55f);
        }
    }

    if (info->cpuWins > 0) {
        snprintf(line, sizeof(line), "VICTORIAS DE LOS CPU  %d", info->cpuWins);
        text(34, 70 + ((info->numPeers < 8 ? info->numPeers : 8) * 10) + 4, line, TEXT_RED, 0.5f);
    }
    if (info->connected) {
        snprintf(line, sizeof(line), "TU PERSONAJE  %s", kCharNames[(sChar >= 0 && sChar < 8) ? sChar : 0]);
        text_center(148, line, TEXT_GREEN, 0.55f);
    }
    if (host) {
        y = 166;
        text_center(y, "ELEGIR PERSONAJE Y PISTA", sel_color(sCursor == LOBBY_PICK), 0.7f);
        y += 16;
        snprintf(line, sizeof(line), "- %s +", kCcNames[sCc]);
        text_center(y, line, sel_color(sCursor == LOBBY_CC), 0.6f);
        text_center(212, "ARRIBA ABAJO ELEGIR   IZQ DER CILINDRADA   B SALIR", TEXT_BLUE, 0.4f);
    } else if (info->role == 2 && info->connected) {
        text_center(168, "ELEGIR PERSONAJE", sel_color(sCursor == LOBBY_PICK), 0.7f);
        text_center(190, sPicking ? "EL ANFITRION ESTA ELIGIENDO LA PISTA..." : "ESPERANDO AL ANFITRION...", TEXT_YELLOW,
                    0.55f);
        text_center(212, "A ELEGIR PERSONAJE   B SALIR DE LA SALA", TEXT_BLUE, 0.45f);
    } else {
        text_center(212, "B CANCELAR", TEXT_BLUE, 0.45f);
    }
}

void online_menu_render(void) {
    OnlineMenuInfo info;

    if (!sOpen) {
        return;
    }
    // Si la carrera ya se esta cargando o corre, el menu se cierra solo (el codigo de los menus no corre en carrera).
    if (online_race_setup_active() || gGamestate == RACING) {
        sOpen = 0;
        sPicking = 0;
        return;
    }
    OnlineMenu_GetInfo(&info);
    panel();
    switch (sState) {
        case ST_MAIN:
            render_main();
            break;
        case ST_NAME:
            render_name();
            break;
        case ST_CODE:
            render_code();
            break;
        case ST_LOBBY:
            render_lobby(&info);
            break;
        case ST_BROWSE:
            render_browse();
            break;
    }
}

/* ---------------------------- resultados de la carrera ---------------------------- */

void online_results_render(void) {
    OnlineViewData v;
    char line[64];
    char name[32];
    int rank;
    int local;

    if (!online_results_active() || !online_get_view_data(&v)) {
        return;
    }
    local = v.localSlot;
    set_matrix_hud_screen();
    gDisplayListHead = draw_box(gDisplayListHead, 0, 0, 320, 240, 0, 0, 0, 150);
    gDisplayListHead = draw_box(gDisplayListHead, 14, 14, 306, 226, 40, 70, 140, 255);
    gDisplayListHead = draw_box(gDisplayListHead, 17, 17, 303, 223, 10, 16, 44, 255);
    text_center(22, "RESULTADOS", TEXT_YELLOW, 0.8f);

    // 16 casillas (4 x 4): retrato pequeño, puesto y, debajo, el nombre del jugador (o el personaje si es CPU)
    for (rank = 0; rank < v.numKarts && rank < 16; rank++) {
        int slot = v.rankPlayer[rank];
        int col = rank % 4;
        int row = rank / 4;
        int cx = 55 + col * 70;     // centro de la casilla
        int top = 38 + row * 41;
        int ch;
        int human;
        int color;
        u32 br;
        u32 bg;
        u32 bb;

        if (slot < 0 || slot >= 16) {
            continue;
        }
        ch = v.kart[slot].character;
        if (ch < 0 || ch > 7) {
            ch = 0;
        }
        human = v.kart[slot].human || online_slot_is_human(slot);
        if (human) {
            clean(name, NetBridge_SlotName(slot), 10);
            if (name[0] == 0) {
                snprintf(name, sizeof(name), "JUGADOR %d", slot + 1);
            }
            color = (slot == local) ? TEXT_GREEN : TEXT_YELLOW;
            br = (slot == local) ? 90 : 255;
            bg = 255;
            bb = (slot == local) ? 120 : 90;
        } else {
            snprintf(name, sizeof(name), "%s", kCharNames[ch]);
            color = TEXT_BLUE;
            br = 120;
            bg = 140;
            bb = 200;
        }
        draw_portrait_small(cx - 11, top + 11, ch, 0.7f, 0, 1, br, bg, bb, 255);
        snprintf(line, sizeof(line), "%d", rank + 1);
        text(cx - 33, top + 17, line, rank == 0 ? TEXT_RED : TEXT_YELLOW, 0.55f);
        text_center_at(cx, top + 34, name, color, 0.45f);
        if (human) {
            // victorias de este jugador, a la derecha del retrato (el ganador ya las trae sumadas)
            snprintf(line, sizeof(line), "%dV", NetBridge_Wins(slot));
            text(cx + 6, top + 9, line, rank == 0 ? TEXT_YELLOW : TEXT_GREEN, 0.7f);
        }
    }
    if (NetBridge_CpuWins() > 0) {
        snprintf(line, sizeof(line), "VICTORIAS DE LOS CPU  %d", NetBridge_CpuWins());
        text_center(205, line, TEXT_RED, 0.4f);
    }
    snprintf(line, sizeof(line), "SIGUIENTE CARRERA EN %d", online_results_seconds_left());
    text_center(214, line, TEXT_BLUE, 0.45f);
}
