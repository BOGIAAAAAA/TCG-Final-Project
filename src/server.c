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
#include <fcntl.h>
#include <sys/mman.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* --- Logging Helper --- */
static void log_info(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    
    fprintf(stderr, "[%s] [INFO] ", buf); // TIMESTAMP
    
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

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

#include "common/cards.h"

static uint16_t rand_card_id(void) {
    // Pool of new IDs
    static const uint16_t pool[] = {
        100, 101, 102, // ATK
        200, 201,      // HEAL
        300, 301,      // SHIELD
        400, 401,      // BUFF
        500, 501       // POISON
    };
    int n = sizeof(pool)/sizeof(pool[0]);
    return pool[rand() % n];
}

static void deal_hand(hand_t *h) {
    memset(h, 0, sizeof(*h));
    h->n = 3;
    for (int i = 0; i < 3; i++) h->card_ids[i] = rand_card_id();
}

static int err_send(connection_t *c, int32_t code, const char *msg) { // Fixed signature in prev step
    error_t e;
    memset(&e, 0, sizeof(e));
    e.code = code;
    if (msg) {
        strncpy(e.msg, msg, sizeof(e.msg) - 1);
        e.msg[sizeof(e.msg) - 1] = '\0';
    }
    return proto_send(c, OP_ERROR, &e, sizeof(e));
}


static int handle_play_card(state_t *st, hand_t *hand, int is_player, uint8_t idx) {
    if (idx >= hand->n) return -1;
    
    uint16_t cid = hand->card_ids[idx];
    if (cid == 0) return -3;

    const card_def_t *c = get_card_def(cid);
    if (!c) return -3;

    if (c->cost > st->mana) return -2;
    st->mana = (uint8_t)(st->mana - c->cost);

    int16_t *self_hp     = is_player ? &st->p_hp     : &st->ai_hp;
    int16_t *enemy_hp    = is_player ? &st->ai_hp    : &st->p_hp;
    int16_t *self_shield = is_player ? &st->p_shield : &st->ai_shield;
    int16_t *enemy_shield= is_player ? &st->ai_shield: &st->p_shield;
    int16_t *self_buff   = is_player ? &st->p_buff   : &st->ai_buff;
    uint8_t *enemy_poison= is_player ? &st->ai_poison : &st->p_poison;

    switch (c->type) {
        case CT_ATK: {
            int dmg = (int)c->value + (int)(*self_buff);
            *self_buff = 0; // consume buff
            apply_damage(enemy_hp, enemy_shield, dmg);
            push_log(st, "%s %s (%d) [mana %u]", is_player ? "P" : "AI",
                     "ATK", dmg, st->mana);
        } break;
        case CT_HEAL: {
            *self_hp = (int16_t)(*self_hp + c->value);
            push_log(st, "%s HEAL (+%d) [mana %u]", is_player ? "P" : "AI",
                     (int)c->value, st->mana);
        } break;
        case CT_SHIELD: {
            *self_shield = (int16_t)(*self_shield + c->value);
            push_log(st, "%s SHIELD (+%d) [mana %u]", is_player ? "P" : "AI",
                     (int)c->value, st->mana);
        } break;
        case CT_BUFF: {
            // User logic: BUFF adds to NEXT attack.
            *self_buff = (int16_t)(*self_buff + c->value);
            push_log(st, "%s BUFF (+%d next) [mana %u]", is_player ? "P" : "AI",
                     (int)c->value, st->mana);
        } break;
        case CT_POISON: {
            // User logic: POISON adds TURNS. (Value = turns)
            *enemy_poison = (uint8_t)(*enemy_poison + (uint8_t)c->value);
            push_log(st, "%s POISON (+%d turns) [mana %u]", is_player ? "P" : "AI",
                     (int)c->value, st->mana);
        } break;
        default:
            return -3;
    }

    check_game_over(st);
    return 0;
}

// FSM and AI (Same as before but ensures correct logic usage)
static void enter_turn(state_t *st, hand_t *hand, int side);
static void phase_draw(state_t *st, hand_t *hand);
static void phase_end(state_t *st, hand_t *hand);

static void enter_turn(state_t *st, hand_t *hand, int side) {
    st->turn = (uint8_t)side;
    st->phase = PHASE_DRAW;
    phase_draw(st, hand);
}

static void phase_draw(state_t *st, hand_t *hand) {
    st->mana = st->max_mana;
    if (st->turn == 0) { // Player
        deal_hand(hand);
        push_log(st, "P: DRAW PHASE");
    } else { // AI
        deal_hand(hand);
        push_log(st, "AI: DRAW PHASE");
    }
    st->phase = PHASE_MAIN;
}

static void phase_end(state_t *st, hand_t *hand) {
    st->phase = PHASE_END;
    push_log(st, "%s: END PHASE", st->turn == 0 ? "P" : "AI");
    tick_poison(st);
    check_game_over(st);
    if (st->game_over) return;
    int next_side = (st->turn == 0) ? 1 : 0;
    enter_turn(st, hand, next_side);
}

static int ai_eval_card(const state_t *st, const card_def_t *c) {
    int score = 0;
    if (st->ai_hp < 10 && c->type == CT_HEAL) score += 100;
    if (st->p_shield > 0 && c->type == CT_BUFF) score += 40; // Break shield setup
    
    switch (c->type) {
        case CT_ATK: score += c->value; break;
        case CT_POISON: if (st->p_poison == 0) score += 30; break;
        case CT_SHIELD: if (st->ai_shield == 0) score += 20; break;
        default: break;
    }
    score -= (c->cost * 2);
    return score;
}

static void process_ai_turn(state_t *st, hand_t *hand) {
    while (st->phase == PHASE_MAIN && !st->game_over) {
        int best_idx = -1;
        int best_score = -9999;

        for (int i = 0; i < hand->n; i++) {
            uint16_t cid = hand->card_ids[i];
            if (cid == 0) continue;
            const card_def_t *c = get_card_def(cid);
            if (!c) continue;
            if (c->cost <= st->mana) {
                int score = ai_eval_card(st, c);
                if (score > best_score) {
                    best_score = score;
                    best_idx = i;
                }
            }
        }
        
        if (best_idx >= 0) {
            handle_play_card(st, hand, 0, (uint8_t)best_idx);
            hand->card_ids[best_idx] = 0; 
        } else {
            break; 
        }
    }
    if (!st->game_over) phase_end(st, hand);
}

static void run_session(int cfd, SSL *ssl, shm_stats_t *stats, shm_store_t *store) {
    srand((unsigned)(time(NULL) ^ getpid()));
    
    connection_t conn;
    conn_init(&conn, cfd, ssl);

    // We need a session ID. 
    // Wait for LOGIN or RESUME.
    uint64_t my_sid = 0;

    state_t st;
    hand_t hand;
    memset(&st, 0, sizeof(st));
    memset(&hand, 0, sizeof(hand));
    
    uint8_t payload[1024];

    // Handshake Phase
    while (my_sid == 0) {
       uint16_t op = 0;
       uint32_t plen = 0;
       if (proto_recv(&conn, &op, payload, sizeof(payload), &plen) != 0) { conn_close(&conn); return; }

       if (op == OP_PING) {
            proto_send(&conn, OP_PONG, NULL, 0);
            continue;
       }

       if (op == OP_LOGIN_REQ) {
           // New session
           st.p_hp = 30; st.ai_hp = 30;
           st.max_mana = 3; 
           st.game_over = 0;
           enter_turn(&st, &hand, 0); // Player turn start -> Phase DRAW -> MAIN
           
           my_sid = ipc_alloc_session(store);
           if (my_sid == 0) {
               err_send(&conn, -999, "server full");
               conn_close(&conn);
               return; 
           }
           ipc_save_session(store, my_sid, &st, &hand);


           login_resp_t resp = { .ok = 1 };
           proto_send(&conn, OP_LOGIN_RESP, &resp, sizeof(resp));
           
           resume_resp_t rr = { .ok = 1, .session_id = my_sid };
           proto_send(&conn, OP_RESUME_RESP, &rr, sizeof(rr));
           
           proto_send(&conn, OP_STATE, &st, sizeof(st));
           proto_send(&conn, OP_HAND, &hand, sizeof(hand));
           break;
       }
       else if (op == OP_RESUME_REQ) {
           if (plen < sizeof(resume_req_t)) { conn_close(&conn); return; }
           resume_req_t *rr = (resume_req_t*)payload;
           if (ipc_load_session(store, rr->session_id, &st, &hand) == 0) {
               // Found
               my_sid = rr->session_id;
               resume_resp_t rresp = { .ok = 1, .session_id = my_sid };
               proto_send(&conn, OP_RESUME_RESP, &rresp, sizeof(rresp));
               proto_send(&conn, OP_STATE, &st, sizeof(st));
               proto_send(&conn, OP_HAND, &hand, sizeof(hand));
               
               push_log(&st, "Player Resumed Session");
               break;
           } else {
               // Not found
               resume_resp_t rresp = { .ok = 0, .session_id = 0 };
               proto_send(&conn, OP_RESUME_RESP, &rresp, sizeof(rresp));
               // Client should try Login
           }
       }
       else {
           // Ignore others during handshake?
       }
    }

    // Main Loop
    for (;;) {
        uint16_t op = 0;
        uint32_t plen = 0;
        
        if (st.turn == 1 && !st.game_over) {
             process_ai_turn(&st, &hand);
             // Save state after AI
             ipc_save_session(store, my_sid, &st, &hand);
        }

        if (proto_recv(&conn, &op, payload, sizeof(payload), &plen) != 0) break;

        ipc_stats_inc_pkt(stats);
        // implicit heartbeat on any packet
        ipc_touch_session(store, my_sid);

        if (op == OP_PING) {
            proto_send(&conn, OP_PONG, NULL, 0);
            continue;
        }

        if (st.game_over) {
            proto_send(&conn, OP_STATE, &st, sizeof(st));
            proto_send(&conn, OP_HAND, &hand, sizeof(hand));
            continue;
        }

        if (op == OP_PLAY_CARD) {
            if (st.turn != 0) { err_send(&conn, -11, "not your turn"); continue; }
            if (st.phase != PHASE_MAIN) { err_send(&conn, -12, "phase error"); continue; }
            if (plen != sizeof(play_req_t)) { err_send(&conn, -10, "bad payload"); continue; }

            play_req_t pr;
            memcpy(&pr, payload, sizeof(pr));

            int rc = handle_play_card(&st, &hand, 1, pr.hand_idx);
            if (rc == 0) {
                // ok
            } else {
                if (rc == -1) err_send(&conn, -1, "invalid hand idx");
                else if (rc == -2) err_send(&conn, -2, "not enough mana");
                else err_send(&conn, -3, "invalid card");
            }
            
            ipc_save_session(store, my_sid, &st, &hand); // Sync to SHM

            proto_send(&conn, OP_STATE, &st, sizeof(st));
            proto_send(&conn, OP_HAND, &hand, sizeof(hand));
            continue;
        }

        if (op == OP_END_TURN) {
            if (st.turn != 0) { err_send(&conn, -11, "not your turn"); continue; }

            phase_end(&st, &hand); // Switch to AI
            
            ipc_save_session(store, my_sid, &st, &hand);

            if (st.turn == 1 && !st.game_over) {
                 process_ai_turn(&st, &hand);
                 ipc_save_session(store, my_sid, &st, &hand);
            }

            proto_send(&conn, OP_STATE, &st, sizeof(st));
            proto_send(&conn, OP_HAND, &hand, sizeof(hand));
            continue;
        }

        // Ignore duplicates LOGIN/RESUME in loop
        if (op == OP_LOGIN_REQ || op == OP_RESUME_REQ) continue;

        err_send(&conn, -99, "unknown opcode");
    }

    conn_close(&conn);
}

int main(int argc, char **argv) {
    uint16_t port = 9000;
    if (argc >= 2) port = (uint16_t)atoi(argv[1]);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGCHLD, on_sigchld);

    ssl_msg_init();
    SSL_CTX *ctx = ssl_init_server_ctx("server.crt", "server.key");
    if (!ctx) {
        fprintf(stderr, "Failed to init SSL context. Check certs.\n");
        return 1;
    }

    shm_stats_t *stats = ipc_stats_init(1);
    if (!stats) {
        perror("ipc_stats_init");
        return 1;
    }
    
    shm_store_t *store = ipc_store_init(1);
    if (!store) {
        perror("ipc_store_init");
        return 1;
    }

    int lfd = tcp_listen(port);
    if (lfd < 0) {
        perror("tcp_listen");
        return 1;
    }
    log_info("[server] listen on %u (SSL)\n", port);

    while (!g_stop) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int cfd = accept(lfd, (struct sockaddr*)&ss, &slen);
        if (cfd < 0) continue;

        pid_t pid = fork();
        if (pid == 0) {
            // child
            close(lfd);
            
            // Set 5 seconds timeout for handshake/recv
            net_set_timeout(cfd, 5);

            // SSL Handshake in Child
            SSL *ssl = SSL_new(ctx);
            SSL_set_fd(ssl, cfd);
            if (SSL_accept(ssl) <= 0) {
                ERR_print_errors_fp(stderr);
                // Do not use conn_close here as we constructed partial object?
                // Or just use OpenSSL cleanup manually
                SSL_free(ssl);
                close(cfd);
                _exit(1);
            }

            if (stats && store) {
                ipc_stats_inc_conn(stats);
                run_session(cfd, ssl, stats, store);
            } else {
                SSL_shutdown(ssl);
                SSL_free(ssl);
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
    SSL_CTX_free(ctx);
    log_info("[server] shutdown initiated\n");

    // --- Cleanup Shared Memory ---
    shm_unlink(PROTO_MAGIC_SHM); 
    shm_unlink(STORE_MAGIC_SHM);
    
    log_info("Shared memory unlinked\n");
    return 0;
}
