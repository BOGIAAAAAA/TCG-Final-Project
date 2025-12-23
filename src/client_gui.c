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
    uint8_t buf[2048];
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

    // keep receiving updates after actions (each action expects STATE+HAND)
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

        DrawText("Mini TCG (raylib GUI)", 30, 20, 28, DARKGRAY);

        if (!sh.connected && !sh.has_state) {
            DrawText("Connecting...", 30, 80, 22, MAROON);
            EndDrawing();
            continue;
        }

        // status panel
        char buf[256];
        if (sh.has_state) {
            snprintf(buf, sizeof(buf), "Player HP: %d", sh.st.p_hp);
            DrawText(buf, 30, 80, 22, BLACK);
            snprintf(buf, sizeof(buf), "AI HP: %d", sh.st.ai_hp);
            DrawText(buf, 30, 110, 22, BLACK);

            const char *turn = (sh.st.turn == 0) ? "PLAYER" : "AI";
            snprintf(buf, sizeof(buf), "Turn: %s", turn);
            DrawText(buf, 30, 150, 22, BLACK);

            snprintf(buf, sizeof(buf), "Game Over: %s", sh.st.game_over ? "YES" : "NO");
            DrawText(buf, 30, 180, 22, BLACK);

            if (sh.st.game_over) {
                const char *w = "NONE";
                if (sh.st.winner == 1) w = "PLAYER";
                else if (sh.st.winner == 2) w = "AI";
                snprintf(buf, sizeof(buf), "Winner: %s", w);
                DrawText(buf, 30, 210, 22, MAROON);
            }
        }

        // End Turn button
        DrawRectangleRec(endBtn, LIGHTGRAY);
        DrawRectangleLines((int)endBtn.x, (int)endBtn.y, (int)endBtn.width, (int)endBtn.height, GRAY);
        DrawText("End Turn", (int)endBtn.x + 18, (int)endBtn.y + 15, 20, BLACK);

        // cards
        DrawText("Hand (click a card to play):", 30, 380, 22, DARKGRAY);

        int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
        for (int i = 0; i < 3; i++) {
            DrawRectangleRec(cardRect[i], (i < n) ? BEIGE : LIGHTGRAY);
            DrawRectangleLines((int)cardRect[i].x, (int)cardRect[i].y, (int)cardRect[i].width, (int)cardRect[i].height, GRAY);

            if (i < n) {
                snprintf(buf, sizeof(buf), "Card %d", i + 1);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 12, 22, BLACK);
                snprintf(buf, sizeof(buf), "ID: %u", sh.hand.card_id[i]);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 48, 20, DARKGRAY);
                snprintf(buf, sizeof(buf), "DMG: %u", sh.hand.dmg[i]);
                DrawText(buf, (int)cardRect[i].x + 12, (int)cardRect[i].y + 76, 20, DARKGRAY);
            } else {
                DrawText("(empty)", (int)cardRect[i].x + 12, (int)cardRect[i].y + 12, 20, GRAY);
            }
        }

        // input handling
        Vector2 mp = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && sh.connected && sh.has_state && sh.has_hand && !sh.st.game_over) {
            // End turn
            if (PointInRect(mp, endBtn) && na.fd_out >= 0) {
                proto_send(na.fd_out, OP_END_TURN, NULL, 0);
            } else {
                // play card
                for (int i = 0; i < n; i++) {
                    if (PointInRect(mp, cardRect[i]) && na.fd_out >= 0) {
                        play_card_t pc;
                        pc.card_id = sh.hand.card_id[i];
                        pc.dmg = sh.hand.dmg[i];
                        proto_send(na.fd_out, OP_PLAY_CARD, &pc, sizeof(pc));
                        break;
                    }
                }
            }
        }

        EndDrawing();
    }

    CloseWindow();
    // net thread will exit when server closes or program ends; we can detach
    pthread_detach(nt);
    return 0;
}
