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

/* 
 * Alignment to be used for entire aperature window. Equivalent to UCS_SYS_CACHE_LINE_SIZE
 * for x86/RV64 platforms, but hardcode here to make offsets arch-agnostic
*/
#define UCT_CXL_ALIGN 64

/* Fixed partition of the CXL aperture - identical on both peers, compiled in,
 * not negotiated at runtime */
#define UCT_CXL_CTRL_OFFSET 0
#define UCT_CXL_CTRL_SIZE                                                      \
    ucs_align_up(sizeof(uct_cxl_ctrl_t), UCT_CXL_ALIGN)

/* FIFO parameters */
#define UCT_CXL_FIFO_ELEM_SIZE 256
#define UCT_CXL_FIFO_DEPTH     64
#define UCT_CXL_FIFO_OFFSET    (UCT_CXL_CTRL_OFFSET + UCT_CXL_CTRL_SIZE)
#define UCT_CXL_FIFO_SIZE      (UCT_CXL_FIFO_ELEM_SIZE * UCT_CXL_FIFO_DEPTH)

/* [UCT_CXL_RMA_OFFSET, aperture_size) of the *local* memdev is handed out by
 * mem_alloc. Each guest owns its own memdev's RMA region, so the two peers'
 * allocators never need to coordinate. */
#define UCT_CXL_RMA_OFFSET (UCT_CXL_FIFO_OFFSET + UCT_CXL_FIFO_SIZE)

/* Suggested to have a larger (more distinct) number and OR in version number
 * for compatability checking, but too much for a PoC currently */
#define UCT_CXL_MAGIC 1234ULL

/* Placeholder, subject to change */
#define UCT_CXL_RMA_MAX_BCOPY 8192
#define UCT_CXL_RMA_MAX_SHORT 256

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


typedef struct uct_cxl_ctrl {
    uint64_t magic; /* owner, at init */
    uint64_t iface_id; /* owner, at init; peer reads in is_reachable */
    uint64_t ack[UCT_CXL_FIFO_DEPTH]; /* PEER writes ack[i] = seq it consumed
                                          from slot i of OWNER's tx stream */
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
    int local_fd; /* Holds the exclusive flock() on the local device */
    size_t aperture_size; /* discovered/configured window size */
    ucs_spinlock_t lock; /* Protects free_list */
    /* Free uct_cxl_seg_t ranges of the RMA region, sorted by offset, with no
     * two adjacent ranges (consolidated on free)*/
    ucs_list_link_t free_list;
    int iface_in_use;
} uct_cxl_md_t;

typedef struct uct_cxl_md_config {
    uct_md_config_t super;
    /* used to mmap, requests for peer */
    char *accel_dev_path;
    /* used to mmap, reading local backing mem (peer's writes) */
    char *local_dev_path;
    /* size of the HDM-decoded window exposed by accel_dev_path */
    size_t aperture_size;
} uct_cxl_md_config_t;

extern uct_component_t uct_cxl_component;

#endif
