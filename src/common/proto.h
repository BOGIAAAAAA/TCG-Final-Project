#pragma once
#include <stdint.h>
#include <stddef.h>

#define PROTO_MAGIC_SHM "/tcgmini_shm_v1"

enum opcode_t {
    OP_LOGIN_REQ  = 0x0001,
    OP_LOGIN_RESP = 0x8001,

    OP_PLAY_CARD  = 0x0101,   // client->server
    OP_END_TURN   = 0x0102,   // client->server
    OP_STATE      = 0x0201,   // server->client (game state update)

    OP_ERROR      = 0xFFFF,

    OP_HAND       = 0x0202,   // server->client
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

typedef struct {
    uint16_t card_id;  // e.g., 1 = "Firebolt"
    uint16_t dmg;      // damage
} play_card_t;

typedef struct {
    int16_t p_hp;      // player HP
    int16_t ai_hp;     // AI HP
    uint8_t turn;      // 0=player, 1=ai
    uint8_t game_over; // 0/1
    uint8_t winner;    // 0=none, 1=player, 2=ai
    uint8_t reserved;
} state_t;

typedef struct {
    uint8_t n;                 // number of cards
    uint8_t reserved;
    uint16_t card_id[8];
    uint16_t dmg[8];
} hand_t;

// checksum
uint16_t proto_checksum16(const void *buf, size_t n);

// pack & send / recv helpers
int proto_send(int fd, uint16_t opcode, const void *payload, uint32_t payload_len);
int proto_recv(int fd, uint16_t *opcode_out, void *payload_buf, uint32_t payload_buf_cap, uint32_t *payload_len_out);