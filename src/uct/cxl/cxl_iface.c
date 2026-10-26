#include <ucs/sys/sys.h>
#include "cxl_iface.h"
#include "cxl_ep.h"
#include "cxl_md.h"
#include "cxl_io.h"
#include <string.h>

static ucs_config_field_t uct_cxl_iface_config_table[] = {
        {"", "", NULL, ucs_offsetof(uct_cxl_iface_config_t, super),
         UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

        {NULL}};

ucs_status_t
uct_cxl_iface_query_tl_devices(uct_md_h md,
                               uct_tl_device_resource_t **tl_devices_p,
                               unsigned *num_tl_devices_p)
{
    return uct_single_device_resource(md, "cxl0", UCT_DEVICE_TYPE_SHM,
                                      UCS_SYS_DEVICE_ID_UNKNOWN, tl_devices_p,
                                      num_tl_devices_p);
}

// busy-poll fifo_ctl->tail's slot seq, dispatch via
// uct_iface_invoke_am(), advance tail. This is the direct analog of
// mm_iface.c's FIFO poll loop
static unsigned uct_cxl_iface_progress(uct_iface_h iface)
{
    return 0;
}

static ucs_status_t
uct_cxl_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_iface, uct_cxl_iface_t);

    uct_base_iface_query(&iface->super, iface_attr);
    iface_attr->cap.flags = UCT_IFACE_FLAG_PUT_BCOPY |
                            UCT_IFACE_FLAG_GET_BCOPY | UCT_IFACE_FLAG_AM_SHORT |
                            UCT_IFACE_FLAG_AM_BCOPY | UCT_IFACE_FLAG_PENDING |
                            UCT_IFACE_FLAG_CB_SYNC |
                            UCT_IFACE_FLAG_CONNECT_TO_IFACE;
    iface_attr->iface_addr_len    = sizeof(uct_cxl_iface_addr_t);
    iface_attr->device_addr_len   = 0;
    iface_attr->ep_addr_len       = 0;
    iface_attr->max_conn_priv     = 0;
    iface_attr->cap.put.max_bcopy = UCT_CXL_RMA_MAX_BCOPY;
    iface_attr->cap.get.max_bcopy = UCT_CXL_RMA_MAX_BCOPY;
    /* am_short carries its 8-byte header inside the payload */
    iface_attr->cap.am.max_short = ucs_field_sizeof(uct_cxl_fifo_elem_t,
                                                    payload) -
                                   sizeof(uint64_t);
    iface_attr->cap.am.max_bcopy = ucs_field_sizeof(uct_cxl_fifo_elem_t,
                                                    payload);
    iface_attr->cap.am.max_iov   = 1;

    iface_attr->latency             = UCT_CXL_IFACE_LATENCY;
    iface_attr->bandwidth.dedicated = UCT_CXL_IFACE_BANDWIDTH_DEDICATED;
    iface_attr->bandwidth.shared    = UCT_CXL_IFACE_BANDWIDTH_SHARED;
    iface_attr->overhead            = UCT_CXL_IFACE_OVERHEAD;
    iface_attr->priority            = 0;
    return UCS_OK;
}

static int
uct_cxl_iface_is_reachable_v2(const uct_iface_h tl_iface,
                              const uct_iface_is_reachable_params_t *params)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_iface, uct_cxl_iface_t);
    uint64_t remote_id;

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    if (params->iface_addr == NULL) {
        uct_iface_fill_info_str_buf(params, "iface address is empty");
        return 0;
    }

    memcpy(&remote_id, params->iface_addr, sizeof(remote_id));
    if (remote_id == iface->id) {
        uct_iface_fill_info_str_buf(params, "loopback is not supported");
        return 0;
    }

    /* All cheap checks have passed, subsequent checks go over socket */
    if (uct_cxl_io_read64(&iface->remote_ctrl->magic) != UCT_CXL_MAGIC) {
        uct_iface_fill_info_str_buf(params, "peer iface is not initialized");
        return 0;
    }

    if (uct_cxl_io_read64(&iface->remote_ctrl->iface_id) != remote_id) {
        uct_iface_fill_info_str_buf(params, "address is not the linked peer");
        return 0;
    }

    return 1;
}

