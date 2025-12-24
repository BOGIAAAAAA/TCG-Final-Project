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
#include <fcntl.h>   // For shm constants
#include <sys/mman.h> // For shm_unlink

/* --- Logging Helper --- */
static void log_info(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    
    fprintf(stderr, "[%s] [INFO] ", buf); // 自動帶上時間與等級
    
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

// FSM Forward Declarations
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
        push_log(st, "P: DRAW PHASE (3 cards)");
    } else { // AI
        deal_hand(hand); // Assume AI also "draws" (gets a fresh hand for simplicity logic)
        push_log(st, "AI: DRAW PHASE");
    }
    
    // Auto transition to MAIN
    st->phase = PHASE_MAIN;
    // Note: In a real async game, we might wait here. But for now, just push to MAIN.
    // Log transition
    // push_log(st, "%s: PHASE MAIN", st->turn == 0 ? "P" : "AI");
}

static void phase_end(state_t *st, hand_t *hand) {
    st->phase = PHASE_END; // visually show we are in END
    
    // Poison / Status ticks
    push_log(st, "%s: END PHASE", st->turn == 0 ? "P" : "AI");
    
    tick_poison(st);
    check_game_over(st);

    if (st->game_over) return;

    // Switch side
    int next_side = (st->turn == 0) ? 1 : 0;
    enter_turn(st, hand, next_side);
}

static int ai_eval_card(const state_t *st, const card_t *c) {
    int score = 0;

    // Survival priority (HP < 10)
    if (st->ai_hp < 10) {
        if (c->type == CARD_HEAL) score += 100;
    }

    // Counter-play: Break player shield
    if (st->p_shield > 0) {
        if (c->type == CARD_BUFF) score += 40;
    }

    switch (c->type) {
        case CARD_ATTACK:
            score += c->value;
            break;
        case CARD_POISON:
            // Early poison is better
            if (st->p_poison == 0) score += 30;
            break;
        case CARD_SHIELD:
            if (st->ai_shield == 0) score += 20;
            break;
        default: break;
    }

    // Efficiency: prefer cheaper cards for similar utility
    score -= (c->cost * 2);

    return score;
}

// AI Turn Execution (called when it becomes AI's turn during FSM flow)
// In this sync model, when we switch to AI, we execute its entire turn then switch back to Player
static void process_ai_turn(state_t *st, hand_t *hand) {
    // AI is in MAIN phase now (set by enter_turn -> phase_draw)
    
    // AI Loop: play cards while it can
    while (st->phase == PHASE_MAIN && !st->game_over) {
        int best_idx = -1;
        int best_score = -9999;

        for (int i = 0; i < hand->n; i++) {
            // Check if card is playable (cost <= mana) and not already used (cost != 255)
            if (hand->cards[i].cost <= st->mana) {
                int score = ai_eval_card(st, &hand->cards[i]);
                if (score > best_score) {
                    best_score = score;
                    best_idx = i;
                }
            }
        }
        
        if (best_idx >= 0) {
            // Play card
            int rc = handle_play_card(st, hand, 0 /* is_player=0 */, (uint8_t)best_idx);
            if (rc != 0) {
                // Should not happen if logic is correct, but break to avoid loop
                break;
            }
            // Mark as used
             hand->cards[best_idx].cost = 255; 
        } else {
            // No more playable cards
            break; 
        }
    }
    
    if (!st->game_over) {
         phase_end(st, hand);
    }
}

