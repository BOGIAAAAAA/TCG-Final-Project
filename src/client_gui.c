#define _POSIX_C_SOURCE 200809L
#include "common/net.h"
#include "common/proto.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "raylib.h"

typedef struct {
    state_t st;
    hand_t  hand;
    int has_state;
    int has_hand;
    int connected;
    int game_over;
} shared_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static shared_t g_sh;

static int recv_until(int fd, state_t *st, hand_t *hand) {
    int got_state = 0, got_hand = 0;
    uint8_t buf[4096];
    uint16_t op;
    uint32_t plen;

    while (!(got_state && got_hand)) {
        if (proto_recv(fd, &op, buf, sizeof(buf), &plen) != 0) return -1;

        if (op == OP_STATE && plen == sizeof(state_t)) {
            memcpy(st, buf, sizeof(state_t));
            got_state = 1;
        } else if (op == OP_HAND && plen == sizeof(hand_t)) {
            memcpy(hand, buf, sizeof(hand_t));
            got_hand = 1;
        } else if (op == OP_ERROR && plen == sizeof(error_t)) {
            // optional: could display error
            // ignore here; UI will show state soon anyway
        } else {
            // ignore others
        }
    }
    return 0;
}

typedef struct {
    const char *host;
    uint16_t port;
    int fd_out;
} net_arg_t;

