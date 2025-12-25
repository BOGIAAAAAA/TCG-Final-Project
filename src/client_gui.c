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
    DrawRectangle(x + 4, y + 4, w, h, (Color){0, 0, 0, 80}); // Softer Drop Shadow
    DrawRectangle(x, y, w, h, (Color){30, 30, 30, 180});      // Semi-transparent Body
    DrawRectangleLinesEx((Rectangle){x, y, w, h}, 1, (Color){60, 60, 60, 150}); // Thinner Frame
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

    // Spaced out cards (Gap 40px, Width 220)
    // Center block of 3 cards: Total W = 220*3 + 40*2 = 660 + 80 = 740.
    // Start X = (900 - 740)/2 = 80.
    // X Positions: 80, 80+220+40=340, 340+220+40=600.
    Rectangle cardRect[3] = {
        { 80,  420, 220, 140 },
        { 340, 420, 220, 140 },
        { 600, 420, 220, 140 }
    };
    
    // Top Right End Turn Button
    Rectangle endBtn = { 900 - 140, 20, 120, 40 };

    int selected_idx = -1; // Slay the Spire interaction state

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

        // Font Sizes
        const int SZ_TITLE = 28;
        const int SZ_HUD   = 20;
        const int SZ_LOG   = 16;
        const int SZ_CARD  = 18;

        // --- LAYOUT CONSTANTS ---
        const int P_X = 40;     // Player Left Align
        const int AI_X = 540;   // AI Left Align
        const int HP_Y = 80;    // HP Bar Y
        const int BAR_W = 320;  // HP Bar Width
        
        // status panel
        char buf[256];
        if (sh.has_state) {
            // --- LIGHTWEIGHT HUD ---
            
            // Player Stats
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

            // AI Stats
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

            // Battle Log (Low profile)
            DrawPanel(AI_X - 10, 210, 350, 150);
            DrawTextEx(g_ui.font, "Battle Log:", (Vector2){AI_X, 215}, SZ_LOG, 1, GRAY);
            int ly = 235;
            for (int i = 0; i < 6; i++) {
                const char *line = sh.st.logs[(sh.st.log_head + i) % 6];
                if (line[0] == '\0') line = "-";
                DrawTextEx(g_ui.font, line, (Vector2){AI_X, (float)ly}, SZ_LOG, 1, LIGHTGRAY);
                ly += 18;
            }

            // --- END TURN BUTTON ---
            Color btnColor = (sh.st.turn == 0) ? SKYBLUE : GRAY;
            DrawPanel(endBtn.x, endBtn.y, endBtn.width, endBtn.height); 
            DrawRectangleRec(endBtn, btnColor); 
            DrawRectangleLinesEx(endBtn, 2, WHITE);
            DrawTextEx(g_ui.font, "End Turn", (Vector2){endBtn.x + 15, endBtn.y + 10}, SZ_HUD, 1, BLACK);

            // --- HAND & FOCUS STATES ---
            DrawTextEx(g_ui.font, "Your Hand:", (Vector2){80, 390}, SZ_HUD, 1, LIGHTGRAY);

            // --- HAND & FOCUS STATES ---
            DrawTextEx(g_ui.font, "Your Hand:", (Vector2){80, 390}, SZ_HUD, 1, LIGHTGRAY);

            int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
            int hover_idx = -1;
            Vector2 mouse_p = GetMousePosition();

            // Detect Hover
            if (sh.st.turn == 0 && !sh.st.game_over) {
                for (int i = 0; i < n; i++) {
                     if (CheckCollisionPointRec(mouse_p, cardRect[i])) {
                         hover_idx = i;
                         break;
                     }
                }
            }

            // Pass 1: Draw Non-Active Cards (Unselected & Unhovered)
            for (int i = 0; i < 3; i++) {
                if (i >= n) {
                     DrawPanel((int)cardRect[i].x, (int)cardRect[i].y, (int)cardRect[i].width, (int)cardRect[i].height);
                     continue;
                }
                if (i == selected_idx || i == hover_idx) continue; // Skip active

                Rectangle r = cardRect[i];
                uint16_t cid = sh.hand.card_ids[i];
                const card_def_t *def = get_card_def(cid);
                
                // Dim if AI Turn
                Color dim = (sh.st.turn != 0) ? (Color){0,0,0,150} : (Color){0,0,0,0};

                DrawPanel((int)r.x, (int)r.y, (int)r.width, (int)r.height);
                if (def) {
                    Texture2D tex = tex_for_card(def->type);
                    DrawTexturePro(tex, (Rectangle){0,0,tex.width,tex.height}, 
                                  (Rectangle){r.x+2,r.y+2,r.width-4,r.height-4}, (Vector2){0,0}, 0, WHITE);
                    
                    DrawRectangle(r.x+2, r.y+2, r.width-4, 30, (Color){0,0,0,180}); 
                    snprintf(buf, sizeof(buf), "%s (%d)", def->name, def->value);
                    DrawTextEx(g_ui.font, buf, (Vector2){r.x+10, r.y+8}, SZ_CARD, 1, WHITE);
                    
                    DrawRectangle(r.x+2, r.y + r.height - 32, r.width-4, 30, (Color){0,0,0,180});
                    snprintf(buf, sizeof(buf), "Cost: %u", def->cost);
                    DrawTextEx(g_ui.font, buf, (Vector2){r.x+10, r.y+r.height-25}, SZ_CARD, 1, GOLD);
                }
                if (dim.a > 0) DrawRectangleRec(r, dim);
            }

            // Pass 2: Draw Hovered (if not selected)
            if (hover_idx >= 0 && hover_idx != selected_idx) {
                int i = hover_idx;
                Rectangle base = cardRect[i];
                float scale = 1.15f;
                float nw = base.width * scale;
                float nh = base.height * scale;
                float nx = base.x - (nw - base.width)*0.5f;
                float ny = base.y - 30; // Float Up
                Rectangle r = {nx, ny, nw, nh};
                
                uint16_t cid = sh.hand.card_ids[i];
                const card_def_t *def = get_card_def(cid);

                DrawRectangle(r.x+8, r.y+8, r.width, r.height, (Color){0,0,0,100}); // Shadow
                DrawRectangleRec(r, (Color){60, 60, 60, 255}); // Lighter Body
                DrawRectangleLinesEx(r, 2, WHITE);

                if (def) {
                    Texture2D tex = tex_for_card(def->type);
                    DrawTexturePro(tex, (Rectangle){0,0,tex.width,tex.height}, 
                                  (Rectangle){r.x+2,r.y+2,r.width-4,r.height-4}, (Vector2){0,0}, 0, WHITE);
                    
                    DrawRectangle(r.x+2, r.y+2, r.width-4, 32, (Color){0,0,0,200}); 
                    snprintf(buf, sizeof(buf), "%s (%d)", def->name, def->value);
                    DrawTextEx(g_ui.font, buf, (Vector2){r.x+12, r.y+8}, SZ_HUD, 1, WHITE);

                    DrawRectangle(r.x+2, r.y + r.height - 32, r.width-4, 32, (Color){0,0,0,200});
                    snprintf(buf, sizeof(buf), "Cost: %u", def->cost);
                    DrawTextEx(g_ui.font, buf, (Vector2){r.x+12, r.y+r.height-28}, SZ_HUD, 1, GOLD);
                }
            }

            // Pass 3: Draw Selected (Biggest, Highest Priority)
            if (selected_idx >= 0 && selected_idx < n) {
                int i = selected_idx;
                Rectangle base = cardRect[i];
                float scale = 1.30f;
                float nw = base.width * scale;
                float nh = base.height * scale;
                float nx = base.x - (nw - base.width)*0.5f;
                float ny = base.y - 80; // Float WAY Up
                Rectangle r = {nx, ny, nw, nh};
                
                uint16_t cid = sh.hand.card_ids[i];
                const card_def_t *def = get_card_def(cid);

                DrawRectangle(r.x+12, r.y+12, r.width, r.height, (Color){0,0,0,150}); // Deep Shadow
                DrawRectangleRec(r, (Color){70, 70, 80, 255}); // Blue-ish Body
                DrawRectangleLinesEx(r, 4, GOLD); // Gold Border

                if (def) {
                    Texture2D tex = tex_for_card(def->type);
                    DrawTexturePro(tex, (Rectangle){0,0,tex.width,tex.height}, 
                                  (Rectangle){r.x+2,r.y+2,r.width-4,r.height-4}, (Vector2){0,0}, 0, WHITE);
                    
                    DrawRectangle(r.x+2, r.y+2, r.width-4, 40, (Color){0,0,0,220}); 
                    snprintf(buf, sizeof(buf), "%s (%d)", def->name, def->value);
                    DrawTextEx(g_ui.font, buf, (Vector2){r.x+12, r.y+10}, SZ_TITLE, 1, WHITE);

                    // Confirm Prompt
                    DrawRectangle(r.x, r.y - 40, r.width, 30, (Color){0,0,0,200});
                    DrawTextEx(g_ui.font, "CLICK TO CONFIRM", (Vector2){r.x + 20, r.y - 35}, 20, 1, GREEN);

                    DrawRectangle(r.x+2, r.y + r.height - 40, r.width-4, 40, (Color){0,0,0,220});
                    snprintf(buf, sizeof(buf), "Cost: %u", def->cost);
                    DrawTextEx(g_ui.font, buf, (Vector2){r.x+12, r.y+r.height-32}, SZ_TITLE, 1, GOLD);
                    
                    // --- BIG PREVIEW PANEL (Right Side) ---
                    // Position: Above Battle Log (approx AI_X + 25, y=75)
                    int px = AI_X + 25; 
                    int py = 75;
                    Rectangle pRect = { px, py, 220, 300 }; // Big Card Size
                    
                    // Shadow
                    DrawRectangle(px+10, py+10, 220, 300, (Color){0,0,0,120});
                    // Body
                    DrawRectangleRec(pRect, (Color){40, 40, 45, 255});
                    DrawRectangleLinesEx(pRect, 3, LIGHTGRAY);
                    
                    // Large Art
                    DrawTexturePro(tex, (Rectangle){0,0,tex.width,tex.height}, 
                                  (Rectangle){px+10, py+10, 200, 150}, (Vector2){0,0}, 0, WHITE);
                    
                    // Name
                    DrawTextEx(g_ui.font, def->name, (Vector2){px+15, py+170}, 30, 1, GOLD);
                    
                    // Description
                    char desc[64];
                    get_card_desc(def, desc, sizeof(desc));
                    // Wrap text logic? For now assume it fits or simple multiline manually if needed.
                    // Description usually short (e.g. "Deal 6 damage.")
                    DrawTextEx(g_ui.font, desc, (Vector2){px+15, py+210}, 20, 1, WHITE);
                    
                    // Detailed Stats
                    snprintf(buf, sizeof(buf), "Cost: %u   Val: %d", def->cost, def->value);
                    DrawTextEx(g_ui.font, buf, (Vector2){px+15, py+250}, 20, 1, LIGHTGRAY);
                    
                    DrawTextEx(g_ui.font, "[PREVIEW]", (Vector2){px+15, py+280}, 16, 1, GRAY);
                }
            }
            
            // --- FOCUS OVERLAYS ---
            
            // AI TURN BANNER
            if (sh.st.turn != 0 && !sh.st.game_over) {
                 // Draw banner in center
                 const char *aitext = "AI TURN";
                 int w = MeasureText(aitext, 40);
                 DrawRectangle(0, H/2 - 40, W, 80, (Color){0, 0, 0, 200});
                 DrawTextEx(g_ui.font, aitext, (Vector2){(W-w)/2, H/2 - 20}, 40, 2, RED);
            }

            // GAME OVER - Full screen mask
            if (sh.st.game_over) {
                DrawRectangle(0, 0, W, H, (Color){0, 0, 0, 240}); // Full screen blackout
                snprintf(buf, sizeof(buf), "GAME OVER - WINNER: %s", winner_name(sh.st.winner));
                DrawTextEx(g_ui.font, buf, (Vector2){250, 270}, 32, 2, GOLD);
            }
        }
        
        // Animations
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

        // input handling
        Vector2 mp = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            sh.connected && sh.has_state && sh.has_hand && !sh.st.game_over && sh.st.turn == 0) {
            
            if (na.fd_out >= 0) {
                int clicked_card_idx = -1;
                
                // End turn check
                if (CheckCollisionPointRec(mp, endBtn)) {
                    proto_send(na.fd_out, OP_END_TURN, NULL, 0);
                    selected_idx = -1; // Reset selection on End Turn
                } else {
                    // Check cards
                    int n = sh.has_hand ? (sh.hand.n > 3 ? 3 : sh.hand.n) : 0;
                    for (int i = 0; i < n; i++) {
                        if (CheckCollisionPointRec(mp, cardRect[i])) {
                            clicked_card_idx = i;
                            break;
                        }
                    }

                    if (clicked_card_idx >= 0) {
                        // Clicked a card
                        if (clicked_card_idx == selected_idx) {
                            // CONFIRMATION - PLAY CARD
                            play_req_t pr;
                            pr.hand_idx = (uint8_t)clicked_card_idx;
                            
                            // Animation
                            uint16_t cid = sh.hand.card_ids[clicked_card_idx];
                            const card_def_t *def = get_card_def(cid);
                            
                            g_anim.active = 1;
                            g_anim.t = 0.0f;
                            g_anim.dur = 0.50f; // Slower for effect
                            g_anim.from = (Vector2){ cardRect[clicked_card_idx].x + 110, cardRect[clicked_card_idx].y + 70 };
                            g_anim.to   = (Vector2){ AI_X + 50, HP_Y + 10 }; 
                            
                            snprintf(g_anim.text, sizeof(g_anim.text), "%s", def ? def->name : "PLAY");

                            proto_send(na.fd_out, OP_PLAY_CARD, &pr, sizeof(pr));
                            
                            selected_idx = -1; // Deselect after playing
                        } else {
                            // SELECTION
                            selected_idx = clicked_card_idx;
                        }
                    } else {
                        // Clicked Empty Space -> Deselect
                        selected_idx = -1;
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
