#include <stdlib.h>

#include <sys/param.h>
#include <netinet/in.h>

#define NETSTACK_LOG_UNIT "TCP"
#include <netstack/checksum.h>
#include <netstack/ip/route.h>
#include <netstack/tcp/tcp.h>
#include <netstack/tcp/tcpopt.h>
#include <netstack/tcp/retransmission.h>
#include <netstack/time/contimer.h>
#include <netstack/time/util.h>


int tcp_send(struct inet_sock *inet, struct frame *frame, struct neigh_route *rt) {
    struct tcp_hdr *hdr = tcp_hdr(frame);

    frame_lock(frame, SHARED_RD);
    uint16_t pktlen = frame_pkt_len(frame);
    frame_unlock(frame);

    // TODO: Don't assume IPv4 L3, choose based on sock->saddr
    struct inet_ipv4_phdr phdr = {
            .saddr = htonl(inet->locaddr.ipv4),
            .daddr = htonl(inet->remaddr.ipv4),
            .hlen  = htons(pktlen),
            .proto = IP_P_TCP,
            .rsvd = 0
    };

    // Calculate TCP checksum, including IP layer
    // TODO: Don't assume IPv4 pseudo-header for checksumming
    uint16_t ph_csum = in_csum(&phdr, sizeof(phdr), 0);
    hdr->csum = in_csum(hdr, pktlen, ~ph_csum);

    frame_incref(frame);

    // TODO: Implement functionality to specify IP flags (different for IP4/6?)
    // TODO: Send socket flags to neigh_send_to() in tcp_send()
    int ret = neigh_send_to(rt, frame, IP_P_TCP, 0, inet->flags);

    frame_decref(frame);
    return ret;
}

int tcp_send_empty(struct tcp_sock *sock, uint32_t seqn, uint32_t ackn,
                   uint8_t flags) {

    // Find route to next-hop
    int err;
    struct neigh_route route = {
            .intf = sock->inet.intf,
            .saddr = sock->inet.locaddr,
            .daddr = sock->inet.remaddr
    };
    if ((err = neigh_find_route(&route)))
        return err;

    struct intf *intf = route.intf;
    struct frame *seg = intf_frame_new(intf, intf_max_frame_size(intf));

    // Send 0 datalen for empty packet
    long count = tcp_init_header(seg, sock, htonl(seqn), htonl(ackn), flags, 0);
    // < 0 indicates error
    if (count < 0)
        return (int) count;

    int ret = tcp_send(&sock->inet, seg, &route);

    frame_decref(seg);

    return ret;
}

int tcp_send_data(struct tcp_sock *sock, uint32_t seqn, size_t len) {

    int err;
    uint16_t count;

    // Ensure the socket isn't closed before each packet send
    tcp_sock_lock(sock);
    if (sock->state != TCP_ESTABLISHED && sock->state != TCP_CLOSE_WAIT) {
        tcp_sock_unlock(sock);
        // TODO: Return socket close reason to user
        return -ECONNRESET;
    }
    tcp_sock_unlock(sock);

    // Find route to next-hop
    struct neigh_route route = {
            .intf = sock->inet.intf,
            .saddr = sock->inet.locaddr,
            .daddr = sock->inet.remaddr
    };
    if ((err = neigh_find_route(&route)))
        return err;

    struct tcb *tcb = &sock->tcb;
    struct intf *intf = route.intf;
    // Initialise a new frame to carry outgoing segment
    struct frame *seg = intf_frame_new(intf, intf_max_frame_size(intf));

    tcp_sock_lock(sock);

    // Get the maximum available bytes to send
    long tosend = seqbuf_available(&sock->sndbuf, seqn);
    if (len > 0)
        // Bound payload size by requested length
        tosend = MIN(tosend, len);

    uint8_t flags = TCP_FLAG_ACK;
    uint32_t ackn = htonl(tcb->rcv.nxt);
    size_t datalen = (size_t) tosend;
    err = tcp_init_header(seg, sock, htonl(seqn), ackn, flags, datalen);
    if (err < 0)
        // < 0 indicates error
        return err;

    // Set the PUSH flag if we're sending the last data in the buffer
    count = (uint16_t) err;
    if (count >= tosend)
        tcp_hdr(seg)->flags.psh = 1;

    // Start the retransmission timeout
    if (sock->unacked.length == 0) {

        // TODO: Use sock->rtt for retransmission timeout
        struct timespec to = { 0, mstons(200) };
        struct tcp_rto_data rtd = { .sock = sock, .seq = tcb->snd.nxt, .len = count };

        // Hold another reference to the socket to prevent it being free'd
        tcp_sock_incref(sock);

        LOG(LVERB, "starting rtimer for sock %p (%u, %i)", sock, rtd.seq, count);
        sock->rto_event = contimer_queue_rel(&sock->rtimer, &to, &rtd, sizeof(rtd));
    }

    // Only push seg_data if segment is in-order (not retransmitting)
    if (seqn == tcb->snd.nxt) {

        LOG(LTRCE, "adding seq_data %u to unacked queue", seqn);

        // Store sequence information for the rto
        struct tcp_seq_data *seg_data = malloc(sizeof(struct tcp_seq_data));
        seg_data->seq = tcb->snd.nxt;
        seg_data->len = (uint16_t) count;

        // Log unsent/unacked segment data for potential later retransmission
        pthread_mutex_lock(&sock->unacked.lock);
        llist_append_nolock(&sock->unacked, seg_data);
        pthread_mutex_unlock(&sock->unacked.lock);

        // Advance SND.NXT past this segment
        tcb->snd.nxt += count;
    }

    // Read data from the send buffer into the segment payload
    long readerr = seqbuf_read(&sock->sndbuf, seqn, seg->data, (size_t) count);
    if (readerr < 0) {
        LOGSE(LERR, "seqbuf_read (%li)", -readerr, readerr);
        tcp_sock_unlock(sock);
        return (int) readerr;
    } else if (readerr == 0) {
        LOG(LWARN, "No data to send");
        tcp_sock_unlock(sock);
        return -1;
    }

    tcp_sock_unlock(sock);
    frame_unlock(seg);

    // Send to neigh, passing IP options
    int ret = tcp_send(&sock->inet, seg, &route);
    
    frame_decref(seg);

    return (ret < 0 ? ret : count);
}

