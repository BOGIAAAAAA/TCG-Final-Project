#include "common/net.h"
#include "common/proto.h"
#include "common/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* forward declarations */
static void on_sigint(int);
static void on_sigchld(int);

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static void on_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static void run_session(int cfd, shm_stats_t *stats) {
    // Simplified game: player vs AI
    state_t st = {
        .p_hp = 30, .ai_hp = 30, .turn = 0,
        .game_over = 0, .winner = 0, .reserved = 0
    };
    hand_t hand;
    memset(&hand, 0, sizeof(hand));
    hand.n = 3;
    hand.card_id[0] = 1; hand.dmg[0] = 3;
    hand.card_id[1] = 2; hand.dmg[1] = 5;
    hand.card_id[2] = 3; hand.dmg[2] = 7;

    uint8_t payload[1024];
    for (;;) {
        uint16_t op = 0;
        uint32_t plen = 0;
        if (proto_recv(cfd, &op, payload, sizeof(payload), &plen) != 0) break;

        ipc_stats_inc_pkt(stats);

        if (op == OP_LOGIN_REQ) {
            login_resp_t resp = { .ok = 1 };
            if (proto_send(cfd, OP_LOGIN_RESP, &resp, sizeof(resp)) != 0) break;
            if (proto_send(cfd, OP_STATE, &st, sizeof(st)) != 0) break;
            if (proto_send(cfd, OP_HAND, &hand, sizeof(hand)) != 0) break;
            continue;
        }

        if (st.game_over) {
            if (proto_send(cfd, OP_STATE, &st, sizeof(st)) != 0) break;
            continue;
        }

        if (op == OP_PLAY_CARD) {
            if (plen != sizeof(play_card_t)) {
                proto_send(cfd, OP_ERROR, NULL, 0);
                continue;
            }
            if (st.turn != 0) {
                proto_send(cfd, OP_ERROR, NULL, 0);
                continue;
            }
            play_card_t pc;
            memcpy(&pc, payload, sizeof(pc));

            // Apply damage to AI (bounded)
            int dmg = (int)pc.dmg;
            if (dmg < 1) dmg = 1;
            if (dmg > 10) dmg = 10;

            st.ai_hp -= (int16_t)dmg;
            if (st.ai_hp <= 0) {
                st.ai_hp = 0;
                st.game_over = 1;
                st.winner = 1;
            }
            if (proto_send(cfd, OP_STATE, &st, sizeof(st)) != 0) break;
            if (proto_send(cfd, OP_HAND, &hand, sizeof(hand)) != 0) break;
            continue;
        }

        if (op == OP_END_TURN) {
            if (st.turn != 0) {
                proto_send(cfd, OP_ERROR, NULL, 0);
                continue;
            }
            // AI turn: simple attack
            st.turn = 1;

            int ai_dmg = 3; // fixed for MVP
            st.p_hp -= (int16_t)ai_dmg;
            if (st.p_hp <= 0) {
                st.p_hp = 0;
                st.game_over = 1;
                st.winner = 2;
            }
            st.turn = 0; // back to player
            if (proto_send(cfd, OP_STATE, &st, sizeof(st)) != 0) break;
            if (proto_send(cfd, OP_HAND, &hand, sizeof(hand)) != 0) break;
            continue;
        }

        proto_send(cfd, OP_ERROR, NULL, 0);
    }

    close(cfd);
}

int main(int argc, char **argv) {
    uint16_t port = 9000;
    if (argc >= 2) port = (uint16_t)atoi(argv[1]);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, on_sigchld);

    shm_stats_t *stats = ipc_stats_init(1);
    if (!stats) {
        perror("ipc_stats_init");
        return 1;
    }

    int lfd = tcp_listen(port);
    if (lfd < 0) {
        perror("tcp_listen");
        return 1;
    }
    fprintf(stderr, "[server] listen on %u\n", port);

    while (!g_stop) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int cfd = accept(lfd, (struct sockaddr*)&ss, &slen);
        if (cfd < 0) continue;

        pid_t pid = fork();
        if (pid == 0) {
            // child
            close(lfd);
            shm_stats_t *child_stats = ipc_stats_init(0);
            if (child_stats) {
                ipc_stats_inc_conn(child_stats);
                run_session(cfd, child_stats);
            } else {
                close(cfd);
            }
            _exit(0);
        } else if (pid > 0) {
            close(cfd);
            // parent continues
        } else {
            close(cfd);
        }
    }

    close(lfd);
    fprintf(stderr, "[server] shutdown\n");
    return 0;
}