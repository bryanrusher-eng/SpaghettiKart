#include <libultraship.h>
#include <macros.h>
#include <defines.h>
#include <common_structs.h>
#include <mk64.h>
#include <sounds.h>
#include <stdio.h>
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

// Menu Online nativo. Todo el texto va en MAYUSCULAS sin acentos porque la fuente del juego solo trae A-Z, 0-9
// y unos pocos signos.

enum { ST_MAIN, ST_NAME, ST_CODE, ST_LOBBY };

enum { ROW_CREATE, ROW_JOIN, ROW_NAME, ROW_PLAYERS, ROW_EXIT, ROW_COUNT };
enum { LOBBY_PICK, LOBBY_CC, LOBBY_ROWS };

#define NAME_MAX 12
#define CODE_LEN 5

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
// Flujo nativo: 0 = ninguno, 1 = anfitrion (SELECT PLAYER y luego elegir pista), 2 = cliente (solo SELECT PLAYER).
static int sFlow = 0;
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
    sState = ST_MAIN;
    sCursor = 0;
    sMsg[0] = 0;
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
    if (b & B_BUTTON) {
        play_sound2(SOUND_MENU_GO_BACK);
        sOpen = 0;
        return;
    }
    if (b & A_BUTTON) {
        switch (sCursor) {
            case ROW_CREATE:
                sMsg[0] = 0;
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
                play_sound2(SOUND_MENU_SELECT);
                sState = ST_CODE;
                sCodePos = 0;
                sMsg[0] = 0;
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
        online_menu_open(); // descarta los cambios
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
        sState = ST_MAIN;
        sCursor = ROW_JOIN;
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
        sState = ST_MAIN;
        sCursor = 0;
    }
}

// Se llama cada fotograma desde update_menus: al volver al GAME SELECT desde el flujo nativo reabre la sala.
void online_menu_flow_tick(void) {
    OnlineMenuInfo info;

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

int online_menu_flow_active(void) {
    return sFlow != 0;
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
        return;
    }
    if (info.role != 0 && sState != ST_LOBBY) {
        sState = ST_LOBBY;
        sCursor = 0;
    } else if (info.role == 0 && sState == ST_LOBBY) {
        // la sala se cerro o se perdio la conexion
        sState = ST_MAIN;
        sCursor = 0;
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
    text_center(58, line, TEXT_BLUE, 0.42f);

    for (i = 0; i < info->numPeers && i < 16; i++) {
        const OnlineMenuPeer* p = &info->peers[i];
        int col = (i < 8) ? 0 : 1;
        int x = 34 + col * 138;
        y = 70 + (i % 8) * 10;
        clean(name, p->name, 11);
        {
            int ch = (p->character >= 0 && p->character < 8) ? p->character : 0;
            if (p->isSelf) {
                snprintf(line, sizeof(line), "%d %s  %s", p->slot + 1, name, kCharNames[ch]);
            } else {
                snprintf(line, sizeof(line), "%d %s  %s  %dMS", p->slot + 1, name, kCharNames[ch], p->ping);
            }
        }
        text(x, y, line, p->isSelf ? TEXT_GREEN : TEXT_BLUE, 0.5f);
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
        text_center(190, "ESPERANDO AL ANFITRION...", TEXT_YELLOW, 0.55f);
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
    }
}
