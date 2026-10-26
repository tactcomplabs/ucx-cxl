#ifndef UCT_CXL_EP_H_
#define UCT_CXL_EP_H_

#include <uct/base/uct_iface.h>
#include <uct/base/uct_md.h>
#include <uct/base/uct_ep.h>
#include "cxl_iface.h"

typedef struct uct_cxl_ep {
    uct_base_ep_t super;
} uct_cxl_ep_t;

UCS_CLASS_DECLARE_NEW_FUNC(uct_cxl_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_cxl_ep_t, uct_ep_t);

ssize_t uct_cxl_ep_put_bcopy(uct_ep_h, uct_pack_callback_t, void *, uint64_t,
                             uct_rkey_t);
ucs_status_t uct_cxl_ep_get_bcopy(uct_ep_h, uct_unpack_callback_t, void *,
                                  size_t, uint64_t, uct_rkey_t,
                                  uct_completion_t *);
ucs_status_t
uct_cxl_ep_am_short(uct_ep_h, uint8_t, uint64_t, const void *, unsigned);
ssize_t
uct_cxl_ep_am_bcopy(uct_ep_h, uint8_t, uct_pack_callback_t, void *, unsigned);

typedef struct {
    uct_pending_req_priv_queue_t base;
    uct_cxl_ep_t                 *ep;
} uct_cxl_pending_req_priv_t;

ucs_status_t uct_cxl_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req,
                                    unsigned flags)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_cxl_iface_t);

    if (uct_cxl_iface_can_send(iface)) {
        return UCS_ERR_BUSY; /* resources available, caller should retry */
    }

    ((uct_cxl_pending_req_priv_t*)&req->priv)->ep =
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

#endif
