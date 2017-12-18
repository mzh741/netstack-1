#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sysexits.h>
#include <errno.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netpacket/packet.h>

#include <linux/if_ether.h>
#include <netstack/eth/ether.h>
#include <netstack/intf/rawsock.h>

int new_rawsock(struct intf *interface) {
    if (interface == NULL) {
        errno = EINVAL;
        return -1;
    }

    // Open a raw socket (raw layer 2/3 frames)
    // Use SOCK_DGRAM to remove ethernet header
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        perror("Error: could not create socket");
        return -1;
    }

    int opt = true;
    int err = setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &opt, sizeof(int));
    if (err < 0) {
        perror("Could not set SO_TIMESTAMPNS on socket");
        close(sock);
        return -1;
    }

    // TODO: This is hacky, assuiming lo is loopback
    // Request first non-loopback interface
    struct if_nameindex *if_ni = NULL,
                        *if_ni_head = if_nameindex();
    if_ni = if_ni_head;
    while (if_ni != NULL && if_ni->if_index != 0) {
        if (strcmp(if_ni->if_name, "lo") != 0) {
            printf("Using interface (#%d) %s", if_ni->if_index,
                   if_ni->if_name);
            break;
        }
        if_ni++;
    }
    if (if_ni == NULL) {
        errno = ENODEV;

        printf("\n");
        if_freenameindex(if_ni_head);
        close(sock);
        return -1;
    }

    // Get chosen interface mac address
    uint8_t *hwaddr = malloc(IFHWADDRLEN);
    struct ifreq req;
    strcpy(req.ifr_name, if_ni->if_name);
    if (ioctl(sock, SIOCGIFHWADDR, &req) == 0) {
        memcpy(hwaddr, req.ifr_addr.sa_data, IFHWADDRLEN);
    } else {
        printf("\n");
        free(hwaddr);
        if_freenameindex(if_ni_head);
        close(sock);
        return -1;
    }

    int mtu;
    if (ioctl(sock, SIOCGIFMTU, &req) == 0) {
        mtu = req.ifr_mtu;
        printf(", mtu %d", mtu);
    } else {
        printf("\n");
        free(hwaddr);
        if_freenameindex(if_ni_head);
        close(sock);
        return -1;
    }

    printf("\n");
    struct intf_rawsock *ll = malloc(sizeof(struct intf_rawsock));
    ll->sock = sock;
    ll->if_index = if_ni->if_index;

    interface->ll = ll;
    interface->ll_addr = hwaddr;
    interface->mtu = mtu;
    interface->type = INTF_RAWSOCK;
    interface->recv_frame = rawsock_recv_frame;
    interface->send_frame = rawsock_send_frame;
    interface->recv_peek = rawsock_peek;
    interface->free = free_rawsock;
    // Newly recv'd frames are of type 'ether'
    interface->input = recv_ether;

    if_freenameindex(if_ni_head);

    return 0;
}

void free_rawsock(struct intf *intf) {
    struct intf_rawsock *sockptr = (struct intf_rawsock *) intf->ll;
    close(sockptr->sock);
    free(sockptr);
    free(intf->ll_addr);
}

ssize_t rawsock_recv_frame(struct intf *interface, struct frame **frame) {
    // Count is raw eth packet size (inc eth + ip + transport)
    ssize_t lookahead = 0,
            count = 0;
    int sock = *((int *) interface->ll);

    // Use peek method in struct, it may have been overridden
    if ((lookahead = interface->recv_peek(interface)) == -1) {
        return lookahead;
    }

    // Allocate a buffer of the correct size
    *frame = init_frame(NULL, (size_t) lookahead);
    struct frame *eth_frame = *frame;

    // Allocate msghdr to receive packet & ancillary data into
    uint8_t ctrl[1024];
    struct msghdr msgh = {0};
    struct iovec iov = {0};
    iov.iov_base = eth_frame->data;
    iov.iov_len = (size_t) lookahead;
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = &ctrl;
    msgh.msg_controllen = 1024;

    // Read network data into the frame
    count = recvmsg(sock, &msgh, 0);

    // There was an error. errno should be set
    if (count == -1) {
        return count;
    }

    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(&msgh);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if ((cmsg->cmsg_level == SOL_SOCKET)
            && (cmsg->cmsg_type == SO_TIMESTAMPNS))
            memcpy(&eth_frame->time, CMSG_DATA(cmsg), sizeof(eth_frame->time));
    }

    // Warn if the sizes don't match (should probably never happen)
    if (count != lookahead) {
        fprintf(stderr, "Warning: MSG_PEEK != recv(): %zi != %zi\n",
                lookahead, count);

        // TODO: Print debug messages for uncommon paths like these

        // realloc a (larger?) new buffer if the size differs, just in case
        void *newbuf;
        if ((newbuf = realloc(eth_frame->buffer, (size_t) count)) == NULL) {
            fprintf(stderr, "Fatal: Failed to reallocate new buffer of "
                    "size %zi bytes\n", count);
            exit(EX_OSERR);
        }
        eth_frame->buffer = newbuf;
    }

    return 0;
}

ssize_t rawsock_send_frame(struct intf *interface, struct frame *frame) {
    // TODO: Move this into a separate thread and use signalling
    struct intf_rawsock *ll = (struct intf_rawsock *) interface->ll;
    struct sockaddr_ll sa;
    sa.sll_family = AF_PACKET;
    sa.sll_ifindex = ll->if_index;
    sa.sll_halen = ETH_ADDR_LEN;
    memcpy(sa.sll_addr, eth_hdr(frame)->daddr, ETH_ADDR_LEN);

    return sendto(ll->sock, frame->buffer, frame->buf_size, 0,
                  (const struct sockaddr *) &sa, sizeof(sa));
}

ssize_t rawsock_peek(struct intf *interface) {
    int sock = *((int *) interface->ll);
    return recv(sock, NULL, 0, (MSG_PEEK | MSG_TRUNC));
}