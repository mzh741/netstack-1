#ifndef NETSTACK_ARP_H
#define NETSTACK_ARP_H

#include <stdint.h>
#include <stdbool.h>

#include <netstack/log.h>
#include <netstack/eth/ether.h>
#include <netstack/lock/retlock.h>


#define ARP_HDR_LEN sizeof(struct arp_hdr)

/* ARP supported hardware types */
#define ARP_HW_ETHER        0x001
#define ARP_HW_IEEE_802     0x006
#define ARP_HW_ARCNET       0x007
#define ARP_HW_FRM_RLY      0x00F
#define ARP_HW_ATM16        0x010
#define ARP_HW_HDLC         0x011
#define ARP_HW_FIB_CH       0x012
#define ARP_HW_ATM19        0x013
#define ARP_HW_SERIAL       0x014

/* ARP operation types */
#define ARP_OP_REQUEST      0x001
#define ARP_OP_REPLY        0x002

static inline char const *fmt_arp_op(unsigned short op) {
    switch (op) {
        case ARP_OP_REQUEST:    return "ARP_OP_REQUEST";
        case ARP_OP_REPLY:      return "ARP_OP_REPLY";
        default:                return NULL;
    }
}

/* ARP message header */
struct arp_hdr {
    uint16_t hwtype,    /* Hardware type */
             proto;     /* Protocol type */
    uint8_t  hlen,      /* Hardware address length */
             plen;      /* Protocol address length */
    uint16_t op;        /* ARP operation (ARP_OP_*) */
}__attribute((packed));

/* ARP IPv4 payload */
struct arp_ipv4 {
    uint8_t  saddr[ETH_ADDR_LEN];
    uint32_t sipv4;
    uint8_t  daddr[ETH_ADDR_LEN];
    uint32_t dipv4;
}__attribute((packed));

/*!
 * Returns a struct arp_hdr from the frame->head
 */
#define arp_hdr(frame) ((struct arp_hdr *) (frame)->head)

bool arp_log(struct pkt_log *log, struct frame *frame);

/*!
 * Receives an arp frame for processing in the network stack
 * @param frame
 */
void arp_recv(struct frame *frame);

// TODO: reduce redundant arguments passed to arp_send_req/reply
// TODO: infer interface and hwtype based on routing rules
/*!
 * Sends an ARP request for an IPv4 address
 * @param intf interface to send request through
 * @param hwtype ARP_HW_* hardware type
 * @param saddr our IPv4 address (from intf)
 * @param daddr address requesting hwaddr for
 * @return 0 on success, various error values otherwise
 */
int arp_send_req(struct intf *intf, uint16_t hwtype,
                 addr_t *saddr, addr_t *daddr);

/*!
 * Sends a reply to an incoming ARP request
 * @param intf interface to send reply through (should the same as request)
 * @param hwtype ARP_HW_* hardware type (must match request)
 * @param sip our IPv4 address (from intf)
 * @param dip address requesting our hwaddr
 * @param daddr our hwaddr (from intf)
 * @return 0 on success, various error values otherwise
 */
int arp_send_reply(struct intf *intf, uint16_t hwtype, ip4_addr_t sip,
                   ip4_addr_t dip, eth_addr_t daddr);

/*!
 * Retrieves an arp_entry from the arp_table matching the hwtype and protoaddr
 * @return An entry, if found, otherwise NULL
 */
struct arp_entry *arp_get_entry(llist_t *arptbl, proto_t hwtype,
                                 addr_t *protoaddr);

/*!
 * Converts a PROTO_* value to a ARP_HW_*
 * @return a ARP_HW_* value, or 0 if no match
 */
uint16_t arp_proto_hw(proto_t proto);

/* ARP table cache */

/* ARP cache validity */
#define ARP_UNKNOWN         0x001
#define ARP_PENDING         0x002
#define ARP_RESOLVED        0x004
#define ARP_PERMANENT       0x008

static inline char const *fmt_arp_state(uint8_t state) {
    switch (state) {
        case ARP_UNKNOWN:   return "Unknown";
        case ARP_PENDING:   return "Pending";
        case ARP_RESOLVED:  return "Resolved";
        case ARP_PERMANENT: return "Permanent";
        default:            return "?";
    }
}

#define ARP_WAIT_TIMEOUT       10   /* seconds */

struct arp_entry {
    uint8_t state;
    addr_t  protoaddr;
    addr_t  hwaddr;
    pthread_mutex_t lock;
};

/*!
 * Prints the ARP table to the log with the specified level
 * @param intf interface to read ARP table from
 * @param file file to write ARP table to
 */
void arp_log_tbl(struct intf *intf, loglvl_t level);

/*!
 * Attempt to update an existing protocol address entry
 * @param intf interface to add mapping to
 * @param hwaddr new hardware type & address
 * @param protoaddr protocol address to update
 * @return true if an existing entry was updated, false otherwise
 */
bool arp_update_entry(struct intf *intf, addr_t *hwaddr, addr_t *protoaddr);

/*!
 * Add new protocol/hardware pair to the ARP cache
 * @param intf interface to add mapping to
 * @param hwaddr hardware type and address
 * @param protoaddr protocol type and address
 * @return true if a new entry was inserted, false otherwise
 */
bool arp_cache_entry(struct intf *intf, addr_t *hwaddr, addr_t *protoaddr);

#endif //NETSTACK_ARP_H
