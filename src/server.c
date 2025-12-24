#include "common/net.h"
#include "common/proto.h"
#include "common/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <time.h>

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

/* ---------- Game helpers ---------- */

static void push_log(state_t *st, const char *fmt, ...) {
    uint8_t idx = st->log_head % LOG_LINES;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->logs[idx], sizeof(st->logs[idx]), fmt, ap);
    va_end(ap);

    // vsnprintf always null terminates if size > 0
    st->log_head = (uint8_t)((st->log_head + 1) % LOG_LINES);
}



static void apply_damage(int16_t *hp, int16_t *shield, int dmg) {
    if (dmg <= 0) return;

    if (*shield > 0) {
        int s = (int)(*shield);
        int used = (dmg < s) ? dmg : s;
        *shield = (int16_t)(*shield - used);
        dmg -= used;
    }
    if (dmg > 0) {
        *hp = (int16_t)(*hp - dmg);
        if (*hp < 0) *hp = 0;
    }
}

static void tick_poison(state_t *st) {
    if (st->p_poison > 0) {
        st->p_poison--;
        st->p_hp = (int16_t)(st->p_hp - 2);
        if (st->p_hp < 0) st->p_hp = 0;
        push_log(st, "P takes poison (-2)");
    }
    if (st->ai_poison > 0) {
        st->ai_poison--;
        st->ai_hp = (int16_t)(st->ai_hp - 2);
        if (st->ai_hp < 0) st->ai_hp = 0;
        push_log(st, "AI takes poison (-2)");
    }
}

static void check_game_over(state_t *st) {
    if (st->game_over) return;
    if (st->p_hp <= 0 || st->ai_hp <= 0) {
        st->game_over = 1;
        if (st->p_hp > st->ai_hp) st->winner = 1;
        else if (st->ai_hp > st->p_hp) st->winner = 2;
        else st->winner = 0;
        push_log(st, "GAME OVER");
    }
}

static card_t rand_card(void) {
    // Card pool (id, type, cost, value)
    // value meaning:
    //  ATK: dmg, HEAL: heal, SHIELD: shield, BUFF: next atk +X, POISON: poison turns to add
    static const card_t pool[] = {
        {100, CARD_ATTACK, 1, 3, 0},
        {101, CARD_ATTACK, 2, 5, 0},
        {102, CARD_ATTACK, 3, 8, 0},

        {200, CARD_HEAL,   2, 4, 0},
        {201, CARD_HEAL,   3, 7, 0},

        {300, CARD_SHIELD, 1, 3, 0},
        {301, CARD_SHIELD, 2, 6, 0},

        {400, CARD_BUFF,   1, 2, 0},
        {401, CARD_BUFF,   2, 4, 0},

        {500, CARD_POISON, 2, 2, 0}, // add 2 turns poison
        {501, CARD_POISON, 3, 3, 0}, // add 3 turns poison
    };
    int n = (int)(sizeof(pool) / sizeof(pool[0]));
    return pool[rand() % n];
}

static void deal_hand(hand_t *h) {
    memset(h, 0, sizeof(*h));
    h->n = 3;
    for (int i = 0; i < 3; i++) h->cards[i] = rand_card();
}

static int err_send(int fd, int32_t code, const char *msg) {
    error_t e;
    memset(&e, 0, sizeof(e));
    e.code = code;
    if (msg) {
        strncpy(e.msg, msg, sizeof(e.msg) - 1);
        e.msg[sizeof(e.msg) - 1] = '\0';
    }
    return proto_send(fd, OP_ERROR, &e, sizeof(e));
}

