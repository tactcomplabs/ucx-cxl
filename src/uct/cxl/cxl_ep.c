#include "cxl_ep.h"
#include "cxl_iface.h"
#include "cxl_md.h"

static UCS_CLASS_INIT_FUNC(uct_cxl_ep_t, const uct_ep_params_t *params)
{
    uct_cxl_iface_t *iface = ucs_derived_of(params->iface, uct_cxl_iface_t);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_cxl_ep_t)
{
}

UCS_CLASS_DEFINE(uct_cxl_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_cxl_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_cxl_ep_t, uct_ep_t);

static void uct_cxl_ep_fifo_push(uct_cxl_iface_t *iface, uint8_t am_id,
                                 const void *data, uint32_t length)
{
    uct_cxl_fifo_elem_t *elem = &iface->tx.fifo[iface->tx.head %
                                                UCT_CXL_FIFO_DEPTH];
    /* length (low 32 bits) and am_id share the 8-byte word after seq */
    uint64_t meta = length | ((uint64_t)am_id << 32);

    uct_cxl_fence(); /* F1: ack read and app's prior stores before the slot */
    uct_cxl_io_write(elem->payload, data, length);
    uct_cxl_io_write64(UCS_PTR_BYTE_OFFSET(elem, ucs_offsetof(uct_cxl_fifo_elem_t,
                                                              length)), meta);
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
ssize_t uct_cxl_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                             void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
    return UCS_ERR_UNSUPPORTED;
}

ucs_status_t uct_cxl_ep_get_bcopy(uct_ep_h tl_ep,
                                  uct_unpack_callback_t unpack_cb, void *arg,
                                  size_t length, uint64_t remote_addr,
                                  uct_rkey_t rkey, uct_completion_t *comp)
{
    return UCS_ERR_UNSUPPORTED;
}


