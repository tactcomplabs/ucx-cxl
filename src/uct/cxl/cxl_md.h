#ifndef UCT_CXL_MD_H_
#define UCT_CXL_MD_H_

#include <ucs/sys/compiler_def.h>
#include <uct/base/uct_md.h>
#include <ucs/config/types.h>
#include <ucs/datastruct/list.h>
#include <ucs/type/spinlock.h>
#include <ucs/type/status.h>
#include <ucs/sys/ptr_arith.h>
#include <stdint.h>
#include <stdbool.h>

/* 
 * Alignment to be used for entire aperature window. Equivalent to UCS_SYS_CACHE_LINE_SIZE
 * for x86/RV64 platforms, but hardcode here to make offsets arch-agnostic
*/
#define UCT_CXL_ALIGN 64

/*
 * The memdev is split into nslots equal slots, one per UCX process (MD) on the
 * owning guest. Both guests must use the same nslots and aperture size, which
 * is_reachable checks through the peer's control block. Within a slot:
 *
 *   [0, CTRL_SIZE)                 control block
 *   [FIFO_OFFSET, RMA_OFFSET)      MAX_SLOTS receive rings; ring r is written
 *                                  only by slot r of the peer guest
 *   [RMA_OFFSET, slot_size)        RMA region, handed out by mem_alloc
 *
 * The in-slot layout is fixed at compile time; only the slot size depends on
 * the aperture size and nslots.
 */
#define UCT_CXL_MAX_SLOTS 8

#define UCT_CXL_CTRL_OFFSET 0 /* from the start of a slot */
#define UCT_CXL_CTRL_SIZE   ucs_align_up(sizeof(uct_cxl_ctrl_t), UCT_CXL_ALIGN)

/* FIFO parameters, offsets measured from start of a slot */
#define UCT_CXL_FIFO_ELEM_SIZE 256
#define UCT_CXL_FIFO_DEPTH     64
#define UCT_CXL_FIFO_OFFSET    (UCT_CXL_CTRL_OFFSET + UCT_CXL_CTRL_SIZE)
#define UCT_CXL_FIFO_SIZE      (UCT_CXL_FIFO_ELEM_SIZE * UCT_CXL_FIFO_DEPTH)

/* Offset of the RMA region from the start of a slot */
#define UCT_CXL_RMA_OFFSET                                                     \
    (UCT_CXL_FIFO_OFFSET + UCT_CXL_MAX_SLOTS * UCT_CXL_FIFO_SIZE)

/* Suggested to have a larger (more distinct) number and OR in version number
 * for compatability checking, but too much for a PoC currently */
#define UCT_CXL_MAGIC 1234ULL

/* Placeholder, subject to change */
#define UCT_CXL_RMA_MAX_BCOPY 8192
#define UCT_CXL_RMA_MAX_SHORT 256
#define UCT_CXL_RMA_MAX_IOV   16

typedef struct uct_cxl_packed_rkey {
    uint64_t seg_offset; /* From the start of the owner's memdev */
    uint64_t seg_addr; /* Owner's VA of seg_offset, used in arithmetic only */
    uint64_t length;
} UCS_S_PACKED uct_cxl_packed_rkey_t;

/* Unpacked rkey, uct_rkey_t is a pointer to it. rkey_unpack gets no MD, so it
 * can't hold the local aperture_base; put/get add that themselves. */
typedef struct uct_cxl_rkey {
    uint64_t seg_offset;
    uint64_t seg_addr;
    uint64_t length;
} uct_cxl_rkey_t;


/* A range of the local RMA region. Returned by mem_alloc as the memh. If
 * linked on uct_cxl_md_t::free_list, it describes a free range instead. */
typedef struct uct_cxl_seg {
    ucs_list_link_t list; /* Entry in free_list, unused while allocated */
    uint64_t offset; /* From the start of the memdev */
    size_t length;
} uct_cxl_seg_t;


/* Written by the owner at init, except ack */
typedef struct uct_cxl_ctrl {
    uint64_t magic; /* written last at init, cleared at close */
    uint64_t iface_id; /* peer reads in is_reachable */
    uint64_t nslots; /* peer checks it matches its own */
    uint64_t slot_size; /* peer checks it matches its own */
    /* Slot r of the PEER writes ack[r][i] = seq it consumed from element i of
     * the ring the OWNER sends to it */
    uint64_t ack[UCT_CXL_MAX_SLOTS][UCT_CXL_FIFO_DEPTH];
} uct_cxl_ctrl_t;