static ucs_status_t
uct_cxl_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *addr)
{
    uct_cxl_iface_t *iface = ucs_derived_of(tl_iface, uct_cxl_iface_t);
    memcpy(addr, &iface->id, sizeof(iface->id));
    return UCS_OK;
}

static UCS_CLASS_DECLARE_DELETE_FUNC(uct_cxl_iface_t, uct_iface_t);

static uct_iface_internal_ops_t uct_cxl_iface_internal_ops = {
        .iface_query_v2      = uct_iface_base_query_v2,
        .iface_estimate_perf = uct_base_iface_estimate_perf,
        .iface_vfs_refresh   = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
        .ep_query = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
        .ep_invalidate = (uct_ep_invalidate_func_t)
                ucs_empty_function_return_unsupported,
        .ep_connect_to_ep_v2 = (uct_ep_connect_to_ep_v2_func_t)
                ucs_empty_function_return_unsupported,
        .iface_is_reachable_v2 = uct_cxl_iface_is_reachable_v2,
        .ep_is_connected       = uct_base_ep_is_connected,
        .ep_get_device_ep      = (uct_ep_get_device_ep_func_t)
                ucs_empty_function_return_unsupported};

static uct_iface_ops_t uct_cxl_iface_ops = {
        .ep_put_short = (uct_ep_put_short_func_t)
                ucs_empty_function_return_unsupported,
        // TODO: am_short, am_bcopy are fully needed. Implement!
        .ep_put_bcopy    = uct_cxl_ep_put_bcopy,
        .ep_get_bcopy    = uct_cxl_ep_get_bcopy,
        .ep_am_short     = uct_cxl_ep_am_short,
        .ep_am_short_iov = (uct_ep_am_short_iov_func_t)
                ucs_empty_function_return_unsupported,
        .ep_am_bcopy       = uct_cxl_ep_am_bcopy,
        .ep_atomic_cswap64 = (uct_ep_atomic_cswap64_func_t)
                ucs_empty_function_return_unsupported,
        .ep_atomic64_post = (uct_ep_atomic64_post_func_t)
                ucs_empty_function_return_unsupported,
        .ep_atomic64_fetch = (uct_ep_atomic64_fetch_func_t)
                ucs_empty_function_return_unsupported,
        .ep_atomic_cswap32 = (uct_ep_atomic_cswap32_func_t)
                ucs_empty_function_return_unsupported,
        .ep_atomic32_post = (uct_ep_atomic32_post_func_t)
                ucs_empty_function_return_unsupported,
        .ep_atomic32_fetch = (uct_ep_atomic32_fetch_func_t)
                ucs_empty_function_return_unsupported,
        .ep_flush = uct_base_ep_flush,
        .ep_fence = uct_base_ep_fence,
        .ep_check = (uct_ep_check_func_t)ucs_empty_function_return_success,
        .ep_pending_add = (uct_ep_pending_add_func_t)
                ucs_empty_function_return_busy,
        .ep_pending_purge = (uct_ep_pending_purge_func_t)ucs_empty_function,
        .ep_create        = UCS_CLASS_NEW_FUNC_NAME(uct_cxl_ep_t),
        .ep_destroy       = UCS_CLASS_DELETE_FUNC_NAME(uct_cxl_ep_t),
        .iface_flush      = uct_base_iface_flush,
        .iface_fence      = uct_base_iface_fence,
        .iface_progress_enable    = uct_base_iface_progress_enable,
        .iface_progress_disable   = uct_base_iface_progress_disable,
        .iface_progress           = uct_cxl_iface_progress,
        .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_cxl_iface_t),
        .iface_query              = uct_cxl_iface_query,
        .iface_get_device_address = (uct_iface_get_device_address_func_t)
                ucs_empty_function_return_success,
        .iface_get_address  = uct_cxl_iface_get_address,
        .iface_is_reachable = uct_base_iface_is_reachable};

