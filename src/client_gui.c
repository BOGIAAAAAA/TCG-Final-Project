#define _POSIX_C_SOURCE 200809L
#include "common/net.h"
#include "common/proto.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>

#include "raylib.h"

// ---------- UI assets ----------
typedef struct {
    Font font;
    Texture2D tex_atk, tex_heal, tex_shield, tex_buff, tex_poison;
    int ok;
} ui_assets_t;

static ui_assets_t g_ui;

static Texture2D tex_for_card(uint8_t type) {
    switch (type) {
        case CARD_ATTACK: return g_ui.tex_atk;
        case CARD_HEAL:   return g_ui.tex_heal;
        case CARD_SHIELD: return g_ui.tex_shield;
        case CARD_BUFF:   return g_ui.tex_buff;
        case CARD_POISON: return g_ui.tex_poison;
        default:          return g_ui.tex_atk;
    }
}

// ---------- animation ----------
typedef struct {
    int active;
    float t;        // 0..1
    float dur;      // seconds
    Vector2 from;
    Vector2 to;
    char text[32];  // e.g. "-5"
} anim_t;

typedef struct {
    int active;
    float t;        // elapsed
    float dur;      // seconds
    Vector2 pos;
    char text[32];  // e.g. "-5"
} float_text_t;

static anim_t g_anim = {0};
static float_text_t g_float = {0};

