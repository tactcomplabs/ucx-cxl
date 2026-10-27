#include "cxl_ep.h"
#include "cxl_iface.h"
#include "cxl_md.h"
#include "cxl_io.h"
#include <inttypes.h>

static UCS_CLASS_INIT_FUNC(uct_cxl_ep_t, const uct_ep_params_t *params)
{
    uct_cxl_iface_t *iface = ucs_derived_of(params->iface, uct_cxl_iface_t);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_cxl_ep_t)
{
    uct_cxl_iface_t *iface = ucs_derived_of(self->super.super.iface,
                                            uct_cxl_iface_t);
    uct_cxl_pending_req_priv_t *priv;

    /* The user must purge the EP's pending requests before destroying it */
    ucs_queue_for_each(priv, &iface->tx.pending, base.queue_elem) {
        ucs_assertv(priv->ep != self, "ep %p destroyed with a pending request",
                    self);
    }
}

UCS_CLASS_DEFINE(uct_cxl_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_cxl_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_cxl_ep_t, uct_ep_t);

ucs_status_t
uct_cxl_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req, unsigned flags)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);

    if (uct_cxl_iface_can_send(iface)) {
        return UCS_ERR_BUSY; /* resources available, caller should retry */
    }

    ((uct_cxl_pending_req_priv_t *)&req->priv)->ep =
            ucs_derived_of(tl_ep, uct_cxl_ep_t);
    uct_pending_req_queue_push(&iface->tx.pending, req);
    return UCS_OK;
}

void uct_cxl_ep_pending_purge(uct_ep_h tl_ep, uct_pending_purge_callback_t cb,
                              void *arg)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);
    uct_cxl_ep_t *ep       = ucs_derived_of(tl_ep, uct_cxl_ep_t);
    uct_cxl_pending_req_priv_t *priv;

    uct_pending_queue_purge(priv, &iface->tx.pending, priv->ep == ep, cb, arg);
}

static void uct_cxl_ep_fifo_push(uct_cxl_iface_t *iface, uint8_t am_id,
                                 const void *data, uint32_t length)
{
    uct_cxl_fifo_elem_t *elem =
            &iface->tx.fifo[iface->tx.head % UCT_CXL_FIFO_DEPTH];
    /* length (low 32 bits) and am_id share the 8-byte word after seq */
    uint64_t meta = length | ((uint64_t)am_id << 32);

    uct_cxl_fence(); /* F1: ack read and app's prior stores before the slot */
    uct_cxl_io_write(elem->payload, data, length);
    uct_cxl_io_write64(UCS_PTR_BYTE_OFFSET(elem,
                                           ucs_offsetof(uct_cxl_fifo_elem_t,
                                                        length)),
                       meta);
    uct_cxl_fence(); /* F2: element contents before seq */
    uct_cxl_io_write64(elem, iface->tx.head + 1); /* seq is at offset 0 */
    ++iface->tx.head;
}

ucs_status_t uct_cxl_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                 const void *payload, unsigned length)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);
    uint8_t buf[UCT_CXL_FIFO_PAYLOAD_SIZE];

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(sizeof(header) + length, 0, UCT_CXL_FIFO_PAYLOAD_SIZE,
                     "am_short");
    if (!uct_cxl_iface_can_send(iface)) {
        return UCS_ERR_NO_RESOURCE;
    }

    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), payload, length);
    uct_cxl_ep_fifo_push(iface, id, buf, sizeof(header) + length);
    return UCS_OK;
}

ssize_t uct_cxl_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                            uct_pack_callback_t pack_cb, void *arg,
                            unsigned flags)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);
    uint8_t buf[UCT_CXL_FIFO_PAYLOAD_SIZE];
    size_t length;

    UCT_CHECK_AM_ID(id);
    if (!uct_cxl_iface_can_send(iface)) {
        return UCS_ERR_NO_RESOURCE;
    }

    length = pack_cb(buf, arg);
    ucs_assertv(length <= UCT_CXL_FIFO_PAYLOAD_SIZE, "length=%zu", length);
    uct_cxl_ep_fifo_push(iface, id, buf, length);
    return length;
}

/* NOTE: overloaded return type; return >=0 signifies successful, number of bytes copied.
 * Negative return values are ucs_status_t values */
