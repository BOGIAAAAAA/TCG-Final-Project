#include "ipc.h"
#include "proto.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

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