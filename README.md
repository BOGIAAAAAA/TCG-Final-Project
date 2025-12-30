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

## TLS Security

This project fully implements TLS encryption to ensure secure data transmission.

### 1. Prerequisites
Please ensure the OpenSSL development package is installed before compiling:
```bash
sudo apt install libssl-dev
```

### 2. Generate Certificates
Before starting the server, you must generate self-signed certificates (server.crt and server.key) for testing purposes:
```bash
./gen_certs.sh
```

### 3. Verification via Wireshark
You can use Wireshark to inspect packets and confirm that the communication is encrypted:
1. Open Wireshark and capture traffic on the Loopback (lo) interface.
2. Set the display filter to: tcp.port == 9000.
3. Run the server and client.
4. You will observe TLS packets (e.g., Client Hello, Server Hello, Application Data) instead of plaintext game commands.

## Stress Testing
This document describes the stress testing design and results for our high-concurrency Client–Server network service system. The purpose of this test is to verify the server’s ability to handle multiple concurrent client connections, ensure system stability under load, and measure basic performance metrics such as latency.

### Test Objectives
The stress test is designed to validate the following aspects:
1. Correct handling of high concurrent connections
2. Stability of the multi-process server architecture
3. Correct synchronization using IPC mechanisms
4. Absence of crashes, deadlocks, or resource leaks under load
5. Measurement of end-to-end request latency

### Test Architecture

#### Client Side (Load Generator)
1. Implemented using a multi-threaded architecture
2. Each thread acts as an independent client
3. Each client:
	• Connects to the server
	• Performs a login handshake
4. Repeatedly sends gameplay requests
5. Latency is measured per thread

#### Server Side
1. Uses a multi-process (fork-based) architecture
2. Each client connection is handled by a worker process
3. Shared statistics are maintained using shared memory IPC
4. Server logic remains authoritative during the test
### Test Environment
1. OS: Ubuntu Linux
2. Architecture: POSIX-compliant system
3. Compiler: GCC
4. Networking: TCP sockets
5. IPC: POSIX shared memory
6. Concurrency Model:
7. Client: multi-threaded
8. Server: multi-process

### How to Run the Stress Test
#### 1. Start the Server
```bash
./server 9000
```
#### 2. Run the Stress Test Client
```bash
./client <threads> <rounds> <server_ip> <port>
```
Example:
```bash
./client 100 5 127.0.0.1 9000
```
Parameters:
1. threads: Number of concurrent client threads
2. rounds: Number of request rounds per client
3. server_ip: Server IP address
4. port: Server listening port

### Output Example
```text
threads=100 rounds=5 ok=100 fail=0
latency(sum per thread) avg=3.236 ms min=0.748 ms max=6.108 ms
```
#### Output Explanation
1. threads: Total number of concurrent clients
2. rounds: Requests sent per client
3. ok: Successfully completed client sessions
4. fail: Failed or interrupted sessions
5. avg latency: Average latency per client thread
6. min / max latency: Best and worst observed latency

### Observations
1. The server successfully handled 100 concurrent clients without failure
2. No abnormal termination or deadlock was observed
3. Latency remained within an acceptable range for a turn-based game
4. IPC statistics confirmed correct packet and connection counting

##  Monitor
monitor is a runtime monitoring utility designed for the server-side of our high-concurrency Client–Server system. It observes shared server statistics through IPC (shared memory) and provides real-time visibility into server behavior during execution. This tool is mainly used to verify correctness, stability, and concurrency behavior under load.

### Purpose
The monitor tool is designed to achieve the following goals:
•	Observe server activity without interfering with execution
•	Validate correct usage of inter-process communication (IPC)
•	Provide real-time insight into:
	•	Active connections
	•	Total processed packets
•	Assist debugging during stress testing and demo sessions

### Architecture
#### IPC Mechanism
•	The server maintains runtime statistics using POSIX shared memory
•	All worker processes update shared counters
•	monitor attaches to the same shared memory region in read-only mode

This design ensures:
•	Low overhead
•	No synchronization interference
•	Safe concurrent access

### Monitored Metrics
The monitor displays the following runtime information:

#### 1. Total Connections
Number of client connections accepted by the server

#### 2. Total Packets Processed
Number of protocol packets handled by the server

#### 3. Server Status
Indicates whether the server is currently running

### How to Run
#### 1. Start the Server
```bash
./server 9000
```
#### 2. Start the Monitor 
```bash
./monitor
```
The monitor will continuously display updated server statistics.

### Example Output
```text
[monitor]
Connections: 128
Packets:     3421
```

### Use Cases
•	Live demonstration during project presentation
•	Verifying server behavior during stress testing
•	Confirming IPC correctness and synchronization
•	Debugging abnormal connection or packet growth

### Implementation Notes
•	Implemented in C (POSIX-compliant)
•	Uses:
	•	shm_open
	•	mmap
	•	Shared memory synchronization primitives
•	No network sockets are used by the monitor
•	Can be started or stopped independently of the server

## Quick Start

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
