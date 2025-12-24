# TCG Final Project

這是一個簡易的卡牌對戰 (TCG) 遊戲專案，包含伺服器 (Server) 與客戶端 (Client)，支援多人連線 (Fork-based) 與簡單的 AI 對戰邏輯。

專案結構如下：

## 檔案功能說明

### 1. 共用函式庫 (`src/common/`)

這部分包含伺服器與客戶端共用的核心功能，編譯後會產生 `libcommon.a`。

*   **`src/common/ipc.h` / `src/common/ipc.c`**
    *   **功能**：處理行程間通訊 (IPC) 與共享記憶體 (Shared Memory)。
    *   **細節**：
        *   定義了統計結構 `shm_stats_t`，用來記錄伺服器的「總連線數」與「總封包數」。
        *   提供 `ipc_stats_init()` 函式來建立或開啟共享記憶體。
        *   提供 `ipc_stats_inc_conn()` 與 `ipc_stats_inc_pkt()`，利用 atomic 操作安全地在多行程環境下更新統計數據。

*   **`src/common/net.h` / `src/common/net.c`**
    *   **功能**：封裝網路通訊的底層函式。
    *   **細節**：
        *   提供 `readn()` 與 `writen()` 函式，確保在 TCP 串流中完整讀寫指定大小的資料 (處理 Partial Read/Write 與信號中斷)。
        *   `tcp_listen()`：簡化伺服器綁定 (Bind) 與監聽 (Listen) Port 的流程。
        *   `tcp_connect()`：簡化客戶端解析主機名稱並連線 (Connect) 到伺服器的流程。

*   **`src/common/proto.h` / `src/common/proto.c`**
    *   **功能**：定義應用層通訊協定 (Protocol)。
    *   **細節**：
        *   定義封包標頭 `pkt_hdr_t`，包含長度、Opcode 與 Checksum。
        *   定義操作碼 `enum opcode_t` (如 `OP_LOGIN_REQ`, `OP_PLAY_CARD`, `OP_STATE` 等)。
        *   定義各類訊息的 Payload 結構 (如 `state_t` 遊戲狀態, `hand_t` 手牌)。
        *   提供 `proto_send()` 與 `proto_recv()` 負責封包的序列化/反序列化與 Checksum 驗證。

---

### 2. 伺服器端 (`src/server.c`)

*   **`src/server.c`**
    *   **功能**：遊戲伺服器主程式。
    *   **流程**：
        *   啟動時初始化共享記憶體 (IPC Stats)。
        *   監聽指定 Port (預設 9000)。
        *   **Fork-based Concurrency**：每當有新連線 (Accept) 時，呼叫 `fork()` 產生子行程 (Child Process) 處理該客戶端。
        *   **遊戲邏輯 (`run_session`)**：
            *   維護玩家與 AI 的 HP、手牌與回合狀態。
            *   處理客戶端的登入 (`OP_LOGIN_REQ`)、出牌 (`OP_PLAY_CARD`) 與結束回合 (`OP_END_TURN`) 請求。
            *   簡單的 AI 邏輯：當玩家結束回合後，AI 會自動進行攻擊。
            *   每收到封包即更新共享記憶體中的統計數據。

---

### 3. 客戶端 (`src/client.c`, `src/client_app.c`)

*   **`src/client.c`**
    *   **功能**：客戶端進入點，包含「壓力測試模式」與「互動模式」。
    *   **壓力測試 (Benchmark Mode)**：
        *   預設模式。
        *   建立大量執行緒 (Thread) 並發連線至伺服器。
        *   模擬登入與對戰流程，計算連線延遲 (Latency) 與吞吐量。
        *   統計並輸出測試結果 (Min/Max/Avg Latency)。
    *   **互動模式 (App Mode)**：
        *   若執行時帶入 `--app` 參數，則呼叫 `run_app_mode()` 進入圖形介面模式。

*   **`src/client_app.c`**
    *   **功能**：提供以 `ncurses` 實作的終端機圖形介面 (TUI)。
    *   **細節**：
        *   `run_app_mode()`：建立與伺服器的連線。
        *   `draw_ui()`：繪製遊戲畫面，顯示 HP、手牌、回合狀態與遊戲結果。
        *   處理使用者鍵盤輸入 (1-3 出牌, E 結束回合, Q 離開)，並透過 `proto_send` 發送對應指令。

---

### 4. 建置系統

*   **`Makefile`**
    *   **功能**：專案編譯腳本。
    *   **Target**：
        *   `libcommon.a`：編譯 common 目錄下的檔案並打包成靜態函式庫。
        *   `server`：編譯 `src/server.c` 並連結 `libcommon.a`。
        *   `client`：編譯 `src/client.c` 與 `src/client_app.c`，連結 `libcommon.a` 與 `ncursesw` 函式庫。

## 編譯與執行

1.  **編譯**
    ```bash
    make
    ```

2.  **執行伺服器**
    ```bash
    ./server [port]
    # 例如: ./server 9000
    ```

3.  **執行客戶端 (壓力測試)**
    ```bash
    ./client [threads] [rounds] [host] [port]
    # 例如: ./client 50 10 127.0.0.1 9000
    ```

4.  **執行客戶端 (遊玩模式)**
    ```bash
    ./client --app [host] [port]
    # 例如: ./client --app 127.0.0.1 9000
    ```
