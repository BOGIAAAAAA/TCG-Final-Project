#define _POSIX_C_SOURCE 200809L
#include "common/net.h"
#include "common/proto.h"
#include "common/cards.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>
#include <openssl/ssl.h>

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
        case CT_ATK:    return g_ui.tex_atk;
        case CT_HEAL:   return g_ui.tex_heal;
        case CT_SHIELD: return g_ui.tex_shield;
        case CT_BUFF:   return g_ui.tex_buff;
        case CT_POISON: return g_ui.tex_poison;
        default:        return g_ui.tex_atk;
    }
}

static void get_card_desc(const card_def_t *def, char *buf, size_t len) {
    if (!def) {
        snprintf(buf, len, "Unknown Effect");
        return;
    }
    switch (def->type) {
        case CT_ATK:
            snprintf(buf, len, "Deal %d damage.", def->value);
            break;
        case CT_HEAL:
            snprintf(buf, len, "Restore %d HP.", def->value);
            break;
        case CT_SHIELD:
            snprintf(buf, len, "Gain %d Block.", def->value);
            break;
        case CT_BUFF:
            snprintf(buf, len, "Next Attack +%d.", def->value);
            break;
        case CT_POISON:
            snprintf(buf, len, "Apply Poison (%d).", def->value);
            break;
        default:
            snprintf(buf, len, "Effect: %d", def->value);
            break;
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

// ---------- CMD PIPE ----------
typedef struct {
    uint16_t op;
    play_req_t play_payload; // only payload we send currently (besides empty)
} net_cmd_t;

typedef struct {
    const char *host;
    uint16_t port;
    int pipe_fd; // read end
    SSL_CTX *ctx;
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

        // SSL Handshake
        SSL *ssl = SSL_new(a->ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) <= 0) {
            printf("[Net] SSL Connect failed\n");
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            close(fd);
            sleep(2);
            continue;
        }
        
        connection_t conn;
        conn_init(&conn, fd, ssl);
        
        printf("[Net] Connected (SSL). Handshake...\n");

        // 2. Login or Resume
        if (g_session_id != 0) {
             printf("[Net] Trying Resume (SID=%lu)...\n", g_session_id);
             resume_req_t rr = { .session_id = g_session_id };
             if (proto_send(&conn, OP_RESUME_REQ, &rr, sizeof(rr)) != 0) {
                 conn_close(&conn); continue; 
             }
        } else {
             printf("[Net] Sending Login...\n");
             if (proto_send(&conn, OP_LOGIN_REQ, NULL, 0) != 0) {
                 conn_close(&conn); continue;
             }
        }

        // 3. Receive Loop (Select on FD and Pipe)
        state_t st;
        hand_t hand;
        uint8_t buf[4096];
        uint16_t op;
        uint32_t plen;
        
        time_t last_ping = time(NULL);

        for (;;) {
            time_t now = time(NULL);
            if (now - last_ping >= 2) {
                if (proto_send(&conn, OP_PING, NULL, 0) != 0) break;
                last_ping = now;
            }

            int pfd = a->pipe_fd;
            int maxfd = (fd > pfd) ? fd : pfd;
            fd_set rfd;
            FD_ZERO(&rfd);
            FD_SET(fd, &rfd);
            FD_SET(pfd, &rfd);
            
            struct timeval tv = {1, 0}; 
            int sret = select(maxfd + 1, &rfd, NULL, NULL, &tv);
            if (sret < 0) break; 
            if (sret == 0) continue; // timeout (check ping)
            
            // PIPE READ (Command from Main Thread)
            if (FD_ISSET(pfd, &rfd)) {
                net_cmd_t cmd;
                ssize_t n = read(pfd, &cmd, sizeof(cmd));
                if (n == sizeof(cmd)) {
                    // Execute Send
                    if (cmd.op == OP_PLAY_CARD) {
                        proto_send(&conn, OP_PLAY_CARD, &cmd.play_payload, sizeof(cmd.play_payload));
                    } else if (cmd.op == OP_END_TURN) {
                        proto_send(&conn, OP_END_TURN, NULL, 0);
                    }
                }
            }

            // SOCKET READ
            if (FD_ISSET(fd, &rfd)) {
                if (proto_recv(&conn, &op, buf, sizeof(buf), &plen) != 0) break;

                if (op == OP_PONG) continue;
                
                if (op == OP_RESUME_RESP) {
                    if (plen >= sizeof(resume_resp_t)) {
                        resume_resp_t *rr = (resume_resp_t*)buf;
                        if (rr->ok) {
                            g_session_id = rr->session_id;
                            printf("[Net] Session Active: %lu\n", g_session_id);
                        } else {
                            printf("[Net] Resume Failed. Retrying Login...\n");
                            g_session_id = 0; 
                            break; // Reconnect/Login
                        }
                    }
                    continue;
                }
                
                if (op == OP_LOGIN_RESP) continue;

                if (op == OP_STATE && plen == sizeof(state_t)) {
                    memcpy(&st, buf, sizeof(state_t));
                    pthread_mutex_lock(&g_mu);
                    state_t prev = g_sh.st; 
                    
                    g_sh.st = st;
                    g_sh.has_state = 1;
                    g_sh.connected = 1; 
                    g_sh.game_over = st.game_over;

                    // Float dmg calc
                    if (prev.max_mana != 0) { 
                        int dmg_to_ai = (int)prev.ai_hp - (int)st.ai_hp;
                        int dmg_to_p  = (int)prev.p_hp  - (int)st.p_hp;

                        if (dmg_to_ai > 0) {
                            g_float.active = 1;
                            g_float.t = 0.0f;
                            g_float.dur = 0.60f;
                            g_float.pos = (Vector2){ 690, 120 }; 
                            snprintf(g_float.text, sizeof(g_float.text), "-%d", dmg_to_ai);
                        } else if (dmg_to_p > 0) {
                            g_float.active = 1;
                            g_float.t = 0.0f;
                            g_float.dur = 0.60f;
                            g_float.pos = (Vector2){ 120, 120 }; 
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
            }
        }
        
        // Disconnected
        printf("[Net] Disconnected.\n");
        conn_close(&conn);
        
        pthread_mutex_lock(&g_mu);
        g_sh.connected = 0;
        pthread_mutex_unlock(&g_mu);
        
        sleep(1); 
    }
    return NULL;
}

static const char* winner_name(uint8_t w) {
    if (w == 1) return "PLAYER";
    if (w == 2) return "AI";
    return "NONE";
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

static void DrawPanel(int x, int y, int w, int h) {
    DrawRectangle(x + 4, y + 4, w, h, (Color){0, 0, 0, 80}); 
    DrawRectangle(x, y, w, h, (Color){30, 30, 30, 180});      
    DrawRectangleLinesEx((Rectangle){x, y, w, h}, 1, (Color){60, 60, 60, 150}); 
}

static Rectangle SrcCropToFit(Texture2D tex, float targetW, float targetH)
{
    float srcW = (float)tex.width;
    float srcH = (float)tex.height;
    if (srcW <= 0 || srcH <= 0) return (Rectangle){0,0,1,1};

    float srcAspect = srcW / srcH;
    float dstAspect = targetW / targetH;

    Rectangle src = {0};

    if (srcAspect > dstAspect) {
        float newW = srcH * dstAspect;
        src.width  = newW;
        src.height = srcH;
        src.x = (srcW - newW) * 0.5f;
        src.y = 0;
    } else {
        float newH = srcW / dstAspect;
        src.width  = srcW;
        src.height = newH;
        src.x = 0;
        src.y = 0; 
    }
    return src;
}

static void DrawCardVertical(Texture2D art, Rectangle cardRect,
                             const card_def_t *def,
                             bool disabled, bool hovered, bool selected)
{
    Color bg = (Color){20,20,20,240};
    if (disabled) bg = (Color){25,25,25,200};

    DrawRectangle(cardRect.x + 6, cardRect.y + 6, cardRect.width, cardRect.height, (Color){0,0,0,100});

    DrawRectangleRec(cardRect, bg);
    DrawRectangleLinesEx(cardRect, selected ? 3 : 2, hovered ? SKYBLUE : (Color){80,80,80,255});
    if (selected) DrawRectangleLinesEx(cardRect, 3, GOLD);

    Rectangle artDst = {
        cardRect.x + 8,
        cardRect.y + 8,
        cardRect.width - 16,
        cardRect.height * 0.55f
    };

    if (art.id != 0) {
        Rectangle src = SrcCropToFit(art, artDst.width, artDst.height);
        DrawTexturePro(art, src, artDst, (Vector2){0,0}, 0.0f, WHITE);
    }
    
    DrawRectangleLinesEx(artDst, 1, BLACK);

    if (def) {
        float tx = cardRect.x + 12;
        float ty = artDst.y + artDst.height + 12;

        DrawTextEx(g_ui.font, def->name, (Vector2){tx, ty}, 22, 1, RAYWHITE);

        char buf[64];
        snprintf(buf, sizeof(buf), "Val: %d", def->value);
        DrawTextEx(g_ui.font, buf, (Vector2){tx, ty + 28}, 18, 1, (Color){180,180,180,255});

        snprintf(buf, sizeof(buf), "Cost: %u", def->cost);
        DrawTextEx(g_ui.font, buf, (Vector2){tx, ty + 50}, 18, 1, YELLOW);
    }

    if (disabled) {
        DrawRectangleRec(cardRect, (Color){0,0,0,150});
    }
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    uint16_t port = 9000;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = (uint16_t)atoi(argv[2]);

    memset(&g_sh, 0, sizeof(g_sh));

    // Create Pipe
    int pfd[2];
    if (pipe(pfd) < 0) { perror("pipe"); return 1; }

    ssl_msg_init();
    SSL_CTX *ctx = ssl_init_client_ctx();
    if (!ctx) return 1;

    net_arg_t na = { .host = host, .port = port, .pipe_fd = pfd[0], .ctx = ctx };
    pthread_t nt;
    pthread_create(&nt, NULL, net_thread, &na);

    const int W = 900, H = 600;
    InitWindow(W, H, "Mini TCG - GUI Client (SSL Enabled)");
    if (!IsWindowReady()) {
        fprintf(stderr, "ERROR: GUI init failed\n");
        return 1;
    }
    g_ui.ok = 1;

    g_ui.font = LoadFontEx("src/assets/font/pixel.ttf", 28, NULL, 0);
    if (g_ui.font.texture.id == 0) {
        g_ui.font = GetFontDefault();
    } else {
        SetTextureFilter(g_ui.font.texture, TEXTURE_FILTER_POINT);
    }

    g_ui.tex_atk    = LoadTexture("src/assets/cards/atk.png");
    g_ui.tex_heal   = LoadTexture("src/assets/cards/heal.png");
    g_ui.tex_shield = LoadTexture("src/assets/cards/shield.png");
    g_ui.tex_buff   = LoadTexture("src/assets/cards/buff.png");
    g_ui.tex_poison = LoadTexture("src/assets/cards/poison.png");
    
    // Set filters
    SetTextureFilter(g_ui.tex_atk, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_heal, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_shield, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_buff, TEXTURE_FILTER_POINT);
    SetTextureFilter(g_ui.tex_poison, TEXTURE_FILTER_POINT);

    Rectangle cardRect[3] = {
        { 180,       360, 160, 220 },
        { 180 + 190, 360, 160, 220 },
        { 180 + 380, 360, 160, 220 }
    };
    
    Rectangle endBtn = { 900 - 140, 20, 120, 40 };
    int selected_idx = -1; 

    while (!WindowShouldClose()) {
        shared_t sh;
        pthread_mutex_lock(&g_mu);
        sh = g_sh;
        pthread_mutex_unlock(&g_mu);

        BeginDrawing();
        ClearBackground((Color){20, 20, 20, 255}); 

        DrawTextEx(g_ui.font, "Mini TCG (raylib SSL)", (Vector2){30, 20}, 32, 2, WHITE);

        if (!sh.connected && !sh.has_state) {
            DrawTextEx(g_ui.font, "Connecting (SSL)...", (Vector2){30, 80}, 24, 2, RED);
            EndDrawing();
            continue;
        }

        const int SZ_HUD   = 20;
        const int SZ_LOG   = 16;
        const int P_X = 40;     
        const int AI_X = 540;   
        const int HP_Y = 80;    
        const int BAR_W = 320;  
        
        char buf[256];
        if (sh.has_state) {
            DrawPanel(P_X - 10, HP_Y - 10, BAR_W + 20, 100); 
            DrawTextEx(g_ui.font, "PLAYER", (Vector2){P_X, HP_Y}, SZ_HUD, 1, WHITE);
            DrawHPBar(P_X, HP_Y + 25, BAR_W, 20, sh.st.p_hp, 30, sh.st.p_shield);
            snprintf(buf, sizeof(buf), "HP %d/%d  SHD %d  Mana %d/%d", 
                sh.st.p_hp, 30, sh.st.p_shield, sh.st.mana, sh.st.max_mana);
            DrawTextEx(g_ui.font, buf, (Vector2){P_X, HP_Y + 50}, SZ_HUD, 1, GREEN);
            if (sh.st.p_buff > 0 || sh.st.p_poison > 0) {
                snprintf(buf, sizeof(buf), "Buff %d  Psn %u", sh.st.p_buff, (unsigned)sh.st.p_poison);
                DrawTextEx(g_ui.font, buf, (Vector2){P_X, HP_Y + 74}, SZ_LOG, 1, LIGHTGRAY);
            }

            DrawPanel(AI_X - 10, HP_Y - 10, BAR_W + 20, 100); 
            DrawTextEx(g_ui.font, "OPPONENT", (Vector2){AI_X, HP_Y}, SZ_HUD, 1, WHITE);
            DrawHPBar(AI_X, HP_Y + 25, BAR_W, 20, sh.st.ai_hp, 30, sh.st.ai_shield);
            snprintf(buf, sizeof(buf), "HP %d/%d  SHD %d  PSN %u", 
                sh.st.ai_hp, 30, sh.st.ai_shield, (unsigned)sh.st.ai_poison);
            DrawTextEx(g_ui.font, buf, (Vector2){AI_X, HP_Y + 50}, SZ_HUD, 1, RED);
            if (sh.st.ai_buff > 0) {
                snprintf(buf, sizeof(buf), "Buff %d", sh.st.ai_buff);
                DrawTextEx(g_ui.font, buf, (Vector2){AI_X, HP_Y + 74}, SZ_LOG, 1, LIGHTGRAY);
            }

            DrawPanel(AI_X - 10, 210, 350, 150);
            DrawTextEx(g_ui.font, "Battle Log:", (Vector2){AI_X, 215}, SZ_LOG, 1, GRAY);
            int ly = 235;
            for (int i = 0; i < 6; i++) {
                const char *line = sh.st.logs[(sh.st.log_head + i) % 6];
                if (line[0] == '\0') line = "-";
                DrawTextEx(g_ui.font, line, (Vector2){AI_X, (float)ly}, SZ_LOG, 1, LIGHTGRAY);
                ly += 18;
            }

            Color btnColor = (sh.st.turn == 0) ? SKYBLUE : GRAY;
            DrawPanel(endBtn.x, endBtn.y, endBtn.width, endBtn.height); 
            DrawRectangleRec(endBtn, btnColor); 
            DrawRectangleLinesEx(endBtn, 2, WHITE);
            DrawTextEx(g_ui.font, "End Turn", (Vector2){endBtn.x + 15, endBtn.y + 10}, SZ_HUD, 1, BLACK);

            int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
            int hover_idx = -1;
            Vector2 mouse_p = GetMousePosition();

            if (sh.st.turn == 0 && !sh.st.game_over) {
                for (int i = 0; i < n; i++) {
                     if (CheckCollisionPointRec(mouse_p, cardRect[i])) {
                         hover_idx = i;
                         break;
                     }
                }
            }

            for (int i = 0; i < 3; i++) {
                if (i >= n) {
                     DrawPanel((int)cardRect[i].x, (int)cardRect[i].y, (int)cardRect[i].width, (int)cardRect[i].height);
                     continue;
                }
                if (i == selected_idx || i == hover_idx) continue;

                Rectangle r = cardRect[i];
                uint16_t cid = sh.hand.card_ids[i];
                const card_def_t *def = get_card_def(cid);
                Texture2D tex = (def) ? tex_for_card(def->type) : (Texture2D){0};

                bool disabled = (sh.st.turn != 0);
                DrawCardVertical(tex, r, def, disabled, false, false);
            }

            if (hover_idx >= 0 && hover_idx != selected_idx) {
                int i = hover_idx;
                Rectangle base = cardRect[i];
                float scale = 1.15f;
                float nw = base.width * scale;
                float nh = base.height * scale;
                float nx = base.x - (nw - base.width)*0.5f;
                float ny = base.y - 40; 
                Rectangle r = {nx, ny, nw, nh};
                
                uint16_t cid = sh.hand.card_ids[i];
                const card_def_t *def = get_card_def(cid);
                Texture2D tex = (def) ? tex_for_card(def->type) : (Texture2D){0};

                DrawCardVertical(tex, r, def, false, true, false);
            }

            if (selected_idx >= 0 && selected_idx < n) {
                int i = selected_idx;
                Rectangle base = cardRect[i];
                float scale = 1.30f;
                float nw = base.width * scale;
                float nh = base.height * scale;
                float nx = base.x - (nw - base.width)*0.5f;
                float ny = base.y - 80; 
                Rectangle r = {nx, ny, nw, nh};
                
                uint16_t cid = sh.hand.card_ids[i];
                const card_def_t *def = get_card_def(cid);
                Texture2D tex = (def) ? tex_for_card(def->type) : (Texture2D){0};

                DrawCardVertical(tex, r, def, false, false, true); 
                
                DrawRectangle(r.x, r.y - 40, r.width, 30, (Color){0,0,0,200});
                DrawTextEx(g_ui.font, "CLICK TO CONFIRM", (Vector2){r.x + 10, r.y - 35}, 16, 1, GREEN);

                if (def) {
                    int px = AI_X + 25; 
                    int py = 75;
                    Rectangle pRect = { px, py, 220, 300 }; 
                    
                    DrawRectangle(px+10, py+10, 220, 300, (Color){0,0,0,120});
                    
                    DrawRectangleRec(pRect, (Color){30, 30, 35, 255});
                    DrawRectangleLinesEx(pRect, 3, GOLD);
                    
                    Rectangle artDst = { px + 10, py + 10, 200, 160 };
                    Rectangle src = SrcCropToFit(tex, artDst.width, artDst.height);
                    DrawTexturePro(tex, src, artDst, (Vector2){0,0}, 0.0f, WHITE);
                    DrawRectangleLinesEx(artDst, 1, BLACK);

                    DrawTextEx(g_ui.font, def->name, (Vector2){px+15, py+180}, 24, 1, GOLD);
                    
                    char desc[64];
                    get_card_desc(def, desc, sizeof(desc));
                    DrawTextEx(g_ui.font, desc, (Vector2){px+15, py+215}, 18, 1, WHITE);
                    
                    snprintf(buf, sizeof(buf), "Cost: %u   Val: %d", def->cost, def->value);
                    DrawTextEx(g_ui.font, buf, (Vector2){px+15, py+260}, 18, 1, LIGHTGRAY);
                    
                    DrawTextEx(g_ui.font, "[PREVIEW]", (Vector2){px+15, py+282}, 14, 1, GRAY);
                }
            }
            
            if (sh.st.turn != 0 && !sh.st.game_over) {
                 const char *aitext = "AI TURN";
                 int w = MeasureText(aitext, 40);
                 DrawRectangle(0, H/2 - 40, W, 80, (Color){0, 0, 0, 200});
                 DrawTextEx(g_ui.font, aitext, (Vector2){(W-w)/2, H/2 - 20}, 40, 2, RED);
            }

            if (sh.st.game_over) {
                DrawRectangle(0, 0, W, H, (Color){0, 0, 0, 240}); 
                snprintf(buf, sizeof(buf), "GAME OVER - WINNER: %s", winner_name(sh.st.winner));
                DrawTextEx(g_ui.font, buf, (Vector2){250, 270}, 32, 2, GOLD);
            }
        }
        
        float dt = GetFrameTime();
        if (g_anim.active) {
            g_anim.t += dt / g_anim.dur;
            if (g_anim.t >= 1.0f) { g_anim.t = 1.0f; g_anim.active = 0; }
            g_anim.to = (Vector2){ AI_X + 50, HP_Y + 10 }; 
            Vector2 pos = v2_lerp(g_anim.from, g_anim.to, g_anim.t);
            DrawPanel((int)pos.x - 30, (int)pos.y - 20, 60, 40);
            DrawRectangle(pos.x - 28, pos.y - 18, 56, 36, (Color){250, 240, 200, 255});
            DrawTextEx(g_ui.font, g_anim.text, (Vector2){pos.x - 22, pos.y - 10}, 16, 1, BLACK);
        }

        Vector2 mp = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            sh.connected && sh.has_state && sh.has_hand && !sh.st.game_over && sh.st.turn == 0) {
            
            // CMD to Thread
            if (CheckCollisionPointRec(mp, endBtn)) {
                net_cmd_t cmd = { .op = OP_END_TURN };
                write(pfd[1], &cmd, sizeof(cmd));
                selected_idx = -1;
            } else {
                int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
                int clicked_card_idx = -1;
                for (int i = 0; i < n; i++) {
                    if (CheckCollisionPointRec(mp, cardRect[i])) {
                        clicked_card_idx = i;
                        break;
                    }
                }

                if (clicked_card_idx >= 0) {
                    if (clicked_card_idx == selected_idx) {
                        // Play Confirmed
                        
                        // Animation setup
                        uint16_t cid = sh.hand.card_ids[clicked_card_idx];
                        const card_def_t *def = get_card_def(cid);
                        g_anim.active = 1;
                        g_anim.t = 0.0f;
                        g_anim.dur = 0.50f;
                        g_anim.from = (Vector2){ cardRect[clicked_card_idx].x + 110, cardRect[clicked_card_idx].y + 70 };
                        g_anim.to   = (Vector2){ AI_X + 50, HP_Y + 10 }; 
                        snprintf(g_anim.text, sizeof(g_anim.text), "%s", def ? def->name : "PLAY");
                        
                        net_cmd_t cmd = { .op = OP_PLAY_CARD, .play_payload = { .hand_idx = (uint8_t)clicked_card_idx } };
                        write(pfd[1], &cmd, sizeof(cmd));
                        
                        selected_idx = -1;
                    } else {
                        selected_idx = clicked_card_idx;
                    }
                } else {
                    selected_idx = -1;
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
    
    // cleanup
    // leak stuff for now (thread detach, ctx, pipe)
    return 0;
}
