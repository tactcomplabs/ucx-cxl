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
void uct_cxl_mem_reg() {}
void uct_cxl_mem_dereg() {}
*/

/* Get RMA region size, potentially rounding down to satisfy alignment requirements */
static size_t uct_cxl_md_rma_size(size_t aperture_size)
{
    UCS_STATIC_ASSERT((UCT_CXL_RMA_OFFSET % UCT_CXL_SEG_ALIGN) == 0);
    return ucs_align_down_pow2(aperture_size, UCT_CXL_SEG_ALIGN) -
           UCT_CXL_RMA_OFFSET;
}

static ucs_status_t
uct_cxl_mem_alloc(uct_md_h tl_md, size_t *length_p, void **address_p,
                  ucs_memory_type_t mem_type, ucs_sys_device_t sys_dev,
                  unsigned flags, const char *alloc_name, uct_mem_h *memh_p)
{
    uct_cxl_md_t *md     = ucs_derived_of(tl_md, uct_cxl_md_t);
    uct_cxl_seg_t *found = NULL;
    uct_cxl_seg_t *range, *seg;
    size_t length;

    if (mem_type != UCS_MEMORY_TYPE_HOST) {
        return UCS_ERR_UNSUPPORTED;
    }

    /* Placement within the memdev is chosen by the free list */
    if (flags & UCT_MD_MEM_FLAG_FIXED) {
        return UCS_ERR_UNSUPPORTED;
    }

    if (*length_p == 0) {
        ucs_error("cxl: cannot allocate a zero-length region for %s",
                  alloc_name);
        return UCS_ERR_INVALID_PARAM;
    }

    /* Also guards ucs_align_up_pow2 below against overflow */
    if (*length_p > md->aperture_size) {
        ucs_debug("cxl: %zu bytes for %s exceed the %zu byte aperture",
                  *length_p, alloc_name, md->aperture_size);
        return UCS_ERR_NO_MEMORY;
    }

    length = ucs_align_up_pow2(*length_p, UCT_CXL_SEG_ALIGN);

    /* Allocated before taking the lock, so that running out of host memory is
     * the only failure that is not "the RMA region is full" */
    seg = ucs_malloc(sizeof(*seg), "uct_cxl_seg");
    if (seg == NULL) {
        ucs_error("cxl: failed to allocate segment descriptor for %s",
                  alloc_name);
        return UCS_ERR_NO_MEMORY;
    }

    ucs_spin_lock(&md->lock);

    /* First fit */
    ucs_list_for_each(range, &md->free_list, list) {
        if (range->length >= length) {
            found = range;
            break;
        }
    }

    if (found == NULL) {
        ucs_spin_unlock(&md->lock);
        ucs_free(seg);
        /* Not an error: callers are expected to fall back to another MD */
        ucs_debug("cxl: no free range of %zu bytes for %s", length, alloc_name);
        return UCS_ERR_NO_MEMORY;
    }

    if (found->length == length) {
        /* Exact fit: hand out the free range descriptor itself */
        ucs_list_del(&found->list);
        ucs_spin_unlock(&md->lock);
        ucs_free(seg);
        seg = found;
    } else {
        seg->offset = found->offset;
        seg->length = length;
        found->offset += length;
        found->length -= length;
        ucs_spin_unlock(&md->lock);
    }

    ucs_trace("cxl: allocated [0x%" PRIx64 ", 0x%" PRIx64 ") for %s",
              seg->offset, seg->offset + seg->length, alloc_name);

    *length_p  = seg->length;
    *address_p = UCS_PTR_BYTE_OFFSET(md->local_base, seg->offset);
    *memh_p    = seg;
    return UCS_OK;
}

static ucs_status_t uct_cxl_mem_free(uct_md_h tl_md, uct_mem_h memh)
{
    uct_cxl_md_t *md    = ucs_derived_of(tl_md, uct_cxl_md_t);
    uct_cxl_seg_t *seg  = memh;
    uct_cxl_seg_t *prev = NULL;
    uct_cxl_seg_t *next = NULL;
    uct_cxl_seg_t *range;

    ucs_assertv((seg->offset >= UCT_CXL_RMA_OFFSET) &&
                        (seg->offset + seg->length <= md->aperture_size),
                "seg=[0x%" PRIx64 ", 0x%" PRIx64 ")", seg->offset,
                seg->offset + seg->length);

    ucs_trace("cxl: freeing [0x%" PRIx64 ", 0x%" PRIx64 ")", seg->offset,
              seg->offset + seg->length);

    ucs_spin_lock(&md->lock);

    /* Find the free ranges bookending seg */
    ucs_list_for_each(range, &md->free_list, list) {
        if (range->offset > seg->offset) {
            next = range;
            break;
        }

        prev = range;
    }

    /* Overlap with a free range means a double free or a bogus memh */
    ucs_assertv((prev == NULL) || (prev->offset + prev->length <= seg->offset),
                "seg offset 0x%" PRIx64 " overlaps free range at 0x%" PRIx64,
                seg->offset, prev->offset);
    ucs_assertv((next == NULL) || (seg->offset + seg->length <= next->offset),
                "seg offset 0x%" PRIx64 " overlaps free range at 0x%" PRIx64,
                seg->offset, next->offset);

    if ((prev != NULL) && (prev->offset + prev->length == seg->offset)) {
        prev->length += seg->length;
        ucs_free(seg);
        seg = prev;
    } else if (next != NULL) {
        ucs_list_insert_before(&next->list, &seg->list);
    } else {
        ucs_list_add_tail(&md->free_list, &seg->list);
    }

    if ((next != NULL) && (seg->offset + seg->length == next->offset)) {
        seg->length += next->length;
        ucs_list_del(&next->list);
        ucs_free(next);
    }

    ucs_spin_unlock(&md->lock);
    return UCS_OK;
}

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

