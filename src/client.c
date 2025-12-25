#include "common/net.h"
#include "common/proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <openssl/ssl.h>

typedef struct {
    const char *host;
    uint16_t port;
    int rounds;
    long long *lat_ns_out; // store sum latency
    int idx;
    SSL_CTX *ctx; // shared ctx
} th_arg_t;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void* worker(void *p) {
    th_arg_t *a = (th_arg_t*)p;

    int fd = tcp_connect(a->host, a->port);
    if (fd < 0) {
        a->lat_ns_out[a->idx] = -1;
        return NULL;
    }
    
    // SSL Handshake
    SSL *ssl = SSL_new(a->ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) <= 0) {
        // Handshake failed
        SSL_free(ssl);
        close(fd);
        a->lat_ns_out[a->idx] = -1;
        return NULL;
    }
    
    connection_t conn;
    conn_init(&conn, fd, ssl);

    // login
    long long t0 = now_ns();
    if (proto_send(&conn, OP_LOGIN_REQ, NULL, 0) != 0) { conn_close(&conn); a->lat_ns_out[a->idx] = -1; return NULL; }

    uint8_t buf[1024];
    uint16_t op; uint32_t plen;
    if (proto_recv(&conn, &op, buf, sizeof(buf), &plen) != 0) { conn_close(&conn); a->lat_ns_out[a->idx] = -1; return NULL; }
    if (op == OP_LOGIN_RESP) {
        // Expected login resp first now? Ah, server sends LOGIN_RESP, then RESUME_RESP, then STATE, then HAND.
        // Wait for RESUME_RESP
        if (proto_recv(&conn, &op, buf, sizeof(buf), &plen) != 0) { conn_close(&conn); a->lat_ns_out[a->idx] = -1; return NULL; }
    } 
    
    // We might have received STATE if we skipped checks, but let's just consume until we get STATE.
    // Actually, looking at server.c:
    // 1. Send LOGIN_RESP
    // 2. Send RESUME_RESP
    // 3. Send STATE
    // 4. Send HAND
    
    // The original client code just did "recv -> check if STATE".
    // If it receives LOGIN_RESP (new opcode), it might fail if we don't handle it.
    // Original code: `if (proto_recv(...) != 0) ... if (op == OP_STATE ...)`
    // It didn't have LOGIN_RESP before?
    // Let's make it robust: read until we get STATE.
    
    int got_state = 0;
    while (!got_state) {
        // Peek or just read?
        // We already did one read above (expecting STATE in old code).
        // If we are here, we might have got LOGIN_RESP.
        if (op == OP_STATE) {
            got_state = 1;
            break;
        }
        // Read next
        if (proto_recv(&conn, &op, buf, sizeof(buf), &plen) != 0) { conn_close(&conn); a->lat_ns_out[a->idx] = -1; return NULL; }
    }
    
    if (op == OP_STATE && plen == sizeof(state_t) && a->idx == 0) {
        state_t st;
        memcpy(&st, buf, sizeof(st));
        printf("[login] HP=%d AI=%d over=%u winner=%u\n", st.p_hp, st.ai_hp, st.game_over, st.winner);
    }
    
    // After STATE, server sends HAND.
    // Original code: "recv -> check if STATE" (wait, there are 2 states in original code?)
    // Original code:
    // 1. Recv (Expected STATE)
    // 2. Recv (Expected STATE) ?? Why twice?
    // Ah, likely one for "Login initial state" and one was maybe "Resume"? Or maybe just copy-paste?
    // Let's stick to the flow: Login -> State -> Hand.
    // The previous client ignored HAND?
    // The previous client code had:
    //    if (proto_recv... op == OP_STATE ...)
    //    if (proto_recv... op == OP_STATE ...)
    // Maybe it was expecting updates?
    
    // With SSL refactor, let's keep it simple: Read until we get HAND too, to drain socket?
    // Or just proceed to benchmark loop.
    
    long long t1 = now_ns();
    long long sum = (t1 - t0);

    // play a few rounds: PLAY_CARD (dmg=5) + END_TURN
    for (int i = 0; i < a->rounds; i++) {
        play_req_t pc = { .hand_idx = 0 };

        t0 = now_ns();
        if (proto_send(&conn, OP_PLAY_CARD, &pc, sizeof(pc)) != 0) break;
        
        // Wait for STATE response
        int seen_state = 0;
        int seen_hand = 0;
        while (!seen_state || !seen_hand) {
             if (proto_recv(&conn, &op, buf, sizeof(buf), &plen) != 0) break;
             if (op == OP_STATE) {
                 seen_state = 1;
                 if (plen == sizeof(state_t) && a->idx == 0) {
                    state_t st;
                    memcpy(&st, buf, sizeof(st));
                    printf("[play]  HP=%d AI=%d over=%u winner=%u\n", st.p_hp, st.ai_hp, st.game_over, st.winner);
                    if (st.game_over) goto done;
                 }
             } else if (op == OP_HAND) {
                 seen_hand = 1;
             }
        }

        if (proto_send(&conn, OP_END_TURN, NULL, 0) != 0) break;
        
        // Wait for STATE (and potentially HAND if AI played?)
        // Server sends STATE, HAND regardless.
        seen_state = 0;
        seen_hand = 0;
         while (!seen_state || !seen_hand) {
             if (proto_recv(&conn, &op, buf, sizeof(buf), &plen) != 0) break;
             if (op == OP_STATE) {
                 seen_state = 1;
                 if (plen == sizeof(state_t) && a->idx == 0) {
                    state_t st;
                    memcpy(&st, buf, sizeof(st));
                    printf("[end]   HP=%d AI=%d over=%u winner=%u\n", st.p_hp, st.ai_hp, st.game_over, st.winner);
                    if (st.game_over) goto done;
                 }
             } else if (op == OP_HAND) {
                 seen_hand = 1;
             }
        }
        
        t1 = now_ns();
        sum += (t1 - t0);
    }
