#include <uct/base/uct_component.h>
#include <uct/sm/base/sm_md.h>
#include "cxl_md.h"

// configuration fields go here such as device paths
ucs_config_field_t uct_cxl_md_config_table[] = { {NULL} };

ucs_status_t uct_cxl_md_open(uct_component_t *component, const char *md_name,
                             const uct_md_config_t *config, uct_md_h *md_p)
{
  /* opens the device node, mmap()s it with uncached/device attributes, fills 
  * in aperture_base/aperture_size, sets rma_next_offset = UCT_CXL_RMA_OFFSET
  */
  // TODO: REALLY needs to be implemented; this will only allow linking, not any device usage
  return UCS_ERR_UNSUPPORTED;
}

// Below functions are currently replaced ucs_empty_function_return_unsupported,
// though they may be need to be implemented beyond linking
/*
void uct_cxl_mem_alloc() {}
void uct_cxl_mem_free() {}

void uct_cxl_mem_reg() {}
void uct_cxl_mem_dereg() {}
*/

ucs_status_t
uct_cxl_mkey_pack(uct_md_h md, uct_mem_h memh, void *address, size_t length, const uct_md_mkey_pack_params_t *params, void *mkey_buffer) 
{
  //TODO: needs to be supported for our plan, but get everything linking first
  return UCS_ERR_UNSUPPORTED;
}

static ucs_status_t
uct_cxl_rkey_unpack(uct_component_t *component, const void *rkey_buffer,
                        const uct_rkey_unpack_params_t *params,
                        uct_rkey_t *rkey_p, void **handle_p)
{
  // TODO: implement
  return UCS_ERR_UNSUPPORTED;
}

static ucs_status_t uct_cxl_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    uct_md_base_md_query(attr);
    attr->reg_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    return UCS_OK;
}


uct_md_ops_t uct_cxl_md_ops = {
    .close              = (uct_md_close_func_t)ucs_empty_function,
    .query              = uct_cxl_md_query,        /* needs a real minimal body, see below */
    .mem_alloc          = (uct_md_mem_alloc_func_t)ucs_empty_function_return_unsupported,
    .mem_free           = (uct_md_mem_free_func_t)ucs_empty_function_return_unsupported,
    .mem_advise         = (uct_md_mem_advise_func_t)ucs_empty_function_return_unsupported,
    .mem_reg            = uct_md_dummy_mem_reg,     /* existing helper, same as self/posix */
    .mem_dereg          = uct_md_dummy_mem_dereg,   /* existing helper */
    .mem_query          = (uct_md_mem_query_func_t)ucs_empty_function_return_unsupported,
    .mkey_pack          = uct_cxl_mkey_pack,        /* needs a real minimal body, see below */
    .mem_attach         = (uct_md_mem_attach_func_t)ucs_empty_function_return_unsupported,
    .detect_memory_type = (uct_md_detect_memory_type_func_t)ucs_empty_function_return_unsupported
};

static uct_component_t uct_cxl_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_cxl_md_open,
    .cm_open            = (uct_component_cm_open_func_t)ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_cxl_rkey_unpack,
    .rkey_ptr           = uct_sm_rkey_ptr,       /* reuse from uct/sm/base */
    .rkey_release       = (uct_component_rkey_release_func_t)ucs_empty_function_return_success,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = "cxl",
    .md_config          = {
        .name   = "CXL memory domain",
        .prefix = "CXL_",
        .table  = uct_cxl_md_config_table,
        .size   = sizeof(uct_cxl_md_config_t),
    },
    .cm_config   = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
    .tl_list     = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_cxl_component),
    .flags       = UCT_COMPONENT_FLAG_RKEY_PTR,
    .md_vfs_init = (uct_component_md_vfs_init_func_t)ucs_empty_function
};

UCT_TL_DEFINE(&uct_cxl_component, cxl, uct_cxl_iface_query_tl_devices,
              uct_cxl_iface_t, "CXL_", uct_cxl_iface_config_table,
              uct_cxl_iface_config_t);

UCT_SINGLE_TL_INIT(&uct_cxl_component, cxl, ctor, , )
