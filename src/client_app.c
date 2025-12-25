#define _POSIX_C_SOURCE 200809L
#include "common/net.h"
#include "common/proto.h"
#include "common/cards.h"

#include <ncursesw/ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <openssl/ssl.h>

static int recv_until(connection_t *conn, state_t *st, hand_t *hand) {
    int got_state = 0, got_hand = 0;
    uint8_t buf[2048];
    uint16_t op;
    uint32_t plen;

    while (!(got_state && got_hand)) {
        if (proto_recv(conn, &op, buf, sizeof(buf), &plen) != 0) return -1;

        if (op == OP_STATE && plen == sizeof(state_t)) {
            memcpy(st, buf, sizeof(state_t));
            got_state = 1;
        } else if (op == OP_HAND && plen == sizeof(hand_t)) {
            memcpy(hand, buf, sizeof(hand_t));
            got_hand = 1;
        } else if (op == OP_LOGIN_RESP || op == OP_RESUME_RESP) {
            // consume
        } else {
            // ignore
        }
    }
    return 0;
}

static void draw_ui(const state_t *st, const hand_t *hand) {
    erase();
    mvprintw(1, 2, "Mini TCG (Client App)  |  Keys: 1/2/3=Play  E=End Turn  Q=Quit");
    mvprintw(3, 2, "Player HP: %d", st->p_hp);
    mvprintw(4, 2, "AI     HP: %d", st->ai_hp);

    mvprintw(6, 2, "Turn: %s", (st->turn == 0) ? "PLAYER" : "AI");
    mvprintw(7, 2, "Game Over: %s", st->game_over ? "YES" : "NO");

    if (st->game_over) {
        const char *w = "NONE";
        if (st->winner == 1) w = "PLAYER";
        else if (st->winner == 2) w = "AI";
        mvprintw(8, 2, "Winner: %s", w);
    }

    mvprintw(10, 2, "Hand:");
    int n = (hand->n > 8) ? 8 : hand->n;
    for (int i = 0; i < n; i++) {
        uint16_t cid = hand->card_ids[i];
        const card_def_t *def = get_card_def(cid);
        if (def) {
            mvprintw(11 + i, 4, "%d) %s (Cost %u, Val %d)", i + 1, def->name, def->cost, def->value);
        } else {
            mvprintw(11 + i, 4, "%d) Unknown Card %u", i + 1, cid);
        }
    }

    mvprintw(20, 2, "Action: ");
    refresh();
}

int run_app_mode(const char *host, uint16_t port) {
    SSL_CTX *ctx = ssl_init_client_ctx();
    if (!ctx) return 1;

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        perror("tcp_connect");
        SSL_CTX_free(ctx);
        return 1;
    }

    // SSL Handshake
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }

    connection_t conn;
    conn_init(&conn, fd, ssl);

    // login
    if (proto_send(&conn, OP_LOGIN_REQ, NULL, 0) != 0) {
        conn_close(&conn);
        SSL_CTX_free(ctx);
        return 1;
    }

    state_t st;
    hand_t hand;
    memset(&st, 0, sizeof(st));
    memset(&hand, 0, sizeof(hand));

    // Receive initial STATE + HAND (consumes login resp implicitly via loop)
    if (recv_until(&conn, &st, &hand) != 0) {
        conn_close(&conn);
        SSL_CTX_free(ctx);
        return 1;
    }

    // ncurses init
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(200);

    while (1) {
        draw_ui(&st, &hand);

        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        if (st.game_over) {
            // keep showing final state; allow quit
            continue;
        }

        if (ch == 'e' || ch == 'E') {
            if (proto_send(&conn, OP_END_TURN, NULL, 0) != 0) break;
            if (recv_until(&conn, &st, &hand) != 0) break;
            continue;
        }

        if (ch == '1' || ch == '2' || ch == '3') {
            int idx = ch - '1';
            if (idx < 0 || idx >= (int)hand.n) continue;

            play_req_t pc;
            pc.hand_idx = (uint8_t)idx;

            if (proto_send(&conn, OP_PLAY_CARD, &pc, sizeof(pc)) != 0) break;
            if (recv_until(&conn, &st, &hand) != 0) break;
            continue;
        }
    }

    endwin();
    conn_close(&conn);
    SSL_CTX_free(ctx);
    return 0;
}
