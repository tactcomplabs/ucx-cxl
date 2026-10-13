#ifndef UCT_CXL_MD_H_
#define UCT_CXL_MD_H_

#include <uct/base/uct_md.h>
#include <ucs/config/types.h>
#include <ucs/type/status.h>
#include "cxl_iface.h"

/* Fixed partition of the CXL aperture - identical on both peers, compiled in,
 * not negotiated at runtime */
#define UCT_CXL_CTRL_OFFSET   0
#define UCT_CXL_CTRL_SIZE     ucs_align_up(sizeof(uct_cxl_fifo_ctl_t), \
                                            UCS_SYS_CACHE_LINE_SIZE)

#define UCT_CXL_FIFO_OFFSET   (UCT_CXL_CTRL_OFFSET + UCT_CXL_CTRL_SIZE)
#define UCT_CXL_FIFO_SIZE      (UCT_CXL_FIFO_ELEM_SIZE * UCT_CXL_FIFO_DEPTH)

#define UCT_CXL_RMA_OFFSET    (UCT_CXL_FIFO_OFFSET + UCT_CXL_FIFO_SIZE)
/* [UCT_CXL_RMA_OFFSET, aperture_size) is bump-allocated for mem_alloc/mem_reg */

typedef struct uct_cxl_packed_rkey {
    uint64_t offset;
    size_t   length;
} UCS_S_PACKED uct_cxl_packed_rkey_t;

typedef struct uct_cxl_md {
    uct_md_t super;
    void     *aperture_base;   /* mmap'd VA of the CXL BAR */
    size_t   aperture_size;    /* discovered/configured window size */
    size_t   rma_next_offset;  /* bump allocator cursor, starts at UCT_CXL_RMA_OFFSET */
} uct_cxl_md_t;

typedef struct uct_cxl_md_config {
    uct_md_config_t super;
    // used to mmap, requests for peer
    char *accel_dev_path;
    // used to mmap, reading local backing mem (peer's writes)
    char *local_dev_path;
} uct_cxl_md_config_t;

#endif
