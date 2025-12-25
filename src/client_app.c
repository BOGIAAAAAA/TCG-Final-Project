#define _POSIX_C_SOURCE 200809L
#include "common/net.h"
#include "common/proto.h"
#include <errno.h>

#include <ncursesw/ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

static int recv_until(int fd, SSL *ssl, state_t *st, hand_t *hand) {

    int got_state = 0, got_hand = 0;
    uint8_t buf[2048];
    uint16_t op;
    uint32_t plen;

    while (!(got_state && got_hand)) {
        /* han edit tls */
        if (proto_recv(fd, ssl, &op, buf, sizeof(buf), &plen) != 0) {
        /* han edit tls end */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
               endwin(); fprintf(stderr, "[client] Receive timeout (waited > 3s)\n");
            } else {
               endwin(); perror("[client] recv error");
            }
            return -1;
        }

        if (op == OP_STATE && plen == sizeof(state_t)) {
            memcpy(st, buf, sizeof(state_t));
            got_state = 1;
        } else if (op == OP_HAND && plen == sizeof(hand_t)) {
            memcpy(hand, buf, sizeof(hand_t));
            got_hand = 1;
        } else {
            // ignore OP_LOGIN_RESP / OP_ERROR / unknown
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
        mvprintw(11 + i, 4, "%d) CardID=%u  Value=%d", i + 1, hand->cards[i].id, hand->cards[i].value);
    }

    mvprintw(20, 2, "Action: ");
    refresh();
}

int run_app_mode(const char *host, uint16_t port) {
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        perror("tcp_connect");
        return 1;
    }
    
    net_set_timeout(fd, 3);

    /* han edit tls */
    net_init_ssl();
    SSL_CTX *ctx = net_create_context(0);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return 1;
    }
    /* han edit tls end */

    // login
    /* han edit tls */
    if (proto_send(fd, ssl, OP_LOGIN_REQ, NULL, 0) != 0) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx);
        close(fd);
        return 1;
    }
    /* han edit tls end */

    state_t st;
    hand_t hand;
    memset(&st, 0, sizeof(st));
    memset(&hand, 0, sizeof(hand));

    // Receive initial STATE + HAND
    /* han edit tls */
    if (recv_until(fd, ssl, &st, &hand) != 0) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx);
        close(fd);
        return 1;
    }
    /* han edit tls end */

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
            /* han edit tls */
            if (proto_send(fd, ssl, OP_END_TURN, NULL, 0) != 0) break;
            if (recv_until(fd, ssl, &st, &hand) != 0) break;
            /* han edit tls end */
            continue;
        }

        if (ch == '1' || ch == '2' || ch == '3') {
            int idx = ch - '1';
            if (idx < 0 || idx >= (int)hand.n) continue;

            play_req_t pc;
            pc.hand_idx = (uint8_t)idx;

            pc.hand_idx = (uint8_t)idx;

            /* han edit tls */
            if (proto_send(fd, ssl, OP_PLAY_CARD, &pc, sizeof(pc)) != 0) break;
            if (recv_until(fd, ssl, &st, &hand) != 0) break;
            /* han edit tls end */
            continue;
        }
    }

    endwin();
    /* han edit tls */
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    /* han edit tls end */
    close(fd);
    return 0;
}
