#pragma once
#include <stdint.h>

typedef struct {
    uint64_t total_connections;
    uint64_t total_packets;
} shm_stats_t;

shm_stats_t* ipc_stats_init(int create);
void ipc_stats_inc_conn(shm_stats_t *s);
void ipc_stats_inc_pkt(shm_stats_t *s);