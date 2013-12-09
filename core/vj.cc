/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <unordered_map>

#include <osv/trace.hh>
#include <osv/poll.h>
#include <osv/types.h>
#include <vj.hh>

#include <bsd/sys/sys/mbuf.h>
#include <bsd/sys/sys/param.h>
#include <bsd/sys/sys/socketvar.h>
#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/netinet/in.h>
#include <bsd/sys/netinet/tcp.h>
#include <bsd/sys/netinet/ip.h>

TRACEPOINT(trace_vj_classifier_cls_add, "(%d,%d,%d,%d,%d)->%p", in_addr_t, in_addr_t, u8, u16, u16, vj::vj_ring_type*);
TRACEPOINT(trace_vj_classifier_cls_remove, "(%d,%d,%d,%d,%d)", in_addr_t, in_addr_t, u8, u16, u16);
TRACEPOINT(trace_vj_classifier_cls_lookup_found, "(%d,%d,%d,%d,%d)", in_addr_t, in_addr_t, u8, u16, u16);
TRACEPOINT(trace_vj_classifier_cls_lookup_not_found, "(%d,%d,%d,%d,%d)", in_addr_t, in_addr_t, u8, u16, u16);
TRACEPOINT(trace_vj_classifier_packet_delivered, "%p", struct mbuf*);
TRACEPOINT(trace_vj_classifier_poll_wake, "%p", struct mbuf*);
TRACEPOINT(trace_vj_classifier_packet_not_delivered, "%p -> %d", struct mbuf*, int);
TRACEPOINT(trace_vj_classifier_packet_not_delivered_not_tcp, "%p, protocol=%d, len=%d", struct mbuf*, int, int);
TRACEPOINT(trace_vj_classifier_packet_dropped, "");
TRACEPOINT(trace_vj_classifier_packet_popped, "%p", struct mbuf*);
TRACEPOINT(trace_vj_classifier_waiting, "");
TRACEPOINT(trace_vj_classifier_done_waiting, "");

using namespace vj;

static vj_ringbuf ringbuf_to_c(vj_ring_type* ring)
{
    return reinterpret_cast<vj_ringbuf>(ring);
}

static vj_ring_type* ringbuf_from_c(vj_ringbuf ring)
{
    return reinterpret_cast<vj_ring_type*>(ring);
}

static vj::classifier* classifier_from_c(vj_classifier cls)
{
    return reinterpret_cast<vj::classifier*>(cls);
}