typedef struct uct_cxl_fifo_elem {
    uint64_t seq; /* written last; valid iff == consumer index + 1 */
    uint32_t length;
    uint8_t am_id;
    uint8_t reserved[3]; /* padding (unused) */
    uint8_t payload[UCT_CXL_FIFO_ELEM_SIZE - 16];
} UCS_S_PACKED uct_cxl_fifo_elem_t;

typedef struct uct_cxl_md {
    uct_md_t super;
    void *aperture_base; /* mmap'd VA of the CXL BAR */
    void *local_base; /* mmap'd VA of the local CXL BAR */
    int aperture_fd;
    int local_fd; /* Holds the OFD lock on byte 'slot' of the local device */
    size_t aperture_size; /* discovered/configured window size */
    unsigned nslots; /* number of slots each memdev is split into */
    unsigned slot; /* slot of the local memdev this MD owns */
    size_t slot_size; /* bytes per slot, a multiple of UCT_CXL_ALIGN */
    uint64_t node_id; /* identifies this guest; same-node peers are
                         unreachable */
    ucs_spinlock_t lock; /* Protects free_list */
    /* Free uct_cxl_seg_t ranges of the RMA region, sorted by offset, with no
     * two adjacent ranges (consolidated on free)*/
    ucs_list_link_t free_list;
    bool iface_in_use;
} uct_cxl_md_t;

typedef struct uct_cxl_md_config {
    uct_md_config_t super;
    /* used to mmap, requests for peer */
    char *accel_dev_path;
    /* used to mmap, reading local backing mem (peer's writes) */
    char *local_dev_path;
    /* size of the HDM-decoded window exposed by accel_dev_path */
    size_t aperture_size;
    /* number of slots each memdev is split into */
    unsigned long nslots;
    /* slot to own, or UCS_ULUNITS_AUTO for the first free one */
    unsigned long slot;
    /* node identifier, or UCS_ULUNITS_AUTO for the machine GUID */
    unsigned long node_id;
} uct_cxl_md_config_t;

extern uct_component_t uct_cxl_component;

/* Offset of slot 'slot' from the start of a memdev */
static UCS_F_ALWAYS_INLINE uint64_t
uct_cxl_md_slot_offset(const uct_cxl_md_t *md, unsigned slot)
{
    return (uint64_t)slot * md->slot_size;
}

/* Control block of slot 'slot' of the memdev mapped at 'base' */
static UCS_F_ALWAYS_INLINE uct_cxl_ctrl_t *
uct_cxl_md_ctrl(const uct_cxl_md_t *md, void *base, unsigned slot)
{
    return (uct_cxl_ctrl_t *)UCS_PTR_BYTE_OFFSET(
            base, uct_cxl_md_slot_offset(md, slot) + UCT_CXL_CTRL_OFFSET);
}

/* Ring that slot 'sender' of the other guest writes, in slot 'slot' of the
 * memdev mapped at 'base' */
static UCS_F_ALWAYS_INLINE uct_cxl_fifo_elem_t *
uct_cxl_md_get_ring(const uct_cxl_md_t *md, void *base, unsigned slot,
                    unsigned sender)
{
    return (uct_cxl_fifo_elem_t *)UCS_PTR_BYTE_OFFSET(
            base, uct_cxl_md_slot_offset(md, slot) + UCT_CXL_FIFO_OFFSET +
                          sender * UCT_CXL_FIFO_SIZE);
}

/* Whether [offset, offset + length) lies within the RMA region of one slot */
static UCS_F_ALWAYS_INLINE bool uct_cxl_md_is_rma_range(const uct_cxl_md_t *md,
                                                        uint64_t global_offset,
                                                        uint64_t length)
{
    uint64_t slot_offset;

    if (global_offset >= (uint64_t)md->nslots * md->slot_size) {
        return false;
    }

    slot_offset = global_offset % md->slot_size;
    return (slot_offset >= UCT_CXL_RMA_OFFSET) &&
           (length <= md->slot_size - slot_offset);
}

#endif
