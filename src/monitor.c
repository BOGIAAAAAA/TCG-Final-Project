#include "common/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h> // 為了 uint64_t

int main() {
    // 1. 連接現有的 Shared Memory (0 = 只讀取，不創建)
    shm_stats_t *stats = ipc_stats_init(0);
    
    if (!stats) {
        fprintf(stderr, "Error: Server is not running (Cannot attach to SHM).\n");
        return 1;
    }

    // 2. 監控迴圈
    while (1) {
        // 使用 ANSI Escape Code 清除螢幕
        printf("\033[2J\033[H");
        
        // 獲取當前時間
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_buf[26];
        strftime(time_buf, 26, "%H:%M:%S", tm_info);

        printf("========================================\n");
        printf("   TCG SERVER MONITOR (PID: %d)   \n", getpid());
        printf("========================================\n");
        printf(" System Time        : %s\n", time_buf);
        printf("----------------------------------------\n");
        // 這些數據來自 Shared Memory，證明 IPC 運作中
        // 使用 %lu 來對應 uint64_t (在某些 64bit 系統上可能需要 %llu，若報錯請改用 %llu)
        printf(" Active Connections : %lu\n", (unsigned long)stats->total_connections); 
        printf(" Total Packets Recv : %lu\n", (unsigned long)stats->total_packets);
        printf("========================================\n");
        printf(" [Press Ctrl+C to exit monitor]\n");
        
        fflush(stdout); 
        sleep(1); 
    }
    return 0;
}
