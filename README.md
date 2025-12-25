# Mini TCG Project

## Recent Changes (Timeout Handling)

Implemented network timeout handling to prevent indefinite blocking during network issues.

### 1. Client-Side Timeout
*   **Settings**: 3-second timeout for all network operations.
*   **Implementation**: Used `SO_RCVTIMEO` and `SO_SNDTIMEO` socket options.
*   **Error Reporting**: Modifed `client.c` and `client_app.c` to catch `EAGAIN`/`EWOULDBLOCK` errors and print a user-friendly message: `[client] Receive timeout (waited > 3s)`.

### 2. Server-Side Timeout
*   **Settings**: 5-second timeout for server connections.
*   **Implementation**: Applied timeout settings in `server.c` immediately after accepting a connection.


### 3. Verification
*   Added `net_set_timeout` helper in `src/common/net.c`.
*   Verified by temporarily adding a delay in the server login process.

## TLS 加密通訊 (TLS Security)

本專案已全面啟用 TLS 加密通訊，以確保資料傳輸的安全性。

### 1. 前置需求 (Prerequisites)
編譯前請確保已安裝 OpenSSL 開發套件：
```bash
sudo apt install libssl-dev
```

### 2. 產生憑證 (Generate Certificates)
在啟動伺服器前，必須先產生測試用的自簽憑證 (`server.crt`, `server.key`)：
```bash
./gen_certs.sh
```

### 3. 使用 Wireshark 驗證 (Verification)
您可以透過 Wireshark 觀察封包，確認通訊已加密：
1. 開啟 Wireshark 並監聽 Loopback (lo) 介面。
2. 設定過濾器：`tcp.port == 9000`。
3. 執行伺服器與客戶端。
4. 您將會看到 `Client Hello`, `Server Hello`, `Application Data` 等 TLS 封包，而非明文的遊戲指令。

## 快速開始 (Quick Start)

### 1. 編譯專案 (Build)
```bash
make
```

##### 2. 啟動遊戲 (Start Game) #####
本專案提供文字介面 (CLI) 與圖形介面 (GUI) 兩種模式。

#### A. 文字介面 (CLI Mode) - 預設
請在兩個不同的終端機視窗分別執行伺服器與客戶端：

**終端機 1 (伺服器Server):**
```bash

# 預設 Port 為 9000
./server
```

**終端機 2 (客戶端Client):**
```bash
# 連線到 127.0.0.1:9000
./client --app 127.0.0.1 9000
```

### 3. 預期結果 (Expected Output)
成功連線後，您應該會看到如下的遊戲畫面 (CLI 介面)：
```text
  Mini TCG (Client App)  |  Keys: 1/2/3=Play  E=End Turn  Q=Quit
  Player HP: 30
  AI     HP: 30

  Turn: PLAYER
  Game Over: NO

  Hand:
    1) ...
    2) ...
    3) ...
```




##### B. 圖形介面 (GUI Mode) - 選用 #####
若您的環境已安裝 Raylib，可編譯並執行圖形版客戶端：

1. **編譯:**
   ```bash
   make client_gui
   ```
2. **執行:**
   ```bash
   ./client_gui 127.0.0.1 9000
   ```
   ./client_gui 127.0.0.1 9000
   ```
   *(注意：此模式需要 Raylib 函式庫支援。若出現 `raylib.h: No such file` 錯誤，請先安裝 Raylib)*




##### 逾時處理測試 (Timeout Handling Test) #####
您可以使用 `nc` (netcat) 模擬一個不會回應的伺服器，來測試客戶端的逾時機制：

1. **啟動模擬伺服器 (不回應任何資料):**
   ```bash
   nc -l -p 9000
   ```

2. **啟動客戶端:**
   ```bash
   ./client --app 127.0.0.1 9000
   ```

3. **預期結果:**
   客戶端會在約 3 秒後顯示逾時錯誤訊息並退出：
   ```
   [client] Receive timeout (waited > 3s)
   ```