done:
    conn_close(&conn);
    a->lat_ns_out[a->idx] = sum;
    return NULL;
}

int run_app_mode(const char *host, uint16_t port);

int main(int argc, char **argv) {
    ssl_msg_init();

    /* ---------- APP MODE ---------- */
    if (argc >= 2 && strcmp(argv[1], "--app") == 0) {
        const char *host = "127.0.0.1";
        uint16_t port = 9000;

        if (argc >= 3) host = argv[2];
        if (argc >= 4) port = (uint16_t)atoi(argv[3]);

        return run_app_mode(host, port);
    }
    
    /* ---------- BENCH MODE (default) ---------- */
    const char *host = "127.0.0.1";
    uint16_t port = 9000;
    int threads = 100;
    int rounds = 5;

    if (argc >= 2) threads = atoi(argv[1]);
    if (argc >= 3) rounds = atoi(argv[2]);
    if (argc >= 4) host = argv[3];
    if (argc >= 5) port = (uint16_t)atoi(argv[4]);

    SSL_CTX *ctx = ssl_init_client_ctx();
    if (!ctx) {
        fprintf(stderr, "Failed to init SSL client ctx\n");
        return 1;
    }

    pthread_t *tids = calloc((size_t)threads, sizeof(pthread_t));
    th_arg_t  *args = calloc((size_t)threads, sizeof(th_arg_t));
    long long *lats = calloc((size_t)threads, sizeof(long long));

    for (int i = 0; i < threads; i++) {
        args[i] = (th_arg_t){ .host=host, .port=port, .rounds=rounds, .lat_ns_out=lats, .idx=i, .ctx=ctx };
        pthread_create(&tids[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);

    // simple stats
    long long ok = 0, sum = 0, min = (1LL<<62), max = 0;
    for (int i = 0; i < threads; i++) {
        if (lats[i] <= 0) continue;
        ok++;
        sum += lats[i];
        if (lats[i] < min) min = lats[i];
        if (lats[i] > max) max = lats[i];
    }

    printf("threads=%d rounds=%d ok=%lld fail=%lld\n", threads, rounds, ok, (long long)threads - ok);
    if (ok > 0) {
        printf("latency(sum per thread) avg=%.3f ms min=%.3f ms max=%.3f ms\n",
               (double)sum / (double)ok / 1e6,
               (double)min / 1e6,
               (double)max / 1e6);
    }

    free(tids); free(args); free(lats);
    SSL_CTX_free(ctx);
    return 0;
}