static void run_session(int cfd, shm_stats_t *stats, shm_store_t *store) {
    srand((unsigned)(time(NULL) ^ getpid()));
    
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
       if (proto_recv(cfd, &op, payload, sizeof(payload), &plen) != 0) return;

       if (op == OP_PING) {
            proto_send(cfd, OP_PONG, NULL, 0);
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
               err_send(cfd, -999, "server full");
               return; 
           }
           ipc_save_session(store, my_sid, &st, &hand);

           login_resp_t resp = { .ok = 1 };
           proto_send(cfd, OP_LOGIN_RESP, &resp, sizeof(resp));
           
           // Send Resume info (Session ID) - ACTUALLY Login Resp doesn't have SID in MVP v1.
           // Teacher requirements: "client adds ... Resume Req". 
           // We need to tell client the Session ID. 
           // HACK: Send RESUME_RESP right after LOGIN_RESP? Or piggyback?
           // The protocol definition for LOGIN_RESP is just `int32_t ok`.
           // Let's send a RESUME_RESP packet immediately after Login to give the ID.
           // Or change LOGIN_RESP struct (breaking change?). 
           // Protocol v2. Let's send RESUME_RESP as an "Info" packet.
           // Or better: The client expects OP_STATE / OP_HAND.
           // Let's send OP_RESUME_RESP with ok=1 and sid.
           resume_resp_t rr = { .ok = 1, .session_id = my_sid };
           proto_send(cfd, OP_RESUME_RESP, &rr, sizeof(rr));
           
           proto_send(cfd, OP_STATE, &st, sizeof(st));
           proto_send(cfd, OP_HAND, &hand, sizeof(hand));
           break;
       }
       else if (op == OP_RESUME_REQ) {
           if (plen < sizeof(resume_req_t)) { close(cfd); return; }
           resume_req_t *rr = (resume_req_t*)payload;
           if (ipc_load_session(store, rr->session_id, &st, &hand) == 0) {
               // Found
               my_sid = rr->session_id;
               resume_resp_t rresp = { .ok = 1, .session_id = my_sid };
               proto_send(cfd, OP_RESUME_RESP, &rresp, sizeof(rresp));
               proto_send(cfd, OP_STATE, &st, sizeof(st));
               proto_send(cfd, OP_HAND, &hand, sizeof(hand));
               
               push_log(&st, "Player Resumed Session");
               break;
           } else {
               // Not found
               resume_resp_t rresp = { .ok = 0, .session_id = 0 };
               proto_send(cfd, OP_RESUME_RESP, &rresp, sizeof(rresp));
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

        if (proto_recv(cfd, &op, payload, sizeof(payload), &plen) != 0) break;

        ipc_stats_inc_pkt(stats);
        // implicit heartbeat on any packet
        ipc_touch_session(store, my_sid);

        if (op == OP_PING) {
            proto_send(cfd, OP_PONG, NULL, 0);
            continue;
        }

        if (st.game_over) {
            proto_send(cfd, OP_STATE, &st, sizeof(st));
            proto_send(cfd, OP_HAND, &hand, sizeof(hand));
            continue;
        }

        if (op == OP_PLAY_CARD) {
            if (st.turn != 0) { err_send(cfd, -11, "not your turn"); continue; }
            if (st.phase != PHASE_MAIN) { err_send(cfd, -12, "phase error"); continue; }
            if (plen != sizeof(play_req_t)) { err_send(cfd, -10, "bad payload"); continue; }

            play_req_t pr;
            memcpy(&pr, payload, sizeof(pr));

            int rc = handle_play_card(&st, &hand, 1, pr.hand_idx);
            if (rc == 0) {
                // ok
            } else {
                if (rc == -1) err_send(cfd, -1, "invalid hand idx");
                else if (rc == -2) err_send(cfd, -2, "not enough mana");
                else err_send(cfd, -3, "invalid card");
            }
            
            ipc_save_session(store, my_sid, &st, &hand); // Sync to SHM

            proto_send(cfd, OP_STATE, &st, sizeof(st));
            proto_send(cfd, OP_HAND, &hand, sizeof(hand));
            continue;
        }

        if (op == OP_END_TURN) {
            if (st.turn != 0) { err_send(cfd, -11, "not your turn"); continue; }

            phase_end(&st, &hand); // Switch to AI
            
            ipc_save_session(store, my_sid, &st, &hand);

            if (st.turn == 1 && !st.game_over) {
                 process_ai_turn(&st, &hand);
                 ipc_save_session(store, my_sid, &st, &hand);
            }

            proto_send(cfd, OP_STATE, &st, sizeof(st));
            proto_send(cfd, OP_HAND, &hand, sizeof(hand));
            continue;
        }

        // Ignore duplicates LOGIN/RESUME in loop
        if (op == OP_LOGIN_REQ || op == OP_RESUME_REQ) continue;

        err_send(cfd, -99, "unknown opcode");
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
            // Re-open/map IPC in child (optional, as fork inherits maps, but clean habits good)
            // Note: fork inherits mmap, so we can just use `stats` and `store`.
            
            if (stats && store) {
                ipc_stats_inc_conn(stats);
                run_session(cfd, stats, store);
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
    log_info("[server] shutdown initiated\n");

    // --- Cleanup Shared Memory ---
    
    // Unlink stats shared memory (PROTO_MAGIC_SHM defined in proto.h)
    shm_unlink(PROTO_MAGIC_SHM); 

    // Unlink session store shared memory (STORE_MAGIC_SHM defined in ipc.h)
    shm_unlink(STORE_MAGIC_SHM);
    
    log_info("Shared memory unlinked\n");

    //fprintf(stderr, "[server] shutdown\n");
    return 0;
}
