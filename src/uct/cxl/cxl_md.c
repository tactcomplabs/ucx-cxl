#include <uct/base/uct_component.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/compiler_def.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include <inttypes.h>
#include "cxl_md.h"

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

/* Get RMA region size, potentially rounding down to satisfy alignment requirements */
static size_t uct_cxl_md_rma_size(size_t aperture_size)
{
    UCS_STATIC_ASSERT((UCT_CXL_RMA_OFFSET % UCT_CXL_ALIGN) == 0);
    return ucs_align_down_pow2(aperture_size, UCT_CXL_ALIGN) -
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

    length = ucs_align_up_pow2(*length_p, UCT_CXL_ALIGN);

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

/* Packs [address, address + length), which must lie within the memh */
static ucs_status_t
uct_cxl_mkey_pack(uct_md_h tl_md, uct_mem_h memh, void *address, size_t length,
                  const uct_md_mkey_pack_params_t *params, void *mkey_buffer)
{
    uct_cxl_md_t *md                   = ucs_derived_of(tl_md, uct_cxl_md_t);
    uct_cxl_seg_t *seg                 = memh;
    uct_cxl_packed_rkey_t *packed_rkey = mkey_buffer;
    void *seg_base = UCS_PTR_BYTE_OFFSET(md->local_base, seg->offset);
    size_t offset  = UCS_PTR_BYTE_DIFF(seg_base, address);

    /* Is this segment inside the RMA range (i.e. from uct_cxl_alloc)? */
    ucs_assertv((seg->offset >= UCT_CXL_RMA_OFFSET) &&
                        (seg->offset + seg->length <= md->aperture_size),
                "seg=[0x%" PRIx64 ", 0x%" PRIx64 ")", seg->offset,
                seg->offset + seg->length);

    /* [address, address + length] lies within [seg_base, seg_base + seg->length] */
    ucs_assertv((address >= seg_base) && (offset <= seg->length) &&
                        (length <= seg->length - offset),
                "address=%p length=%zu seg=%p length=%zu", address, length,
                seg_base, seg->length);

    packed_rkey->seg_offset = seg->offset + offset;
    packed_rkey->seg_addr   = (uintptr_t)address;
    packed_rkey->length     = length;
    return UCS_OK;
}

static ucs_status_t uct_cxl_rkey_unpack(uct_component_t *component,
                                        const void *rkey_buffer,
                                        const uct_rkey_unpack_params_t *params,
                                        uct_rkey_t *rkey_p, void **handle_p)
{
    /* rkey_buffer may be unaligned; the packed struct is read bytewise */
    const uct_cxl_packed_rkey_t *packed_rkey = rkey_buffer;
    uct_cxl_rkey_t *rkey;

    /* The aperture bound is checked by put/get, which have the MD */
    if ((packed_rkey->seg_offset < UCT_CXL_RMA_OFFSET) ||
        (packed_rkey->length > UINT64_MAX - packed_rkey->seg_offset)) {
        ucs_error("cxl: invalid rkey: offset 0x%" PRIx64 " length %" PRIu64,
                  packed_rkey->seg_offset, packed_rkey->length);
        return UCS_ERR_INVALID_PARAM;
    }

    rkey = ucs_malloc(sizeof(*rkey), "uct_cxl_rkey");
    if (rkey == NULL) {
        ucs_error("cxl: failed to allocate rkey");
        return UCS_ERR_NO_MEMORY;
    }

    rkey->seg_offset = packed_rkey->seg_offset;
    rkey->seg_addr   = packed_rkey->seg_addr;
    rkey->length     = packed_rkey->length;

    *rkey_p   = (uintptr_t)rkey;
    *handle_p = NULL;
    return UCS_OK;
}

static ucs_status_t
uct_cxl_rkey_release(uct_component_t *component, uct_rkey_t rkey, void *handle)
{
    ucs_free((void *)rkey);
    return UCS_OK;
}

/* Each unpack allocates, so compare what the rkeys describe, not pointers */
static ucs_status_t
uct_cxl_rkey_compare(uct_component_t *component, uct_rkey_t rkey1,
                     uct_rkey_t rkey2, const uct_rkey_compare_params_t *params,
                     int *result)
{
    const uct_cxl_rkey_t *r1 = (const uct_cxl_rkey_t *)rkey1;
    const uct_cxl_rkey_t *r2 = (const uct_cxl_rkey_t *)rkey2;

    if ((params->field_mask != 0) || (result == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    *result = memcmp(r1, r2, sizeof(*r1));
    return UCS_OK;
}

static ucs_status_t uct_cxl_md_query(uct_md_h tl_md, uct_md_attr_v2_t *attr)
{
    uct_cxl_md_t *md = ucs_derived_of(tl_md, uct_cxl_md_t);
    uct_md_base_md_query(attr);

    /* No UCT_MD_FLAG_REG: the peer can only reach the local RMA region, so
     * remote-accessible memory must come from mem_alloc */
    attr->flags            = UCT_MD_FLAG_ALLOC | UCT_MD_FLAG_NEED_RKEY;
    attr->alloc_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->max_alloc        = uct_cxl_md_rma_size(md->aperture_size);
    attr->max_reg          = 0;
    attr->rkey_packed_size = sizeof(uct_cxl_packed_rkey_t);

    return UCS_OK;
}

static void uct_cxl_md_unmap_device(void *address, size_t length, int fd)
{
    munmap(address, length);
    close(fd);
}

/* With 'exclusive', take a non-blocking exclusive flock() on the device. It is
 * held for as long as fd stays open, so the caller must keep fd open for the
 * lifetime of the mapping. */
static ucs_status_t uct_cxl_md_map_device(const char *file_name, size_t length,
                                          bool exclusive, void **address_p,
                                          int *fd_p)
{
    ucs_status_t status;
    void *address;
    int fd;

    fd = open(file_name, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ucs_debug("cxl: failed to open %s: %m", file_name);
        return UCS_ERR_NO_DEVICE;
    }

    if (exclusive && (flock(fd, LOCK_EX | LOCK_NB) != 0)) {
        if (errno == EWOULDBLOCK) {
            /* UCP skips an MD that fails to open without logging, so say why
             * the transport disappeared */
            ucs_error("cxl: %s is in use by another UCX memory domain (in "
                      "this or another process); only one may own it",
                      file_name);
            status = UCS_ERR_BUSY;
        } else {
            ucs_error("cxl: failed to lock %s: %m", file_name);
            status = UCS_ERR_IO_ERROR;
        }
        goto err_close;
    }

    /* The driver installs uncached/device PTEs, so accesses are not cached or
     * merged (and on the peer window, every access traps to the peer) */
    address = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        ucs_error("cxl: failed to mmap %zu bytes of %s: %m", length, file_name);
        status = UCS_ERR_IO_ERROR;
        goto err_close;
    }

    *address_p = address;
    *fd_p      = fd;
    return UCS_OK;

err_close:
    close(fd);
    return status;
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
    uct_cxl_md_unmap_device(md->local_base, md->aperture_size, md->local_fd);
    uct_cxl_md_unmap_device(md->aperture_base, md->aperture_size,
                            md->aperture_fd);
    ucs_free(md);
}

uct_md_ops_t uct_cxl_md_ops = {
        .close = uct_cxl_md_close,
        .query = uct_cxl_md_query, /* needs a real minimal body, see below */
        .mem_alloc  = uct_cxl_mem_alloc,
        .mem_free   = uct_cxl_mem_free,
        .mem_advise = (uct_md_mem_advise_func_t)
                ucs_empty_function_return_unsupported,
        .mem_reg = (uct_md_mem_reg_func_t)ucs_empty_function_return_unsupported,
        .mem_dereg = (uct_md_mem_dereg_func_t)
                ucs_empty_function_return_unsupported,
        .mem_query = (uct_md_mem_query_func_t)
                ucs_empty_function_return_unsupported,
        .mkey_pack  = uct_cxl_mkey_pack,
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
    uct_cxl_seg_t *range;
    uct_cxl_md_t *md;
    ucs_status_t status;

    /* Unrelated to opening, but UCS_STATIC_ASSERT needs function scope */
    UCS_STATIC_ASSERT(sizeof(uct_cxl_fifo_elem_t) == UCT_CXL_FIFO_ELEM_SIZE);
    UCS_STATIC_ASSERT(ucs_offsetof(uct_cxl_fifo_elem_t, seq) == 0);
    UCS_STATIC_ASSERT((ucs_offsetof(uct_cxl_fifo_elem_t, payload) % 8) == 0);
    UCS_STATIC_ASSERT((ucs_offsetof(uct_cxl_ctrl_t, ack) % 8) == 0);
    UCS_STATIC_ASSERT((UCT_CXL_FIFO_OFFSET % UCT_CXL_ALIGN) == 0);

    if (config->aperture_size < UCT_CXL_RMA_OFFSET + UCT_CXL_ALIGN) {
        ucs_error("cxl: aperture size %zu is too small for the control block, "
                  "FIFO and RMA region (need at least %zu bytes)",
                  config->aperture_size,
                  (size_t)(UCT_CXL_RMA_OFFSET + UCT_CXL_ALIGN));
        return UCS_ERR_INVALID_PARAM;
    }

    md = ucs_calloc(1, sizeof(*md), "uct_cxl_md");
    if (md == NULL) {
        ucs_error("cxl: failed to allocate memory domain");
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_cxl_md_map_device(config->accel_dev_path,
                                   config->aperture_size, false,
                                   &md->aperture_base, &md->aperture_fd);
    if (status != UCS_OK) {
        goto err_free_md;
    }

    /* The free list assumes it is the only allocator of the local memdev */
    status = uct_cxl_md_map_device(config->local_dev_path,
                                   config->aperture_size, true, &md->local_base,
                                   &md->local_fd);
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
    uct_cxl_md_unmap_device(md->local_base, config->aperture_size,
                            md->local_fd);
err_unmap_aperture:
    uct_cxl_md_unmap_device(md->aperture_base, config->aperture_size,
                            md->aperture_fd);
err_free_md:
    ucs_free(md);
    return status;
}

uct_component_t uct_cxl_component = {
        .query_md_resources = uct_md_query_single_md_resource,
        .md_open            = uct_cxl_md_open,
        .cm_open            = (uct_component_cm_open_func_t)
                ucs_empty_function_return_unsupported,
        .rkey_unpack = uct_cxl_rkey_unpack,
        .rkey_ptr    = (uct_component_rkey_ptr_func_t)
                ucs_empty_function_return_unsupported,
        .rkey_release = uct_cxl_rkey_release,
        .rkey_compare = uct_cxl_rkey_compare,
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
        .flags       = 0,
        .md_vfs_init = (uct_component_md_vfs_init_func_t)ucs_empty_function};