namespace vj {

classifier::classifier()
{

}

classifier::~classifier()
{

}

void classifier::add(struct in_addr src_ip, struct in_addr dst_ip,
    u8 ip_proto, u16 src_port, u16 dst_port, vj_ring_type* ring)
{
    trace_vj_classifier_cls_add(src_ip.s_addr, dst_ip.s_addr,
        ip_proto, src_port, dst_port, ring);

    classifer_control_msg * cmsg = new classifer_control_msg();
    cmsg->next = nullptr;
    cmsg->ht.src_ip = src_ip;
    cmsg->ht.dst_ip = dst_ip;
    cmsg->ht.ip_proto = ip_proto;
    cmsg->ht.src_port = src_port;
    cmsg->ht.dst_port = dst_port;
    cmsg->ring = ring;
    cmsg->type = classifer_control_msg::ADD;

    _cls_control.push(cmsg);
}


void classifier::remove(struct in_addr src_ip, struct in_addr dst_ip,
    u8 ip_proto, u16 src_port, u16 dst_port)
{
    trace_vj_classifier_cls_remove(src_ip.s_addr, dst_ip.s_addr,
        ip_proto, src_port, dst_port);

    classifer_control_msg * cmsg = new classifer_control_msg();
    cmsg->next = nullptr;
    cmsg->ht.src_ip = src_ip;
    cmsg->ht.dst_ip = dst_ip;
    cmsg->ht.ip_proto = ip_proto;
    cmsg->ht.src_port = src_port;
    cmsg->ht.dst_port = dst_port;
    cmsg->type = classifer_control_msg::REMOVE;

    _cls_control.push(cmsg);
}

void classifier::process_control(void)
{
    struct classifer_control_msg * item;
    while ((item = _cls_control.pop())) {
        switch (item->type) {
        case classifer_control_msg::ADD:
            _classifications.insert(std::make_pair(item->ht, item->ring));
            break;
        case classifer_control_msg::REMOVE:
            _classifications.erase(item->ht);
            break;
        default:
            debug("vj: unknown classification\n");
        }
        delete item;
    }
}

vj_ring_type* classifier::lookup(struct in_addr src_ip, struct in_addr dst_ip,
    u8 ip_proto, u16 src_port, u16 dst_port)
{
    vj_hashed_tuple ht;
    ht.src_ip = src_ip;
    ht.dst_ip = dst_ip;
    ht.ip_proto = ip_proto;
    ht.src_port = src_port;
    ht.dst_port = dst_port;

    auto it = _classifications.find(ht);
    if (it == _classifications.end()) {
        trace_vj_classifier_cls_lookup_not_found(src_ip.s_addr,
            dst_ip.s_addr, ip_proto, src_port, dst_port);
        return nullptr;
    }

    trace_vj_classifier_cls_lookup_found(src_ip.s_addr, dst_ip.s_addr,
        ip_proto, src_port, dst_port);
    return (it->second);
}

bool classifier::try_deliver(struct mbuf* m)
{
    struct in_addr src_ip;
    struct in_addr dst_ip;
    u8 ip_proto;
    u16 src_port;
    u16 dst_port;

    // Test packet length
    if (m->m_hdr.mh_len < (int)(ETHER_HDR_LEN + sizeof(struct ip))) {
        trace_vj_classifier_packet_not_delivered(m, 1);
        return false;
    }

    // Basic decode
    u8* pkt = mtod(m, u8*);
    struct ip* ip = reinterpret_cast<struct ip*>(pkt + ETHER_HDR_LEN);
    u8 hlen = ip->ip_hl << 2;
    src_ip = ip->ip_src;
    dst_ip = ip->ip_dst;
    ip_proto = ip->ip_p;

    // Make sure it's a TCP packet and that it has space to read the
    // TCP header
    if ((ip_proto != IPPROTO_TCP) ||
        (m->m_hdr.mh_len < (int)(ETHER_HDR_LEN + hlen + sizeof(struct tcphdr)))) {
        trace_vj_classifier_packet_not_delivered_not_tcp(m, ip_proto, m->m_hdr.mh_len);
        return false;
    }

    // Process control messages
    process_control();

    struct tcphdr* tcp = reinterpret_cast<struct tcphdr*>(pkt + ETHER_HDR_LEN + hlen);
    src_port = tcp->th_sport;
    dst_port = tcp->th_dport;

    auto ring = lookup(dst_ip, src_ip, ip_proto, dst_port, src_port);
    if (!ring) {
//            uint8_t* packet = mtod(m, uint8_t*);
//            puts("Packet lookup failed:\n");
//            hexdump(packet, m->m_len);
//            puts("----\n");
        trace_vj_classifier_packet_not_delivered(m, 3);
        return false;
    }

    bool rc = ring->push(m);
    if (!rc) {
        trace_vj_classifier_packet_dropped();
        m_free(m);
        return true;
    }

    trace_vj_classifier_packet_delivered(m);

    // Wake up user in case it is waiting
    ring->wake_consumer();

    return true;
}

} // namespace vj


vj_ringbuf vj_ringbuf_create()
{
    return ringbuf_to_c(new vj_ring_type());
}

struct mbuf* vj_ringbuf_pop(vj_ringbuf ringbuf)
{
    vj_ring_type* ring = ringbuf_from_c(ringbuf);
    struct mbuf* result = nullptr;

    ring->pop(result);
    trace_vj_classifier_packet_popped(result);

    return (result);
}

void vj_ringbuf_destroy(vj_ringbuf ringbuf)
{
    vj_ring_type* ring = ringbuf_from_c(ringbuf);
    delete ring;
}


void vj_wait(vj_ringbuf ringbuf)
{
    vj_ring_type* ring = ringbuf_from_c(ringbuf);
    trace_vj_classifier_waiting();
    ring->wait_for_items();
    trace_vj_classifier_done_waiting();
}

//////////////////////////////////////////////////////////////////////////////


void vj_classify_remove(vj_classifier cls, struct in_addr laddr, struct in_addr faddr,
    u_char ip_p, u_int16_t lport, u_int16_t fport)
{
    classifier_from_c(cls)->remove(laddr, faddr, ip_p, lport, fport);
}

void vj_classify_add(vj_classifier cls,
    struct in_addr laddr, struct in_addr faddr,
    u_char ip_p, u_int16_t lport, u_int16_t fport, struct socket* so)
{
    vj::classifier* obj = classifier_from_c(cls);
    if (obj)
        obj->add(laddr, faddr, ip_p, lport, fport, so->so_rcv.sb_ring);
}

int vj_try_deliver(vj_classifier cls, struct mbuf* m)
{
    vj::classifier* obj = classifier_from_c(cls);
    assert(obj != nullptr);
    if (obj)
        return (obj->try_deliver(m) ? 1 : 0);

    return 0;
}