static void* net_thread(void *p) {
    net_arg_t *a = (net_arg_t*)p;
    int fd = tcp_connect(a->host, a->port);
    if (fd < 0) {
        pthread_mutex_lock(&g_mu);
        g_sh.connected = 0;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    a->fd_out = fd;

    if (proto_send(fd, OP_LOGIN_REQ, NULL, 0) != 0) {
        close(fd);
        pthread_mutex_lock(&g_mu);
        g_sh.connected = 0;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }

    state_t st;
    hand_t hand;
    memset(&st, 0, sizeof(st));
    memset(&hand, 0, sizeof(hand));

    if (recv_until(fd, &st, &hand) != 0) {
        close(fd);
        pthread_mutex_lock(&g_mu);
        g_sh.connected = 0;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }

    pthread_mutex_lock(&g_mu);
    g_sh.st = st;
    g_sh.hand = hand;
    g_sh.has_state = 1;
    g_sh.has_hand = 1;
    g_sh.connected = 1;
    g_sh.game_over = st.game_over ? 1 : 0;
    pthread_mutex_unlock(&g_mu);

    while (1) {
        if (recv_until(fd, &st, &hand) != 0) break;

        pthread_mutex_lock(&g_mu);
        g_sh.st = st;
        g_sh.hand = hand;
        g_sh.has_state = 1;
        g_sh.has_hand = 1;
        g_sh.game_over = st.game_over ? 1 : 0;
        pthread_mutex_unlock(&g_mu);
    }

    close(fd);
    pthread_mutex_lock(&g_mu);
    g_sh.connected = 0;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static bool PointInRect(Vector2 p, Rectangle r) {
    return (p.x >= r.x && p.x <= r.x + r.width &&
            p.y >= r.y && p.y <= r.y + r.height);
}

static const char* card_type_name(uint8_t t) {
    switch (t) {
        case CARD_ATTACK: return "ATK";
        case CARD_HEAL:   return "HEAL";
        case CARD_SHIELD: return "SHD";
        case CARD_BUFF:   return "BUFF";
        case CARD_POISON: return "PSN";
        default: return "UNK";
    }
}

static const char* winner_name(uint8_t w) {
    if (w == 1) return "PLAYER";
    if (w == 2) return "AI";
    return "NONE";
}

static void draw_logs(const state_t *st, int x, int y) {
    DrawText("Battle Log:", x, y, 20, DARKGRAY);

    // show in chronological order (oldest -> newest)
    // Since log_head points to next write, oldest is log_head.
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = (st->log_head + i) % LOG_LINES;
        const char *line = st->logs[idx];
        if (line[0] == '\0') line = "-";
        DrawText(line, x, y + 26 + i * 22, 18, GRAY);
    }
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    uint16_t port = 9000;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = (uint16_t)atoi(argv[2]);

    memset(&g_sh, 0, sizeof(g_sh));

    net_arg_t na = { .host = host, .port = port, .fd_out = -1 };
    pthread_t nt;
    pthread_create(&nt, NULL, net_thread, &na);

    const int W = 900, H = 600;
    InitWindow(W, H, "Mini TCG - GUI Client (raylib)");
    if (!IsWindowReady()) {
        fprintf(stderr, "ERROR: GUI init failed (OpenGL/GLX missing). Try software rendering.\n");
        return 1;
    }
    SetTargetFPS(60);

    Rectangle cardRect[3] = {
        { 60,  420, 220, 140 },
        { 340, 420, 220, 140 },
        { 620, 420, 220, 140 }
    };
    Rectangle endBtn = { 740, 40, 130, 50 };

    while (!WindowShouldClose()) {
        // snapshot shared state
        shared_t sh;
        pthread_mutex_lock(&g_mu);
        sh = g_sh;
        pthread_mutex_unlock(&g_mu);

        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawText("Mini TCG (raylib GUI)  |  Click a card to play", 30, 20, 24, DARKGRAY);

        if (!sh.connected && !sh.has_state) {
            DrawText("Connecting...", 30, 80, 22, MAROON);
            EndDrawing();
            continue;
        }

        // status panel
        char buf[256];
        if (sh.has_state) {
            // HP
            snprintf(buf, sizeof(buf), "Player HP: %d", sh.st.p_hp);
            DrawText(buf, 30, 70, 22, BLACK);
            snprintf(buf, sizeof(buf), "AI HP: %d", sh.st.ai_hp);
            DrawText(buf, 30, 100, 22, BLACK);

            // Mana + statuses
            snprintf(buf, sizeof(buf), "Mana: %u / %u", (unsigned)sh.st.mana, (unsigned)sh.st.max_mana);
            DrawText(buf, 30, 135, 22, BLACK);

            snprintf(buf, sizeof(buf), "P Shield: %d   P Buff: %d   P Poison: %u",
                     sh.st.p_shield, sh.st.p_buff, (unsigned)sh.st.p_poison);
            DrawText(buf, 30, 170, 20, DARKGRAY);

            snprintf(buf, sizeof(buf), "AI Shield: %d  AI Buff: %d  AI Poison: %u",
                     sh.st.ai_shield, sh.st.ai_buff, (unsigned)sh.st.ai_poison);
            DrawText(buf, 30, 195, 20, DARKGRAY);

            // Turn
            const char *turn = (sh.st.turn == 0) ? "PLAYER" : "AI";
            snprintf(buf, sizeof(buf), "Turn: %s", turn);
            DrawText(buf, 30, 230, 22, BLACK);

            // Game over
            snprintf(buf, sizeof(buf), "Game Over: %s", sh.st.game_over ? "YES" : "NO");
            DrawText(buf, 30, 260, 22, BLACK);

            if (sh.st.game_over) {
                snprintf(buf, sizeof(buf), "Winner: %s", winner_name(sh.st.winner));
                DrawText(buf, 30, 292, 24, MAROON);
            }

            // Logs
            draw_logs(&sh.st, 520, 70);
        }

        // End Turn button
        DrawRectangleRec(endBtn, LIGHTGRAY);
        DrawRectangleLines((int)endBtn.x, (int)endBtn.y, (int)endBtn.width, (int)endBtn.height, GRAY);
        DrawText("End Turn", (int)endBtn.x + 18, (int)endBtn.y + 15, 20, BLACK);

        // cards
        DrawText("Hand:", 30, 380, 22, DARKGRAY);

        int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
        for (int i = 0; i < 3; i++) {
            DrawRectangleRec(cardRect[i], (i < n) ? BEIGE : LIGHTGRAY);
            DrawRectangleLines((int)cardRect[i].x, (int)cardRect[i].y,
                               (int)cardRect[i].width, (int)cardRect[i].height, GRAY);

            if (i < n) {
                const card_t *c = &sh.hand.cards[i];

                snprintf(buf, sizeof(buf), "Card %d", i + 1);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 12, 22, BLACK);

                snprintf(buf, sizeof(buf), "Type: %s", card_type_name(c->type));
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 44, 20, DARKGRAY);

                snprintf(buf, sizeof(buf), "Cost: %u", (unsigned)c->cost);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 70, 20, DARKGRAY);

                snprintf(buf, sizeof(buf), "Value: %d", (int)c->value);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 96, 20, DARKGRAY);

                snprintf(buf, sizeof(buf), "ID: %u", (unsigned)c->id);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 120, 18, GRAY);
            } else {
                DrawText("(empty)", (int)cardRect[i].x + 12, (int)cardRect[i].y + 12, 20, GRAY);
            }
        }

        // input handling
        Vector2 mp = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            sh.connected && sh.has_state && sh.has_hand && !sh.st.game_over) {

            if (na.fd_out >= 0) {
                // End turn
                if (PointInRect(mp, endBtn)) {
                    proto_send(na.fd_out, OP_END_TURN, NULL, 0);
                } else {
                    // play card by index
                    for (int i = 0; i < n; i++) {
                        if (PointInRect(mp, cardRect[i])) {
                            play_req_t pr;
                            pr.hand_idx = (uint8_t)i;
                            proto_send(na.fd_out, OP_PLAY_CARD, &pr, sizeof(pr));
                            break;
                        }
                    }
                }
            }
        }

        EndDrawing();
    }

    CloseWindow();
    pthread_detach(nt);
    return 0;
}
