#ifndef NETSTACK_ADDR_H
#define NETSTACK_ADDR_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <netstack/proto.h>

/* Ethernet */
#define ETH_ADDR_LEN    6 /* # of octets per address */
typedef uint8_t eth_addr_t[ETH_ADDR_LEN];

/* IPv4 */
typedef uint32_t ip4_addr_t;

/* IPv6*/
typedef uint8_t ip6_addr_t[16];


typedef struct {
    uint16_t proto;
    union {
        uint8_t     address;
        eth_addr_t  ether;
        ip4_addr_t  ipv4;
        ip6_addr_t  ipv6;
    };
} addr_t;


/*!
 * Gets the length of a protocol address given the protocol
 * @return the length of the address, or 0 for invalid protocols
 */
static inline size_t addrlen(uint16_t proto) {
    switch (proto) {
        /* Hardware protocols (layer 1) */
        case PROTO_ETHER:   return sizeof(eth_addr_t);

        /* Network protocols (layer 2) */
        case PROTO_IPV4:    return sizeof(ip4_addr_t);
        case PROTO_IPV6:    return sizeof(ip6_addr_t);

        case PROTO_IP:
        case PROTO_TCP:
        case PROTO_UDP:
        case PROTO_ICMP:
        default:
            // Addresses that have no length are invalid
            return 0;
    }
}

/*!
 * Compares two addresses and returns true if both the protocols and
 * addresses are the same
 * @return true if both addresses and protocols match
 */
static inline bool addreq(addr_t *a, addr_t *b) {
    if (a == NULL || b == NULL)
        return false;
    size_t len = addrlen(a->proto);
    return len != 0 &&
           a->proto == b->proto &&
           memcmp(&a->address, &b->address, len) == 0;
}


#endif //NETSTACK_ADDR_H