static int handle_play_card(state_t *st, hand_t *hand, int is_player, uint8_t idx) {
    if (idx >= hand->n) return -1;

    card_t c = hand->cards[idx];

    // MVP: st->mana is the mana of current turn owner.
    if (c.cost > st->mana) return -2;
    st->mana = (uint8_t)(st->mana - c.cost);

    int16_t *self_hp     = is_player ? &st->p_hp     : &st->ai_hp;
    int16_t *enemy_hp    = is_player ? &st->ai_hp    : &st->p_hp;
    int16_t *self_shield = is_player ? &st->p_shield : &st->ai_shield;
    int16_t *enemy_shield= is_player ? &st->ai_shield: &st->p_shield;
    int16_t *self_buff   = is_player ? &st->p_buff   : &st->ai_buff;
    uint8_t *enemy_poison= is_player ? &st->ai_poison : &st->p_poison;

    switch (c.type) {
        case CARD_ATTACK: {
            int dmg = (int)c.value + (int)(*self_buff);
            *self_buff = 0; // consume buff
            apply_damage(enemy_hp, enemy_shield, dmg);
            push_log(st, "%s %s (%d) [mana %u]", is_player ? "P" : "AI",
                     "ATK", dmg, st->mana);
        } break;
        case CARD_HEAL: {
            *self_hp = (int16_t)(*self_hp + c.value);
            push_log(st, "%s HEAL (+%d) [mana %u]", is_player ? "P" : "AI",
                     (int)c.value, st->mana);
        } break;
        case CARD_SHIELD: {
            *self_shield = (int16_t)(*self_shield + c.value);
            push_log(st, "%s SHIELD (+%d) [mana %u]", is_player ? "P" : "AI",
                     (int)c.value, st->mana);
        } break;
        case CARD_BUFF: {
            *self_buff = (int16_t)(*self_buff + c.value);
            push_log(st, "%s BUFF (+%d next) [mana %u]", is_player ? "P" : "AI",
                     (int)c.value, st->mana);
        } break;
        case CARD_POISON: {
            // add turns to enemy poison
            *enemy_poison = (uint8_t)(*enemy_poison + (uint8_t)c.value);
            push_log(st, "%s POISON (+%d turns) [mana %u]", is_player ? "P" : "AI",
                     (int)c.value, st->mana);
        } break;
        default:
            return -3;
    }

    check_game_over(st);
    return 0;
}

// super simple AI: play first affordable card, else end turn
static void ai_take_turn(state_t *st, hand_t *hand) {
    st->turn = 1;
    st->mana = st->max_mana;

    // pick first card that cost <= mana
    int picked = -1;
    for (int i = 0; i < hand->n; i++) {
        if (hand->cards[i].cost <= st->mana) { picked = i; break; }
    }
    if (picked >= 0 && !st->game_over) {
        (void)handle_play_card(st, hand, 0, (uint8_t)picked);
    } else {
        push_log(st, "AI ends turn");
    }

    // end AI turn: poison tick + switch back
    tick_poison(st);
    check_game_over(st);

    st->turn = 0;
    st->mana = st->max_mana;
    deal_hand(hand);
    push_log(st, "P draw 3 cards");
}

static void run_session(int cfd, shm_stats_t *stats) {
    srand((unsigned)(time(NULL) ^ getpid()));

    state_t st;
    memset(&st, 0, sizeof(st));
    st.p_hp = 30;
    st.ai_hp = 30;
    st.turn = 0;
    st.game_over = 0;
    st.winner = 0;

    st.max_mana = 3;
    st.mana = st.max_mana;

    // statuses init
    st.p_shield = 0; st.ai_shield = 0;
    st.p_buff = 0;   st.ai_buff = 0;
    st.p_poison = 0; st.ai_poison = 0;
    st.log_head = 0;
    for (int i = 0; i < LOG_LINES; i++) st.logs[i][0] = '\0';

    push_log(&st, "WELCOME");
    push_log(&st, "P draw 3 cards");

    hand_t hand;
    deal_hand(&hand);

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
            if (proto_send(cfd, OP_HAND, &hand, sizeof(hand)) != 0) break;
            continue;
        }

        if (op == OP_PLAY_CARD) {
            if (plen != sizeof(play_req_t)) {
                (void)err_send(cfd, -10, "bad payload");
                continue;
            }
            if (st.turn != 0) {
                (void)err_send(cfd, -11, "not your turn");
                continue;
            }

            play_req_t pr;
            memcpy(&pr, payload, sizeof(pr));

            int rc = handle_play_card(&st, &hand, 1, pr.hand_idx);
            if (rc == -1) {
                (void)err_send(cfd, -1, "invalid hand idx");
            } else if (rc == -2) {
                (void)err_send(cfd, -2, "not enough mana");
            } else if (rc != 0) {
                (void)err_send(cfd, -3, "invalid card");
            }

            if (proto_send(cfd, OP_STATE, &st, sizeof(st)) != 0) break;
            if (proto_send(cfd, OP_HAND, &hand, sizeof(hand)) != 0) break;
            continue;
        }

        if (op == OP_END_TURN) {
            if (st.turn != 0) {
                (void)err_send(cfd, -11, "not your turn");
                continue;
            }

            push_log(&st, "P ends turn");

            // end player turn: poison tick
            tick_poison(&st);
            check_game_over(&st);

            if (!st.game_over) {
                // AI plays, then gives player new hand
                ai_take_turn(&st, &hand);
            }

            if (proto_send(cfd, OP_STATE, &st, sizeof(st)) != 0) break;
            if (proto_send(cfd, OP_HAND, &hand, sizeof(hand)) != 0) break;
            continue;
        }

        (void)err_send(cfd, -99, "unknown opcode");
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
