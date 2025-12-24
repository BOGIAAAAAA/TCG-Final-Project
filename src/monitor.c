#include "common/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

int main() {
    // 1. Attach to existing Shared Memory (Read-only)
    shm_stats_t *stats = ipc_stats_init(0);
    
    if (!stats) {
        fprintf(stderr, "Error: Server is not running (Cannot attach to SHM).\n");
        return 1;
    }

    // 2. Monitoring Loop
    while (1) {
        // Clear screen using ANSI escape code
        printf("\033[2J\033[H");
        
        // Get current system time
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_buf[26];
        strftime(time_buf, 26, "%H:%M:%S", tm_info);

        printf("========================================\n");
        printf("   TCG SERVER MONITOR (PID: %d)   \n", getpid());
        printf("========================================\n");
        printf(" System Time        : %s\n", time_buf);
        printf("----------------------------------------\n");
        
        // Display IPC stats from Shared Memory
        // Cast to unsigned long for portability across 32/64-bit systems
        printf(" Active Connections : %lu\n", (unsigned long)stats->total_connections); 
        printf(" Total Packets Recv : %lu\n", (unsigned long)stats->total_packets);
        
        printf("========================================\n");
        printf(" [Press Ctrl+C to exit monitor]\n");
        
        fflush(stdout); 
        sleep(1); 
    }
    return 0;
}