int tcp_init_header(struct frame *seg, struct tcp_sock *sock, uint32_t seqn,
                     uint32_t ackn, uint8_t flags, size_t datalen) {

    // Obtain TCP options + hdrlen
    // Maximum of 40 bytes of options
    uint8_t tcp_optdat[40];
    // optdat doesn't need to be zero'ed as the final 4 bytes are cleared later
    size_t tcp_optsum = tcp_options(sock, flags, tcp_optdat);
    size_t tcp_optlen = (tcp_optsum + 3) & -4;    // Round to multiple of 4

    // Obtain IP options + hdrlen
    // TODO: Calculate IP layer options in tcp_send_data()
    size_t ip_optlen = 0;

    // TODO: Take into account ethernet header variations, such as VLAN tags

    // Find the largest possible segment payload with headers taken into account
    // then clamp the value to at most the requested payload size
    // https://tools.ietf.org/html/rfc793#section-3.7
    // (see https://tools.ietf.org/html/rfc879 for details)
    size_t count = MIN((sock->mss - tcp_optlen - ip_optlen), datalen);

    // Allocate TCP payload and header space, including options
    size_t hdrlen = sizeof(struct tcp_hdr) + tcp_optlen;
    frame_data_alloc(seg, count);
    struct tcp_hdr *hdr = frame_head_alloc(seg, hdrlen);

    seg->intf = sock->inet.intf;

    // Set connection values in header
    hdr->flagval = flags;
    hdr->seqn = seqn;
    hdr->ackn = ackn;
    hdr->sport = htons(sock->inet.locport);
    hdr->dport = htons(sock->inet.remport);
    // Zero out some constant values
    hdr->rsvd = 0;
    hdr->csum = 0;
    hdr->urg_ptr = 0;
    hdr->hlen = (uint8_t) (hdrlen >> 2);     // hdrlen / 4
    hdr->wind = htons(sock->tcb.rcv.wnd);

    // Copy options
    uint8_t *optptr = (seg->head + sizeof(struct tcp_hdr));
    // Zero last 4 bytes for padding
    *((uint32_t *) (optptr - 4)) = 0;
    memcpy(optptr, tcp_optdat, tcp_optsum);

    return (int) count;
}

size_t tcp_options(struct tcp_sock *sock, uint8_t tcp_flags, uint8_t *opt) {
    // Track the start pointer
    uint8_t *optstart = opt;

    // Only send MSS option in SYN flags
    uint16_t mss = tcp_mss_ipv4(sock->inet.intf);
    if ((tcp_flags & TCP_FLAG_SYN) && (mss != TCP_DEF_MSS)) {
        // Maximum Segment Size option is 1+1+2 bytes:
        // https://tools.ietf.org/html/rfc793#page-19
        LOG(LDBUG, "MSS option enabled with mss = %u", mss);

        *opt++ = TCP_OPT_MSS;
        *opt++ = TCP_OPT_MSS_LEN;
        *((uint16_t *) opt) = htons(mss);   // MSS value
        opt += 2;
    }

    // Length of options is delta
    uint64_t len = opt - optstart;
    LOG(LVERB, "options length %lu", len);

    return len;
}
