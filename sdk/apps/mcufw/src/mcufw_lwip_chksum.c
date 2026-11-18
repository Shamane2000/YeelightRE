/* mcufw_lwip_chksum.c — Replacement for broken SDK inet_chksum.c
 * SDK lwIP 1.4.x has signature mismatch between inet_chksum.h and inet_chksum.c.
 * We provide our own implementations to avoid patching vendor code.
 */
#include "lwip/opt.h"
#include "lwip/inet_chksum.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

/* Standard internet checksum (RFC 1071) */
static u16_t mcufw_standard_chksum(const void *dataptr, u16_t len)
{
    const u8_t *pb = (const u8_t *)dataptr;
    u32_t acc = 0;
    u16_t i;
    for (i = 0; i < (len >> 1); i++) {
        acc += ((u16_t)pb[0] << 8) | pb[1];
        pb += 2;
    }
    if (len & 1) {
        acc += (u16_t)(*pb) << 8;
    }
    while (acc >> 16) {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    return (u16_t)(~acc);
}

u16_t inet_chksum(void *dataptr, u16_t len)
{
    return mcufw_standard_chksum(dataptr, len);
}

u16_t inet_chksum_pbuf(struct pbuf *p)
{
    u32_t acc = 0;
    struct pbuf *q;
    u8_t swapped = 0;
    for (q = p; q != NULL; q = q->next) {
        const u8_t *pb = (const u8_t *)q->payload;
        u16_t len = q->len;
        u16_t i;
        if (swapped) {
            /* Previous pbuf had odd length; first byte pairs with high byte */
            acc += (u16_t)(*pb);
            pb++;
            len--;
            swapped = 0;
        }
        for (i = 0; i < (len >> 1); i++) {
            acc += ((u16_t)pb[0] << 8) | pb[1];
            pb += 2;
        }
        if (len & 1) {
            acc += (u16_t)(*pb) << 8;
            swapped = 1;
        }
    }
    while (acc >> 16) {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    return (u16_t)(~acc);
}

u16_t inet_chksum_pseudo(struct pbuf *p, u8_t proto, u16_t proto_len,
                          ip_addr_t *src, ip_addr_t *dest)
{
    u32_t acc = 0;
    struct pbuf *q;
    u8_t swapped = 0;
    /* Pseudo-header */
    acc += (ntohl(src->addr) >> 16) & 0xFFFF;
    acc += ntohl(src->addr) & 0xFFFF;
    acc += (ntohl(dest->addr) >> 16) & 0xFFFF;
    acc += ntohl(dest->addr) & 0xFFFF;
    acc += (u16_t)proto;
    acc += proto_len;
    /* Payload */
    for (q = p; q != NULL; q = q->next) {
        const u8_t *pb = (const u8_t *)q->payload;
        u16_t len = q->len;
        u16_t i;
        if (swapped) {
            acc += (u16_t)(*pb);
            pb++;
            len--;
            swapped = 0;
        }
        for (i = 0; i < (len >> 1); i++) {
            acc += ((u16_t)pb[0] << 8) | pb[1];
            pb += 2;
        }
        if (len & 1) {
            acc += (u16_t)(*pb) << 8;
            swapped = 1;
        }
    }
    while (acc >> 16) {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    return (u16_t)(~acc);
}

u16_t inet_chksum_pseudo_partial(struct pbuf *p, u8_t proto, u16_t proto_len,
                                  u16_t chksum_len, ip_addr_t *src, ip_addr_t *dest)
{
    /* For simplicity, compute full pseudo checksum; partial is only an optimization */
    (void)chksum_len;
    return inet_chksum_pseudo(p, proto, proto_len, src, dest);
}
