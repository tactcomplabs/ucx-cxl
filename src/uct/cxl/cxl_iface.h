#ifndef CXL_IFACE_H_
#define CXL_IFACE_H_

#include <uct/api/uct.h>
#include <uct/base/uct_iface.h>
#include <stdint.h>
#include <stdbool.h>
#include "cxl_md.h"
#include "cxl_io.h"

#define UCT_CXL_IFACE_LATENCY             ucs_linear_func_make(10e-6, 0)
#define UCT_CXL_IFACE_BANDWIDTH_DEDICATED 10e6
#define UCT_CXL_IFACE_BANDWIDTH_SHARED    0
#define UCT_CXL_IFACE_OVERHEAD            1e-6

#define UCT_CXL_FIFO_HDR_SIZE ucs_offsetof(uct_cxl_fifo_elem_t, payload)
#define UCT_CXL_FIFO_PAYLOAD_SIZE                                              \
    (UCT_CXL_FIFO_ELEM_SIZE - UCT_CXL_FIFO_HDR_SIZE)

typedef struct uct_cxl_iface_addr {
    uint64_t id; /* uct_cxl_iface_t::id */
    uint64_t slot; /* slot of its memdev the iface owns */
} UCS_S_PACKED uct_cxl_iface_addr_t;

/* The device address is the node id, see uct_cxl_md_t::node_id */
typedef uint64_t uct_cxl_device_addr_t;

typedef struct uct_cxl_iface {
    uct_base_iface_t super;
    uct_cxl_md_t *md;
    uint64_t id; /* unique iface identifier */
    uct_cxl_ctrl_t *local_ctrl; /* own control in RAM */
    void *rma_bounce; /* UCT_CXL_RMA_MAX_BCOPY bytes, put/get staging */
    /* Indexed by the peer slot sent to. Each peer slot gets one stream from
     * this iface, shared by all EPs to it. */
    struct {
        uct_cxl_fifo_elem_t *fifo; /* peer's RX ring for us, via aperture */
        uint64_t head; /* next sequence index to send */
        ucs_queue_head_t pending;
        bool dispatching; /* set while draining the pending queue */
    } tx[UCT_CXL_MAX_SLOTS];
    /* Indexed by the peer slot received from */
    struct {
        uct_cxl_fifo_elem_t *fifo; /* own RX ring in local RAM */
        uct_cxl_ctrl_t *peer_ctrl; /* sender's control, via aperture */
        uint64_t tail; /* next sequence index to consume */
    } rx[UCT_CXL_MAX_SLOTS];
    unsigned rx_next; /* ring progress polls first, for fairness */
} uct_cxl_iface_t;

typedef struct uct_cxl_iface_config {
    uct_iface_config_t super;
} uct_cxl_iface_config_t;

ucs_status_t
uct_cxl_iface_query_tl_devices(uct_md_h md,
                               uct_tl_device_resource_t **tl_devices_p,
                               unsigned *num_tl_devices_p);

ucs_status_t uct_cxl_iface_fence(uct_iface_h tl_iface, unsigned flags);
ucs_status_t uct_cxl_iface_flush(uct_iface_h tl_iface, unsigned flags,
                                 uct_completion_t *comp);

/* Element head % DEPTH of the ring to peer slot 'slot' is free on the first
 * lap, or once the peer acked the sequence number that last occupied it */
static UCS_F_ALWAYS_INLINE int
uct_cxl_iface_tx_slot_free(uct_cxl_iface_t *iface, unsigned slot)
{
    uint64_t head = iface->tx[slot].head;

    return (head < UCT_CXL_FIFO_DEPTH) ||
           (uct_cxl_io_read64(
                    &iface->local_ctrl->ack[slot][head % UCT_CXL_FIFO_DEPTH]) ==
            head + 1 - UCT_CXL_FIFO_DEPTH);
}

/* New sends must not overtake queued pending requests */
static UCS_F_ALWAYS_INLINE int
uct_cxl_iface_can_send(uct_cxl_iface_t *iface, unsigned slot)
{
    return (iface->tx[slot].dispatching ||
            ucs_queue_is_empty(&iface->tx[slot].pending)) &&
           uct_cxl_iface_tx_slot_free(iface, slot);
}

#endif
