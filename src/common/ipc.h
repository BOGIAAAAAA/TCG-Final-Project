#pragma once
#include <stdint.h>

typedef struct {
    uint64_t total_connections;
    uint64_t total_packets;
} shm_stats_t;

// Session Store
#include "proto.h"
#include <time.h>

#define MAX_SESSIONS 64
#define STORE_MAGIC_SHM "/tcg_store_v1"

typedef struct {
    uint64_t session_id;
    time_t   last_seen;
    state_t  st;
    hand_t   hand;
    int      valid;
} session_entry_t;

typedef struct {
    session_entry_t sessions[MAX_SESSIONS];
} shm_store_t;

shm_stats_t* ipc_stats_init(int create);
void ipc_stats_inc_conn(shm_stats_t *s);
void ipc_stats_inc_pkt(shm_stats_t *s);

shm_store_t* ipc_store_init(int create);
uint64_t ipc_alloc_session(shm_store_t *store);
int ipc_save_session(shm_store_t *store, uint64_t sid, const state_t *st, const hand_t *h);
int ipc_load_session(shm_store_t *store, uint64_t sid, state_t *st, hand_t *h);
int ipc_touch_session(shm_store_t *store, uint64_t sid);