/* Deallocate all free memory regions in the RMA range */
static void uct_cxl_md_free_list_cleanup(uct_cxl_md_t *md)
{
    size_t free_bytes = 0;
    uct_cxl_seg_t *range, *tmp;

    ucs_list_for_each_safe(range, tmp, &md->free_list, list) {
        free_bytes += range->length;
        ucs_list_del(&range->list);
        ucs_free(range);
    }

    if (free_bytes != uct_cxl_md_rma_size(md->aperture_size)) {
        ucs_warn("cxl: md %p closed with %zu bytes of RMA region still "
                 "allocated",
                 md, uct_cxl_md_rma_size(md->aperture_size) - free_bytes);
    }
}

static void uct_cxl_md_close(uct_md_h uct_md)
{
    uct_cxl_md_t *md = ucs_derived_of(uct_md, uct_cxl_md_t);

    uct_cxl_md_free_list_cleanup(md);
    ucs_spinlock_destroy(&md->lock);
    munmap(md->local_base, md->aperture_size);
    munmap(md->aperture_base, md->aperture_size);
    ucs_free(md);
}

uct_md_ops_t uct_cxl_md_ops = {
        .close = uct_cxl_md_close,
        .query = uct_cxl_md_query, /* needs a real minimal body, see below */
        .mem_alloc  = uct_cxl_mem_alloc,
        .mem_free   = uct_cxl_mem_free,
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
uct_cxl_md_map_device(const char *file_name, size_t length, void **address_p)
{
    void *address;
    int fd;

    fd = open(file_name, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ucs_debug("cxl: failed to open %s: %m", file_name);
        return UCS_ERR_NO_DEVICE;
    }

    /* The driver installs uncached/device PTEs, so accesses are not cached or
     * merged (and on the peer window, every access traps to the peer). The
     * mapping stays valid after the descriptor is closed. */
    address = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (address == MAP_FAILED) {
        ucs_error("cxl: failed to mmap %zu bytes of %s: %m", length, file_name);
        return UCS_ERR_IO_ERROR;
    }

    *address_p = address;
    return UCS_OK;
}

static ucs_status_t
uct_cxl_md_open(uct_component_t *component, const char *md_name,
                const uct_md_config_t *uct_config, uct_md_h *md_p)
{
    const uct_cxl_md_config_t *config = ucs_derived_of(uct_config,
                                                       uct_cxl_md_config_t);
    uct_cxl_seg_t *range;
    uct_cxl_md_t *md;
    ucs_status_t status;

    if (config->aperture_size < UCT_CXL_RMA_OFFSET + UCT_CXL_SEG_ALIGN) {
        ucs_error("cxl: aperture size %zu is too small for the control block, "
                  "FIFO and RMA region (need at least %zu bytes)",
                  config->aperture_size,
                  (size_t)(UCT_CXL_RMA_OFFSET + UCT_CXL_SEG_ALIGN));
        return UCS_ERR_INVALID_PARAM;
    }

    md = ucs_calloc(1, sizeof(*md), "uct_cxl_md");
    if (md == NULL) {
        ucs_error("cxl: failed to allocate memory domain");
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_cxl_md_map_device(config->accel_dev_path,
                                   config->aperture_size, &md->aperture_base);
    if (status != UCS_OK) {
        goto err_free_md;
    }

    status = uct_cxl_md_map_device(config->local_dev_path,
                                   config->aperture_size, &md->local_base);
    if (status != UCS_OK) {
        goto err_unmap_aperture;
    }

    status = ucs_spinlock_init(&md->lock, 0);
    if (status != UCS_OK) {
        ucs_error("cxl: failed to initialize allocator lock");
        goto err_unmap_local;
    }

    /* Initially the whole RMA region is one free range */
    range = ucs_malloc(sizeof(*range), "uct_cxl_seg");
    if (range == NULL) {
        ucs_error("cxl: failed to allocate RMA region descriptor");
        status = UCS_ERR_NO_MEMORY;
        goto err_destroy_lock;
    }

    range->offset = UCT_CXL_RMA_OFFSET;
    range->length = uct_cxl_md_rma_size(config->aperture_size);
    ucs_list_head_init(&md->free_list);
    ucs_list_add_tail(&md->free_list, &range->list);

    md->super.ops       = &uct_cxl_md_ops;
    md->super.component = component;
    md->aperture_size   = config->aperture_size;

    *md_p = &md->super;
    return UCS_OK;

err_destroy_lock:
    ucs_spinlock_destroy(&md->lock);
err_unmap_local:
    munmap(md->local_base, config->aperture_size);
err_unmap_aperture:
    munmap(md->aperture_base, config->aperture_size);
err_free_md:
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
