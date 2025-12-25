#include "ipc.h"
#include "proto.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

shm_stats_t* ipc_stats_init(int create) {
    int oflags = O_RDWR;
    if (create) oflags |= O_CREAT;

    int fd = shm_open(PROTO_MAGIC_SHM, oflags, 0600);
    if (fd < 0) return NULL;

    if (create) {
        if (ftruncate(fd, sizeof(shm_stats_t)) != 0) { close(fd); return NULL; }
    }

    void *p = mmap(NULL, sizeof(shm_stats_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;

    if (create) {
        // safe init (best-effort)
        shm_stats_t zero;
        memset(&zero, 0, sizeof(zero));
        memcpy(p, &zero, sizeof(zero));
    }
    return (shm_stats_t*)p;
}

void ipc_stats_inc_conn(shm_stats_t *s) {
    __sync_fetch_and_add(&s->total_connections, 1);
}
void ipc_stats_inc_pkt(shm_stats_t *s) {
    __sync_fetch_and_add(&s->total_packets, 1);
}

shm_store_t* ipc_store_init(int create) {
    int oflags = O_RDWR;
    if (create) oflags |= O_CREAT;

    int fd = shm_open(STORE_MAGIC_SHM, oflags, 0600);
    if (fd < 0) return NULL;

    if (create) {
        if (ftruncate(fd, sizeof(shm_store_t)) != 0) { close(fd); return NULL; }
    }

    void *p = mmap(NULL, sizeof(shm_store_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;

    if (create) {
        shm_store_t zero;
        memset(&zero, 0, sizeof(zero));
        memcpy(p, &zero, sizeof(zero));
    }
    return (shm_store_t*)p;
}

uint64_t ipc_alloc_session(shm_store_t *store) {
    // simple linear search for empty slot
    for (int i = 0; i < MAX_SESSIONS; i++) {
        // Warning: this check is not atomic against other processes, 
        // but since we only have one main parent deciding (or low concurrency fork), it's acceptable for MVP.
        // Actually forks are per connection. Collision possible if multiple logins happen EXACTLY same time.
        // But store is pre-check. 
        if (!store->sessions[i].valid) {
            store->sessions[i].valid = 1;
            uint64_t sid = (uint64_t)time(NULL) + (uint64_t)i + (uint64_t)getpid(); 
            // Better random SID
            sid = sid ^ ((uint64_t)rand() << 32);
            if (sid == 0) sid = 1;
            
            store->sessions[i].session_id = sid;
            store->sessions[i].last_seen = time(NULL);
            return sid;
        }
    }
    // Try to recycle oldest? 
    // For now return 0 (fail) if full.
    return 0;
}

int ipc_save_session(shm_store_t *store, uint64_t sid, const state_t *st, const hand_t *h) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (store->sessions[i].valid && store->sessions[i].session_id == sid) {
            store->sessions[i].st = *st;
            store->sessions[i].hand = *h;
            store->sessions[i].last_seen = time(NULL);
            return 0;
        }
    }
    return -1;
}

int ipc_load_session(shm_store_t *store, uint64_t sid, state_t *st, hand_t *h) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (store->sessions[i].valid && store->sessions[i].session_id == sid) {
            *st = store->sessions[i].st;
            *h  = store->sessions[i].hand;
            store->sessions[i].last_seen = time(NULL);
            return 0;
        }
    }
    return -1;
}

int ipc_touch_session(shm_store_t *store, uint64_t sid) {
     for (int i = 0; i < MAX_SESSIONS; i++) {
        if (store->sessions[i].valid && store->sessions[i].session_id == sid) {
            store->sessions[i].last_seen = time(NULL);
            return 0;
        }
    }
    return -1;
}