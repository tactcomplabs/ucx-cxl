#include "cxl_ep.h"
#include "cxl_iface.h"

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

// NOTE: overloaded return type; return >=0 signifies successful, number of bytes copied. 
// Negative return values are ucs_status_t values
ssize_t uct_cxl_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                            void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
  return UCS_ERR_UNSUPPORTED;
}

ucs_status_t uct_cxl_ep_get_bcopy(uct_ep_h tl_ep, uct_unpack_callback_t unpack_cb,
                                 void *arg, size_t length,
                                 uint64_t remote_addr, uct_rkey_t rkey,
                                 uct_completion_t *comp)
{
  return UCS_ERR_UNSUPPORTED;
}

ucs_status_t uct_cxl_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
  return UCS_ERR_UNSUPPORTED;
}

ssize_t uct_cxl_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
  return UCS_ERR_UNSUPPORTED;
}
