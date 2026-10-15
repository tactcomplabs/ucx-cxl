#ifndef UCT_CXL_EP_H_
#define UCT_CXL_EP_H_

#include <uct/base/uct_iface.h>
#include <uct/base/uct_md.h>

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

#endif
