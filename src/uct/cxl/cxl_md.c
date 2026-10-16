#include <uct/base/uct_component.h>
#include <uct/sm/base/sm_md.h>
#include "cxl_md.h"
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/compiler_def.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static ucs_config_field_t uct_cxl_md_config_table[] = {
        {"", "", NULL, ucs_offsetof(uct_cxl_md_config_t, super),
         UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

        {"ACCEL_DEV", "/dev/cxl_accel0",
         "Character device whose mapping forwards accesses to the peer",
         ucs_offsetof(uct_cxl_md_config_t, accel_dev_path),
         UCS_CONFIG_TYPE_STRING},

        {"LOCAL_DEV", "/dev/cxl_accel_local0",
         "Character device whose mapping exposes local backing memory, i.e. "
         "what the peer writes into",
         ucs_offsetof(uct_cxl_md_config_t, local_dev_path),
         UCS_CONFIG_TYPE_STRING},

        {"APERTURE_SIZE", "256M",
         "Size of the CXL window; must match the device's HDM region size",
         ucs_offsetof(uct_cxl_md_config_t, aperture_size),
         UCS_CONFIG_TYPE_MEMUNITS},

        {NULL}};

// Below functions are currently replaced ucs_empty_function_return_unsupported,
// though they may be need to be implemented beyond linking
/*
void uct_cxl_mem_alloc() {}
void uct_cxl_mem_free() {}

void uct_cxl_mem_reg() {}
void uct_cxl_mem_dereg() {}
*/

ucs_status_t
uct_cxl_mkey_pack(uct_md_h md, uct_mem_h memh, void *address, size_t length,
                  const uct_md_mkey_pack_params_t *params, void *mkey_buffer)
{
    //TODO: needs to be supported for our plan, but get everything linking first
    return UCS_ERR_UNSUPPORTED;
}

static ucs_status_t uct_cxl_rkey_unpack(uct_component_t *component,
                                        const void *rkey_buffer,
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


static void uct_cxl_md_close(uct_md_h uct_md)
{
    uct_cxl_md_t *md = ucs_derived_of(uct_md, uct_cxl_md_t);

    munmap(md->aperture_base, md->aperture_size);
    ucs_free(md);
}

uct_md_ops_t uct_cxl_md_ops = {
        .close = uct_cxl_md_close,
        .query = uct_cxl_md_query, /* needs a real minimal body, see below */
        .mem_alloc = (uct_md_mem_alloc_func_t)
                ucs_empty_function_return_unsupported,
        .mem_free = (uct_md_mem_free_func_t)
                ucs_empty_function_return_unsupported,
        .mem_advise = (uct_md_mem_advise_func_t)
                ucs_empty_function_return_unsupported,
        .mem_reg =
                uct_md_dummy_mem_reg, /* existing helper, same as self/posix */
        .mem_dereg = uct_md_dummy_mem_dereg, /* existing helper */
        .mem_query = (uct_md_mem_query_func_t)
                ucs_empty_function_return_unsupported,
        .mkey_pack =
                uct_cxl_mkey_pack, /* needs a real minimal body, see below */
        .mem_attach = (uct_md_mem_attach_func_t)
                ucs_empty_function_return_unsupported,
        .detect_memory_type = (uct_md_detect_memory_type_func_t)
                ucs_empty_function_return_unsupported};

static ucs_status_t
uct_cxl_md_open(uct_component_t *component, const char *md_name,
                const uct_md_config_t *uct_config, uct_md_h *md_p)
{
    const uct_cxl_md_config_t *config = ucs_derived_of(uct_config,
                                                       uct_cxl_md_config_t);
    uct_cxl_md_t *md;
    ucs_status_t status;
    void *base;
    int fd;

    if (config->aperture_size <= UCT_CXL_RMA_OFFSET) {
        ucs_error("cxl: aperture size %zu is too small for the control block "
                  "and FIFO (need more than %zu bytes)",
                  config->aperture_size, (size_t)UCT_CXL_RMA_OFFSET);
        return UCS_ERR_INVALID_PARAM;
    }

    md = ucs_calloc(1, sizeof(*md), "uct_cxl_md");
    if (md == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    fd = open(config->accel_dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ucs_debug("cxl: failed to open %s: %m", config->accel_dev_path);
        status = UCS_ERR_NO_DEVICE;
        goto err_free;
    }

    /* The driver installs uncached/device PTEs, so every access traps to the
     * peer. The mapping stays valid after the descriptor is closed. */
    base = mmap(NULL, config->aperture_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        ucs_error("cxl: failed to mmap %zu bytes of %s: %m",
                  config->aperture_size, config->accel_dev_path);
        status = UCS_ERR_IO_ERROR;
        goto err_free;
    }

    md->super.ops       = &uct_cxl_md_ops;
    md->super.component = component;
    md->aperture_base   = base;
    md->aperture_size   = config->aperture_size;
    md->rma_next_offset = UCT_CXL_RMA_OFFSET;

    *md_p = &md->super;
    return UCS_OK;

err_free:
    ucs_free(md);
    return status;
}

uct_component_t uct_cxl_component = {
        .query_md_resources = uct_md_query_single_md_resource,
        .md_open            = uct_cxl_md_open,
        .cm_open            = (uct_component_cm_open_func_t)
                ucs_empty_function_return_unsupported,
        .rkey_unpack  = uct_cxl_rkey_unpack,
        .rkey_ptr     = uct_sm_rkey_ptr, /* reuse from uct/sm/base */
        .rkey_release = (uct_component_rkey_release_func_t)
                ucs_empty_function_return_success,
        .rkey_compare = uct_base_rkey_compare,
        .name         = "cxl",
        .md_config =
                {
                        .name   = "CXL memory domain",
                        .prefix = "CXL_",
                        .table  = uct_cxl_md_config_table,
                        .size   = sizeof(uct_cxl_md_config_t),
                },
        .cm_config   = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
        .tl_list     = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_cxl_component),
        .flags       = UCT_COMPONENT_FLAG_RKEY_PTR,
        .md_vfs_init = (uct_component_md_vfs_init_func_t)ucs_empty_function};
