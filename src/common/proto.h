#pragma once
#include <stdint.h>
#include <stddef.h>

#define PROTO_MAGIC_SHM "/tcgmini_shm_v1"

enum opcode_t {
    OP_LOGIN_REQ  = 0x0001,
    OP_LOGIN_RESP = 0x8001,

    OP_PING       = 0x0002,   // client->server (no payload)
    OP_PONG       = 0x8002,   // server->client (no payload)

    OP_RESUME_REQ = 0x0003,   // client->server (payload: resume_req_t)
    OP_RESUME_RESP= 0x8003,   // server->client (payload: resume_resp_t)

    OP_PLAY_CARD  = 0x0101,   // client->server (payload: play_req_t)
    OP_END_TURN   = 0x0102,   // client->server (no payload)

    OP_STATE      = 0x0201,   // server->client (payload: state_t)
    OP_HAND       = 0x0202,   // server->client (payload: hand_t)

    OP_ERROR      = 0xFFFF,   // server->client (payload: error_t optional)
};

#pragma pack(push, 1)
typedef struct {
    uint32_t len;      // total packet length (header+payload), network byte order
    uint16_t opcode;   // network byte order
    uint16_t cksum;    // checksum over entire packet with this field = 0, network byte order
} pkt_hdr_t;
#pragma pack(pop)

typedef struct {
    int32_t ok;        // 1=ok, 0=fail
} login_resp_t;

#pragma pack(push, 1)
typedef struct {
    uint64_t session_id;
} resume_req_t;

typedef struct {
    int32_t ok;         // 1 ok, 0 fail
    uint64_t session_id;
} resume_resp_t;
#pragma pack(pop)

/* ---------------------------
 *  Game Protocol v2 (MVP+)
 * ---------------------------
 */

#define LOG_LINES 6
#define LOG_LEN   64

/* Card Types */
typedef enum {
    CT_ATK = 1,
    CT_HEAL,
    CT_SHIELD,
    CT_BUFF,
    CT_POISON
} card_type_t;

/* Card Definition (Static Data) */
typedef struct {
    uint16_t id;
    uint8_t  type;    // card_type_t
    uint8_t  cost;
    int16_t  value;   // dmg/heal/shield/buff_val
    uint8_t  duration;
    const char *name;
} card_def_t;

/* Hand (Instance Data) - NOW USES IDs */
#pragma pack(push, 1)
typedef struct {
    uint8_t n;
    uint16_t card_ids[8]; // Max 8, server sends IDs
} hand_t;

/* Play Card Request */
typedef struct {
    uint8_t hand_idx;  // 0..n-1
} play_req_t;
#pragma pack(pop)

typedef enum {
    PHASE_DRAW = 0,
    PHASE_MAIN = 1,
    PHASE_END  = 2,
} phase_t;

// state includes resources + statuses + ring-buffer logs
#define LOG_LINES 6
#define LOG_LEN   64

#pragma pack(push, 1)
typedef struct {
    int16_t p_hp;      // player HP
    int16_t ai_hp;     // AI HP

    uint8_t turn;      // 0=player, 1=ai
    uint8_t phase;     // 0=DRAW, 1=MAIN, 2=END
    uint8_t game_over; // 0/1
    uint8_t winner;    // 0=none, 1=player, 2=ai

    // mana system
    uint8_t mana;      // current mana of current turn side (MVP)
    uint8_t max_mana;  // usually 3

    // statuses
    int16_t p_shield;  // absorbs damage first
    int16_t ai_shield;

    int16_t p_buff;    // next attack +X then consumed
    int16_t ai_buff;

    uint8_t p_poison;  // remaining poison turns (ticks at end turn)
    uint8_t ai_poison;

    // log ring buffer
    uint8_t log_head;  // next write index (0..LOG_LINES-1)
    char    logs[LOG_LINES][LOG_LEN];
} state_t;
#pragma pack(pop)

// optional: error payload
#pragma pack(push, 1)
typedef struct {
    int32_t code;      // e.g., -2 = not enough mana, -1 invalid idx
    char    msg[48];   // short message
} error_t;
#pragma pack(pop)

// checksum
uint16_t proto_checksum16(const void *buf, size_t n);

// pack & send / recv helpers
// pack & send / recv helpers
#include "net.h"
int proto_send(connection_t *c, uint16_t opcode, const void *payload, uint32_t payload_len);
int proto_recv(connection_t *c, uint16_t *opcode_out, void *payload_buf, uint32_t payload_buf_cap, uint32_t *payload_len_out);

