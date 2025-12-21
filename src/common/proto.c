#include "proto.h"
#include "net.h"
#include <string.h>
#include <arpa/inet.h>

uint16_t proto_checksum16(const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += p[i];
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum);
}

int proto_send(int fd, uint16_t opcode, const void *payload, uint32_t payload_len) {
    pkt_hdr_t h;
    uint32_t total_len = (uint32_t)sizeof(h) + payload_len;

    h.len = htonl(total_len);
    h.opcode = htons(opcode);
    h.cksum = htons(0);

    // build temp buffer for checksum
    uint8_t stackbuf[2048];
    uint8_t *buf = stackbuf;
    if (total_len > sizeof(stackbuf)) return -1; // keep it simple for MVP

    memcpy(buf, &h, sizeof(h));
    if (payload_len && payload) memcpy(buf + sizeof(h), payload, payload_len);

    // compute checksum with cksum=0 in header
    uint16_t cks = proto_checksum16(buf, total_len);
    ((pkt_hdr_t*)buf)->cksum = htons(cks);

    // write out
    if (writen(fd, buf, total_len) != (ssize_t)total_len) return -1;
    return 0;
}

int proto_recv(int fd, uint16_t *opcode_out, void *payload_buf, uint32_t payload_buf_cap, uint32_t *payload_len_out) {
    pkt_hdr_t h;
    if (readn(fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) return -1;

    uint32_t total_len = ntohl(h.len);
    uint16_t opcode = ntohs(h.opcode);
    uint16_t got_ck = ntohs(h.cksum);

    if (total_len < sizeof(h) || total_len > (sizeof(h) + payload_buf_cap)) return -1;

    uint32_t payload_len = total_len - (uint32_t)sizeof(h);
    if (payload_len) {
        if (readn(fd, payload_buf, payload_len) != (ssize_t)payload_len) return -1;
    }

    // verify checksum
    uint8_t stackbuf[2048];
    uint8_t *buf = stackbuf;
    if (total_len > sizeof(stackbuf)) return -1;

    pkt_hdr_t h2 = h;
    h2.cksum = htons(0);
    memcpy(buf, &h2, sizeof(h2));
    if (payload_len) memcpy(buf + sizeof(h2), payload_buf, payload_len);

    uint16_t calc = proto_checksum16(buf, total_len);
    if (calc != got_ck) return -1;

    if (opcode_out) *opcode_out = opcode;
    if (payload_len_out) *payload_len_out = payload_len;
    return 0;
}