static UCS_CLASS_INIT_FUNC(uct_cxl_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_cxl_md_t *cxl_md = ucs_derived_of(md, uct_cxl_md_t);
    int in_use;
    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_cxl_iface_ops,
                              &uct_cxl_iface_internal_ops, md, worker, params,
                              tl_config UCS_STATS_ARG(
                                      (params->field_mask &
                                       UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                                              params->stats_root :
                                              NULL) UCS_STATS_ARG("cxl"));

    ucs_spin_lock(&cxl_md->lock);
    in_use               = cxl_md->iface_in_use;
    cxl_md->iface_in_use = 1;
    ucs_spin_unlock(&cxl_md->lock);
    if (in_use) {
        ucs_error("cxl: md %p already has an open interface, only one is "
                  "supported per process",
                  cxl_md);
        return UCS_ERR_BUSY;
    }

    /* pack_cb/unpack_cb must not touch the aperture, so stage through here */
    self->rma_bounce = ucs_malloc(UCT_CXL_RMA_MAX_BCOPY, "uct_cxl_rma_bounce");
    if (self->rma_bounce == NULL) {
        ucs_error("cxl: failed to allocate RMA bounce buffer");
        ucs_spin_lock(&cxl_md->lock);
        cxl_md->iface_in_use = 0;
        ucs_spin_unlock(&cxl_md->lock);
        return UCS_ERR_NO_MEMORY;
    }

    self->md          = cxl_md;
    self->tx.fifo     = UCS_PTR_BYTE_OFFSET(cxl_md->aperture_base,
                                            UCT_CXL_FIFO_OFFSET);
    self->rx.fifo     = UCS_PTR_BYTE_OFFSET(cxl_md->local_base,
                                            UCT_CXL_FIFO_OFFSET);
    self->local_ctrl  = UCS_PTR_BYTE_OFFSET(cxl_md->local_base,
                                            UCT_CXL_CTRL_OFFSET);
    self->remote_ctrl = UCS_PTR_BYTE_OFFSET(cxl_md->aperture_base,
                                            UCT_CXL_CTRL_OFFSET);
    self->tx.head     = 0;
    ucs_queue_head_init(&self->tx.pending);
    self->rx.tail = 0;

    // Generate unique id for the iface, so it can be correctly
    // identified and loopback is avoided. Avoid troublesome edge
    // cases
    do {
        self->id = ucs_generate_uuid((uintptr_t)self);
    } while (self->id == 0 || self->id == UINT64_MAX);

    // zero local control block and fifo
    memset(self->local_ctrl, 0, UCT_CXL_CTRL_SIZE);
    memset(self->rx.fifo, 0, UCT_CXL_FIFO_SIZE);

    uct_cxl_fence();

    uct_cxl_io_write64(&self->local_ctrl->iface_id, self->id);

    uct_cxl_fence();

    // For PoC, leave simple. Later could OR in a version number
    // to ensure compatible versions
    uct_cxl_io_write64(&self->local_ctrl->magic, UCT_CXL_MAGIC);

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_cxl_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    ucs_assert(ucs_queue_is_empty(&self->tx.pending));
    // Zero magic so iface will no longer be seen as valid
    uct_cxl_io_write64(&self->local_ctrl->magic, 0);
    uct_cxl_fence();

    ucs_free(self->rma_bounce);

    ucs_spin_lock(&self->md->lock);
    self->md->iface_in_use = 0;
    ucs_spin_unlock(&self->md->lock);
}

UCS_CLASS_DEFINE(uct_cxl_iface_t, uct_base_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_cxl_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t *,
                                 const uct_iface_config_t *);

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_cxl_iface_t, uct_iface_t);

UCT_TL_DEFINE_ENTRY(&uct_cxl_component, cxl, uct_cxl_iface_query_tl_devices,
                    uct_cxl_iface_t, "CXL_", uct_cxl_iface_config_table,
                    uct_cxl_iface_config_t);

UCT_SINGLE_TL_INIT(&uct_cxl_component, cxl, ctor, , )