static Vector2 v2_lerp(Vector2 a, Vector2 b, float t) {
    return (Vector2){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

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

static uint64_t g_session_id = 0; // stored session id

static void* net_thread(void *p) {
    net_arg_t *a = (net_arg_t*)p;
    
    while (1) {
        // 1. Connect
        printf("[Net] Connecting to %s:%u...\n", a->host, a->port);
        int fd = tcp_connect(a->host, a->port);
        if (fd < 0) {
            pthread_mutex_lock(&g_mu);
            g_sh.connected = 0;
            pthread_mutex_unlock(&g_mu);
            sleep(2); // retry delay
            continue;
        }
        
        a->fd_out = fd;
        printf("[Net] Connected. Handshake...\n");

        // 2. Login or Resume
        int logged_in = 0;
        
        if (g_session_id != 0) {
             printf("[Net] Trying Resume (SID=%lu)...\n", g_session_id);
             resume_req_t rr = { .session_id = g_session_id };
             if (proto_send(fd, OP_RESUME_REQ, &rr, sizeof(rr)) != 0) {
                 close(fd); continue; 
             }
        } else {
             printf("[Net] Sending Login...\n");
             if (proto_send(fd, OP_LOGIN_REQ, NULL, 0) != 0) {
                 close(fd); continue;
             }
        }

        // 3. Receive Loop
        state_t st;
        hand_t hand;
        uint8_t buf[4096];
        uint16_t op;
        uint32_t plen;
        
        // Setup PING timer
        time_t last_ping = time(NULL);

        // We need to wait for initial state to confirm "Connected"
        // But inside the loop works too.
        
        for (;;) {
            // Check heartbeat need (every 2s)
            time_t now = time(NULL);
            if (now - last_ping >= 2) {
                if (proto_send(fd, OP_PING, NULL, 0) != 0) break;
                last_ping = now;
            }

            // Blocking recv? No, we need non-blocking or select/poll to do heartbeat properly?
            // Current `proto_recv` uses `read` which blocks. 
            // If we block on read, we can't send PING.
            // For MVP, simplistic approach: "Send PING only if we are active loop"? 
            // No, that fails if idle.
            // We need `proto_recv` with timeout.
            // OR: We assume server sends PING? Request said "Client 2 sec sends PING".
            // Implementation detail: `proto_recv` blocks naturally.
            // FIX: Set socket timeout or use poll.
            // Let's implement a simple poll check.
            
            struct timeval tv = {1, 0}; // 1 sec timeout
            fd_set rfd;
            FD_ZERO(&rfd);
            FD_SET(fd, &rfd);
            
            int sret = select(fd + 1, &rfd, NULL, NULL, &tv);
            if (sret < 0) break; // error
            if (sret == 0) {
                // Timeout -> loop allow checking PING time
                continue;
            }
            
            // Ready to read
            if (proto_recv(fd, &op, buf, sizeof(buf), &plen) != 0) break;

            if (op == OP_PONG) {
                // ok
                continue;
            }
            
            if (op == OP_RESUME_RESP) {
                if (plen >= sizeof(resume_resp_t)) {
                    resume_resp_t *rr = (resume_resp_t*)buf;
                    if (rr->ok) {
                        g_session_id = rr->session_id;
                        printf("[Net] Session Active: %lu\n", g_session_id);
                        // Wait for State/Hand next
                    } else {
                        // Resume failed
                        printf("[Net] Resume Failed. Retrying Login...\n");
                        g_session_id = 0; 
                        // Break recv loop to re-connect/login clean? 
                        // Or just send Login now?
                        // Easiest is close and retry loop (which sees sid=0)
                        break;
                    }
                }
                continue;
            }
            
            if (op == OP_LOGIN_RESP) {
                // ok
                continue;
            }

            if (op == OP_STATE && plen == sizeof(state_t)) {
                memcpy(&st, buf, sizeof(state_t));
                pthread_mutex_lock(&g_mu);
                state_t prev = g_sh.st; // snapshot old
                
                g_sh.st = st;
                g_sh.has_state = 1;
                g_sh.connected = 1; 
                g_sh.game_over = st.game_over;

                // --- damage float trigger (AI HP drop => player dealt damage) ---
                // Only if we had state before (to avoid -30 on init)
                if (prev.max_mana != 0) { // simple check if prev was valid
                    int dmg_to_ai = (int)prev.ai_hp - (int)st.ai_hp;
                    int dmg_to_p  = (int)prev.p_hp  - (int)st.p_hp;

                    // 只要有變化，就先記一個「最新的浮字」（MVP：一次顯示一個）
                    if (dmg_to_ai > 0) {
                        g_float.active = 1;
                        g_float.t = 0.0f;
                        g_float.dur = 0.60f;
                        g_float.pos = (Vector2){ 690, 120 }; // AI HP 區域附近
                        snprintf(g_float.text, sizeof(g_float.text), "-%d", dmg_to_ai);
                    } else if (dmg_to_p > 0) {
                        g_float.active = 1;
                        g_float.t = 0.0f;
                        g_float.dur = 0.60f;
                        g_float.pos = (Vector2){ 120, 120 }; // Player HP 區域附近
                        snprintf(g_float.text, sizeof(g_float.text), "-%d", dmg_to_p);
                    }
                }

                pthread_mutex_unlock(&g_mu);
                continue;
            }
            
            if (op == OP_HAND && plen == sizeof(hand_t)) {
                memcpy(&hand, buf, sizeof(hand_t));
                pthread_mutex_lock(&g_mu);
                g_sh.hand = hand;
                g_sh.has_hand = 1;
                pthread_mutex_unlock(&g_mu);
                continue;
            }
            
            // Ignore others
        }
        
        // Disconnected
        printf("[Net] Disconnected.\n");
        close(fd);
        a->fd_out = -1;
        
        pthread_mutex_lock(&g_mu);
        g_sh.connected = 0;
        pthread_mutex_unlock(&g_mu);
        
        sleep(1); // Small delay before reconnect
    }
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

static const char* phase_name(uint8_t p) {
    switch (p) {
        case PHASE_DRAW: return "DRAW";
        case PHASE_MAIN: return "MAIN";
        case PHASE_END:  return "END";
        default: return "???";
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

static void DrawHPBar(int x, int y, int w, int h, int hp, int maxhp, int shield) {
    DrawRectangle(x, y, w, h, (Color){30,30,30,255});               // frame
    DrawRectangle(x+2, y+2, w-4, h-4, (Color){10,10,10,255});       // bg

    if (maxhp <= 0) maxhp = 1;
    float ratio = (float)hp / (float)maxhp;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int fillw = (int)((w-4) * ratio);
    DrawRectangle(x+2, y+2, fillw, h-4, (Color){80,220,120,255});   // hp

    // shield overlay (thin bar on top)
    if (shield > 0) {
        int sw = shield;
        if (sw > maxhp) sw = maxhp;
        float sratio = (float)sw / (float)maxhp;
        int shw = (int)((w-4) * sratio);
        DrawRectangle(x+2, y+2, shw, 4, (Color){120,180,255,255});
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
    // Load Assets
    g_ui.ok = 1;

    // Font: Pixel style (TTF)
    // Note: User requested "assets/...", but files are in "src/assets/...". 
    // Using src/assets/ to ensure it works from project root.
    g_ui.font = LoadFontEx("src/assets/font/pixel.ttf", 28, NULL, 0);
    if (g_ui.font.texture.id == 0) {
        TraceLog(LOG_WARNING, "Failed to load font src/assets/font/pixel.ttf, fallback default font.");
        g_ui.font = GetFontDefault();
    } else {
        SetTextureFilter(g_ui.font.texture, TEXTURE_FILTER_POINT);
    }

    // PNG Card Textures
    g_ui.tex_atk    = LoadTexture("src/assets/cards/atk.png");
    g_ui.tex_heal   = LoadTexture("src/assets/cards/heal.png");
    g_ui.tex_shield = LoadTexture("src/assets/cards/shield.png");
    g_ui.tex_buff   = LoadTexture("src/assets/cards/buff.png");
    g_ui.tex_poison = LoadTexture("src/assets/cards/poison.png");

    SetTextureFilter(g_ui.tex_atk, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_heal, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_shield, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_buff, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_poison, TEXTURE_FILTER_POINT);

    // Check for missing textures
    if (g_ui.tex_atk.id == 0 || g_ui.tex_heal.id == 0 || g_ui.tex_shield.id == 0 ||
        g_ui.tex_buff.id == 0 || g_ui.tex_poison.id == 0) {
        TraceLog(LOG_WARNING, "Some card textures missing. Check src/assets/cards/*.png");
    }

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
        ClearBackground((Color){20, 20, 20, 255}); // Dark theme

        DrawTextEx(g_ui.font, "Mini TCG (raylib GUI)", (Vector2){30, 20}, 32, 2, WHITE);

        if (!sh.connected && !sh.has_state) {
            DrawTextEx(g_ui.font, "Connecting...", (Vector2){30, 80}, 24, 2, RED);
            EndDrawing();
            continue;
        }

        // status panel
        char buf[256];
        if (sh.has_state) {
            // HP Bars
            // Player
            DrawHPBar(30, 90, 360, 20, sh.st.p_hp, 30, sh.st.p_shield);
            
            // AI
            DrawHPBar(520, 90, 360, 20, sh.st.ai_hp, 30, sh.st.ai_shield);

            // Text info
            snprintf(buf, sizeof(buf), "P HP %d  SHD %d  BUFF %d  PSN %u", 
                sh.st.p_hp, sh.st.p_shield, sh.st.p_buff, (unsigned)sh.st.p_poison);
            DrawTextEx(g_ui.font, buf, (Vector2){30, 60}, 20, 1, (Color){200,200,200,255});

            snprintf(buf, sizeof(buf), "AI HP %d  SHD %d  BUFF %d  PSN %u", 
                sh.st.ai_hp, sh.st.ai_shield, sh.st.ai_buff, (unsigned)sh.st.ai_poison);
            DrawTextEx(g_ui.font, buf, (Vector2){520, 60}, 20, 1, (Color){200,200,200,255});

            // Mana
            snprintf(buf, sizeof(buf), "Mana: %u / %u", (unsigned)sh.st.mana, (unsigned)sh.st.max_mana);
            DrawTextEx(g_ui.font, buf, (Vector2){30, 130}, 24, 1, SKYBLUE);


            // Turn & Phase
            const char *turn = (sh.st.turn == 0) ? "PLAYER" : "AI";
            Color tc = (sh.st.turn == 0) ? GREEN : RED;
            snprintf(buf, sizeof(buf), "[ %s ]  %s", turn, phase_name(sh.st.phase));
            DrawTextEx(g_ui.font, buf, (Vector2){30, 270}, 24, 2, tc);

            if (sh.st.game_over) {
                snprintf(buf, sizeof(buf), "WINNER: %s", winner_name(sh.st.winner));
                DrawTextEx(g_ui.font, buf, (Vector2){30, 310}, 32, 2, GOLD);
            }

            draw_logs(&sh.st, 520, 70);
        }

        // End Turn button
        Color btnColor = (sh.st.turn == 0) ? SKYBLUE : GRAY;
        DrawRectangleRec(endBtn, btnColor);
        DrawRectangleLines((int)endBtn.x, (int)endBtn.y, (int)endBtn.width, (int)endBtn.height, WHITE);
        DrawTextEx(g_ui.font, "End Turn", (Vector2){endBtn.x + 20, endBtn.y + 12}, 20, 1, BLACK);

        // Hand
        DrawTextEx(g_ui.font, "Hand:", (Vector2){30, 390}, 22, 1, LIGHTGRAY);

        int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
        for (int i = 0; i < 3; i++) {
            Rectangle r = cardRect[i];
            
            // Draw Card Background
            DrawRectangleRec(r, (i < n) ? RAYWHITE : DARKGRAY);
            DrawRectangleLinesEx(r, 2, GRAY);

            if (i < n) {
                const card_t *c = &sh.hand.cards[i];
                
                // Texture
                Texture2D tex = tex_for_card(c->type);
                DrawTexturePro(tex, (Rectangle){0,0,tex.width,tex.height}, 
                              (Rectangle){r.x+10, r.y+30, r.width-20, r.height-40}, 
                              (Vector2){0,0}, 0, WHITE);
                
                // Text overlay
                snprintf(buf, sizeof(buf), "%s (%d)", card_type_name(c->type), c->value);
                DrawTextEx(g_ui.font, buf, (Vector2){r.x+10, r.y+5}, 18, 1, BLACK);
                
                snprintf(buf, sizeof(buf), "Cost: %u", c->cost);
                DrawTextEx(g_ui.font, buf, (Vector2){r.x+10, r.y+r.height-25}, 18, 1, BLACK);
            }
        }
        
        // Floating Text Animation (if active)
        // (Placeholder logic: in real app, we'd trigger this on events)
        if (g_float.active) {
            g_float.t += GetFrameTime();
            if (g_float.t >= g_float.dur) g_float.active = 0;
            else {
                float alpha = 1.0f - (g_float.t / g_float.dur);
                Vector2 pos = g_float.pos;
                pos.y -= g_float.t * 30.0f; // float up
                Color col = RED;
                col.a = (unsigned char)(alpha * 255);
                DrawTextEx(g_ui.font, g_float.text, pos, 32, 2, col);
            }
        }

        // Fly Animation
        float dt = GetFrameTime();
        if (g_anim.active) {
            g_anim.t += dt / g_anim.dur;
            if (g_anim.t >= 1.0f) { g_anim.t = 1.0f; g_anim.active = 0; }
            Vector2 pos = v2_lerp(g_anim.from, g_anim.to, g_anim.t);

            // draw a tiny "card" sprite
            Rectangle r = { pos.x - 30, pos.y - 20, 60, 40 };
            DrawRectangleRec(r, (Color){250, 240, 200, 255});
            DrawRectangleLinesEx(r, 2, (Color){30,30,30,255});
            DrawTextEx(g_ui.font, g_anim.text, (Vector2){r.x + 8, r.y + 10}, 16, 1, (Color){30,30,30,255});
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
                            
                            // start anim (card flies to AI area)
                            g_anim.active = 1;
                            g_anim.t = 0.0f;
                            g_anim.dur = 0.30f;
                            g_anim.from = (Vector2){ cardRect[i].x + cardRect[i].width*0.5f, cardRect[i].y + cardRect[i].height*0.5f };
                            g_anim.to   = (Vector2){ 700, 110 }; // AI HP bar area
                            snprintf(g_anim.text, sizeof(g_anim.text), "PLAY");

                            proto_send(na.fd_out, OP_PLAY_CARD, &pr, sizeof(pr));
                            break;
                        }
                    }
                }
            }
        }

        EndDrawing();
    }

    UnloadTexture(g_ui.tex_atk);
    UnloadTexture(g_ui.tex_heal);
    UnloadTexture(g_ui.tex_shield);
    UnloadTexture(g_ui.tex_buff);
    UnloadTexture(g_ui.tex_poison);
    UnloadFont(g_ui.font);

    CloseWindow();
    pthread_detach(nt);
    return 0;
}
