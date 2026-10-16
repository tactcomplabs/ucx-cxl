#ifndef UCT_CXL_MD_H_
#define UCT_CXL_MD_H_

#include <uct/base/uct_md.h>
#include <ucs/config/types.h>
#include <ucs/datastruct/list.h>
#include <ucs/type/spinlock.h>
#include <ucs/type/status.h>
#include "cxl_iface.h"

/* Fixed partition of the CXL aperture - identical on both peers, compiled in,
 * not negotiated at runtime */
#define UCT_CXL_CTRL_OFFSET 0
#define UCT_CXL_CTRL_SIZE                                                      \
    ucs_align_up(sizeof(uct_cxl_fifo_ctl_t), UCS_SYS_CACHE_LINE_SIZE)

#define UCT_CXL_FIFO_OFFSET (UCT_CXL_CTRL_OFFSET + UCT_CXL_CTRL_SIZE)
#define UCT_CXL_FIFO_SIZE   (UCT_CXL_FIFO_ELEM_SIZE * UCT_CXL_FIFO_DEPTH)

#define UCT_CXL_RMA_OFFSET (UCT_CXL_FIFO_OFFSET + UCT_CXL_FIFO_SIZE)
/* [UCT_CXL_RMA_OFFSET, aperture_size) of the *local* memdev is handed out by
 * mem_alloc. Each guest owns its own memdev's RMA region, so the two peers'
 * allocators never need to coordinate. */

/* Granularity of RMA region allocations, both offset and length */
#define UCT_CXL_SEG_ALIGN UCS_SYS_CACHE_LINE_SIZE


typedef struct uct_cxl_packed_rkey {
    uint64_t offset;
    size_t length;
} UCS_S_PACKED uct_cxl_packed_rkey_t;


/* A range of the local RMA region. Returned by mem_alloc as the memh. If
 * linked on uct_cxl_md_t::free_list, it describes a free range instead. */
typedef struct uct_cxl_seg {
    ucs_list_link_t list; /* Entry in free_list, unused while allocated */
    uint64_t offset; /* From the start of the memdev */
    size_t length;
} uct_cxl_seg_t;


struct uct_cxl_md {
    uct_md_t super;
    void *aperture_base; /* mmap'd VA of the CXL BAR */
    void *local_base; /* mmap'd VA of the local CXL BAR */
    size_t aperture_size; /* discovered/configured window size */
    ucs_spinlock_t lock; /* Protects free_list */
    /* Free uct_cxl_seg_t ranges of the RMA region, sorted by offset, with no
     * two adjacent ranges (consolidated on free)*/
    ucs_list_link_t free_list;
};

typedef struct uct_cxl_md_config {
    uct_md_config_t super;
    // used to mmap, requests for peer
    char *accel_dev_path;
    // used to mmap, reading local backing mem (peer's writes)
    char *local_dev_path;
    // size of the HDM-decoded window exposed by accel_dev_path
    size_t aperture_size;
} uct_cxl_md_config_t;

extern uct_component_t uct_cxl_component;

#endif
