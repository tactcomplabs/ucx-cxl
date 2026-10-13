
#include "cxl_iface.h"

// are these arg types correct? want super md or this one?
ucs_status_t
uct_cxl_iface_query_tl_devices(uct_md_h md, uct_tl_device_resource_t **tl_devices_p,
                          unsigned *num_tl_devices_p)
{

}

// TODO: any other parameters? Needs md pointer?
// computes iface->fifo_ctl = md->aperture_base + UCT_CXL_CTRL_OFFSET, zeroes head/tail on the owning side
static UCS_CLASS_INIT_FUNC(uct_cxl_iface_t, uct_md_h md, uct_worker_h worker,
                            const uct_iface_params_t *params,
                            const uct_iface_config_t *tl_config)
{
    uct_cxl_iface_t *iface = ucs_derived_of(params->iface, uct_cxl_iface_t);

    // do we want some UCS_STATS_ARG in this? What args would we want here?
    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_cxl_iface_ops, &uct_cxl_iface_internal_ops, md, worker, params, tl_config);
    return UCS_OK;

}

static UCS_CLASS_CLEANUP_FUNC(uct_cxl_iface_t)
{

}

// TODO: below functions need to be implemented and called?
UCS_CLASS_DEFINE(uct_cxl_iface_t, uct_base_iface_t);

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_cxl_iface_t, uct_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_cxl_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t*);

// busy-poll fifo_ctl->tail's slot owner bit, dispatch via uct_iface_invoke_am(),
// advance tail. This is the direct analog of mm_iface.c's FIFO poll loop
static unsigned
uct_cxl_iface_progress(uct_iface_h iface)
{
  return 0;
}

// TODO: fills uct_iface_attr_t with the capability flags (PUT/GET bcopy, AM short/bcopy; leave zcopy and atomics unset)
static ucs_status_t
uct_cxl_iface_query(uct_iface_h iface, uct_iface_attr_t *iface_attr)
{
  return UCS_OK;
}

static int
uct_cxl_iface_is_reachable_v2(const uct_iface_h tl_iface,
                              const uct_iface_is_reachable_params_t *params)
{
    const uct_cxl_iface_t *iface = ucs_derived_of(tl_iface, uct_cxl_iface_t);
    const uct_cxl_iface_addr_t *addr;

    // seems to be the correct return for reachable interface
    // TODO: implement the actual checking mechanisms after everything
    // is wired up
    return 1;
}

static ucs_status_t uct_cxl_iface_get_address(uct_iface_h tl_iface,
                                              uct_iface_addr_t *addr)
{
    const uct_cxl_iface_t *iface = ucs_derived_of(tl_iface, uct_cxl_iface_t);

    // TODO: iface does not have this field (adapted from sm/self iface).
    // Implement some other way, but returning UCS_OK fine for now
    //*(uct_cxl_iface_addr_t*)addr = iface->id;
    return UCS_OK;
}

static uct_iface_internal_ops_t uct_cxl_iface_internal_ops = {
    .iface_query_v2         = uct_iface_base_query_v2,
    .iface_estimate_perf    = uct_base_iface_estimate_perf,
    .iface_vfs_refresh      = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query               = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate          = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2    = (uct_ep_connect_to_ep_v2_func_t)ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2  = uct_self_iface_is_reachable_v2,
    .ep_is_connected        = uct_base_ep_is_connected,
    .ep_get_device_ep       = (uct_ep_get_device_ep_func_t)ucs_empty_function_return_unsupported
};

static uct_iface_ops_t uct_cxl_iface_ops = {
    .ep_put_short             = (uct_ep_put_short_func_t)ucs_empty_function_return_unsupported,
    // TODO: put_bcopy, get_bcopy, am_short, am_bcopy are fully needed. Implement!
    .ep_put_bcopy             = uct_cxl_ep_put_bcopy,
    .ep_get_bcopy             = uct_cxl_ep_get_bcopy,
    .ep_am_short              = uct_cxl_ep_am_short,
    .ep_am_short_iov          = (uct_ep_am_short_iov_func_t)ucs_empty_function_return_unsupported,
    .ep_am_bcopy              = uct_cxl_ep_am_bcopy,
    .ep_atomic_cswap64        = (uct_ep_atomic_cswap64_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_post         = (uct_ep_atomic64_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch        = (uct_ep_atomic64_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32        = (uct_ep_atomic_cswap32_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = (uct_ep_atomic32_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = (uct_ep_atomic32_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_flush                 = uct_base_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)ucs_empty_function_return_success,
    .ep_pending_add           = (uct_ep_pending_add_func_t)ucs_empty_function_return_busy,
    .ep_pending_purge         = (uct_ep_pending_purge_func_t)ucs_empty_function,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_cxl_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_cxl_ep_t),
    .iface_flush              = uct_base_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = (uct_iface_progress_enable_func_t)ucs_empty_function,
    .iface_progress_disable   = (uct_iface_progress_disable_func_t)ucs_empty_function,
    .iface_progress           = (uct_iface_progress_func_t)ucs_empty_function_return_zero,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_cxl_iface_t),
    //.iface_query              = uct_cxl_iface_query,
    .iface_query              = (uct_iface_query_func_t)ucs_empty_function_return_unsupported,
    .iface_get_device_address = (uct_iface_get_device_address_func_t)ucs_empty_function_return_success,
    .iface_get_address        = uct_cxl_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};
