#ifndef __VJ_HH__
#define __VJ_HH__

#include <sys/cdefs.h>
#include <osv/types.h>
#include <bsd/sys/sys/mbuf.h>
#include <bsd/sys/netinet/in.h>

// Forward declaration of socket
struct socket;

#ifdef __cplusplus

#include <unordered_map>
#include <functional>
#include <lockfree/ring.hh>

struct vj_hashed_tuple {
public:
    struct in_addr src_ip;
    struct in_addr dst_ip;
    u8 ip_proto;
    u16 src_port;
    u16 dst_port;
};

namespace std {

template<>
struct hash<vj_hashed_tuple> {
    std::size_t operator()(vj_hashed_tuple const& ht) const {
        return ( ht.src_ip.s_addr ^ ht.dst_ip.s_addr ^ ht.ip_proto ^ ht.src_port ^ ht.dst_port );
    }
};

template<>
struct equal_to<vj_hashed_tuple> {
    bool operator() (const vj_hashed_tuple& ht1, const vj_hashed_tuple& ht2) const {
        return ((ht1.src_ip.s_addr == ht2.src_ip.s_addr) &&
                (ht1.dst_ip.s_addr == ht2.dst_ip.s_addr) &&
                (ht1.ip_proto == ht2.ip_proto) &&
                (ht1.src_port == ht2.src_port) &&
                (ht1.dst_port == ht2.dst_port));

    }
};

}

namespace vj {

static constexpr int rcv_ring_size = 1024;

typedef ring_spsc_waiter<struct mbuf*, rcv_ring_size> vj_ring_type;

struct classifer_control_msg {
    enum cls_type {
        ADD,
        REMOVE
    };

    classifer_control_msg *next;
    cls_type type;
    vj_hashed_tuple ht;
    vj_ring_type* ring;
};


//
// Implements lockless packet classification using a hash function
// This class should be interfaced using a single consumer and producer
// And an instance should be created per each interface.
//

class classifier {
public:
    classifier();
    ~classifier();

    void add(struct in_addr src_ip, struct in_addr dst_ip,
        u8 ip_proto, u16 src_port, u16 dst_port, vj_ring_type* ring);

    void remove(struct in_addr src_ip, struct in_addr dst_ip,
        u8 ip_proto, u16 src_port, u16 dst_port);

    // If we have an existing classification, queue this packet on the rx
    // sockbuf processing ring
    bool try_deliver(struct mbuf* m);

private:

    vj_ring_type* lookup(struct in_addr src_ip, struct in_addr dst_ip,
        u8 ip_proto, u16 src_port, u16 dst_port);

    std::unordered_map<vj_hashed_tuple, vj_ring_type*> _classifications;

    // Control messages
    void process_control(void);
    lockfree::queue_mpsc<classifer_control_msg> _cls_control;
};

}

#endif // __cplusplus


__BEGIN_DECLS

#ifdef __cplusplus
typedef vj::vj_ring_type* vj_ringbuf;
typedef vj::classifier* vj_classifier;
#else
typedef struct vj_ringbuf_struct *vj_ringbuf;
typedef struct vj_classifier_struct *vj_classifier;
#endif
//////////////////////////////// Ring Creation /////////////////////////////////

vj_ringbuf vj_ringbuf_create();
struct mbuf* vj_ringbuf_pop(vj_ringbuf ringbuf);
void vj_ringbuf_destroy(vj_ringbuf ringbuf);

void vj_wait(vj_ringbuf ringbuf);

//////////////////////////////// Classification ////////////////////////////////

vj_classifier vj_classifier_create();
void vj_classify_remove(vj_classifier cls, struct in_addr laddr, struct in_addr faddr,
    u_char ip_p, u_int16_t lport, u_int16_t fport);
void vj_classify_add(vj_classifier cls,
    struct in_addr laddr, struct in_addr faddr,
    u_char ip_p, u_int16_t lport, u_int16_t fport, struct socket* sb);

// If packet can be classified, try to deliver a mbuf to the correct sockbuf ring
// for user processing
int vj_try_deliver(vj_classifier cls, struct mbuf*);

__END_DECLS

#endif // !__VJ_HH__