/* Translate [remote_addr, remote_addr + length) of the peer's segment described
 * by rkey into a pointer into the local aperture. The range must lie within
 * the segment and the peer's RMA region. */
static ucs_status_t uct_cxl_ep_rma_ptr(uct_cxl_iface_t *iface,
                                       uint64_t remote_addr, uct_rkey_t rkey,
                                       size_t length, void **mmio_p)
{
    const uct_cxl_rkey_t *cxl_rkey = (const uct_cxl_rkey_t *)rkey;
    uint64_t seg_delta             = remote_addr - cxl_rkey->seg_addr;
    uint64_t offset;

    /* seg_delta wraps around if remote_addr is below the segment */
    if ((remote_addr < cxl_rkey->seg_addr) || (seg_delta > cxl_rkey->length) ||
        (length > cxl_rkey->length - seg_delta)) {
        ucs_error("cxl: remote range [0x%" PRIx64 ", +%zu) is outside the "
                  "segment [0x%" PRIx64 ", +%" PRIu64 ")",
                  remote_addr, length, cxl_rkey->seg_addr, cxl_rkey->length);
        return UCS_ERR_INVALID_PARAM;
    }

    /* rkey_unpack checked seg_offset >= UCT_CXL_RMA_OFFSET and that
     * seg_offset + length does not overflow */
    offset = cxl_rkey->seg_offset + seg_delta;
    if (offset + length > iface->md->aperture_size) {
        ucs_error("cxl: remote range [0x%" PRIx64 ", +%zu) is outside the "
                  "%zu byte aperture",
                  offset, length, iface->md->aperture_size);
        return UCS_ERR_INVALID_PARAM;
    }

    *mmio_p = UCS_PTR_BYTE_OFFSET(iface->md->aperture_base, offset);
    return UCS_OK;
}

/* UCP only selects RMA lanes that support put_short. buffer is ordinary host
 * memory, so it is copied straight to the aperture with no bounce. */
ucs_status_t uct_cxl_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                  unsigned length, uint64_t remote_addr,
                                  uct_rkey_t rkey)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);
    ucs_status_t status;
    void *mmio;

    UCT_CHECK_LENGTH(length, 0, UCT_CXL_RMA_MAX_SHORT, "put_short");

    status = uct_cxl_ep_rma_ptr(iface, remote_addr, rkey, length, &mmio);
    if (status != UCS_OK) {
        return status;
    }

    uct_cxl_io_write(mmio, buffer, length);
    ucs_trace_data("PUT_SHORT [size %u] to 0x%" PRIx64, length, remote_addr);
    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, SHORT, length);
    return UCS_OK;
}

// NOTE: overloaded return type; return >=0 signifies successful, number of bytes copied.
// Negative return values are ucs_status_t values
ssize_t uct_cxl_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                             void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);
    ucs_status_t status;
    size_t length;
    void *mmio;

    length = pack_cb(iface->rma_bounce, arg);
    ucs_assertv(length <= UCT_CXL_RMA_MAX_BCOPY, "length=%zu", length);

    status = uct_cxl_ep_rma_ptr(iface, remote_addr, rkey, length, &mmio);
    if (status != UCS_OK) {
        return status;
    }

    /* Each access waits for the peer, so the data has landed on return */
    uct_cxl_io_write(mmio, iface->rma_bounce, length);
    ucs_trace_data("PUT_BCOPY [size %zu] to 0x%" PRIx64, length, remote_addr);
    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, BCOPY, length);
    return length;
}

ucs_status_t uct_cxl_ep_get_bcopy(uct_ep_h tl_ep,
                                  uct_unpack_callback_t unpack_cb, void *arg,
                                  size_t length, uint64_t remote_addr,
                                  uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);
    ucs_status_t status;
    void *mmio;

    UCT_CHECK_LENGTH(length, 0, UCT_CXL_RMA_MAX_BCOPY, "get_bcopy");

    status = uct_cxl_ep_rma_ptr(iface, remote_addr, rkey, length, &mmio);
    if (status != UCS_OK) {
        return status;
    }

    /* Synchronous like put, so comp is never used */
    uct_cxl_io_read(iface->rma_bounce, mmio, length);
    unpack_cb(arg, iface->rma_bounce, length);
    ucs_trace_data("GET_BCOPY [length %zu] from 0x%" PRIx64, length,
                   remote_addr);
    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, BCOPY, length);
    return UCS_OK;
}
