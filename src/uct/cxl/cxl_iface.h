#ifndef CXL_IFACE_H_
#define CXL_IFACE_H_

#include <uct/api/uct.h>
#include <uct/base/uct_iface.h>
#include <stdint.h>
#include "cxl_md.h"

#define UCT_CXL_IFACE_LATENCY             ucs_linear_func_make(10e-6, 0)
#define UCT_CXL_IFACE_BANDWIDTH_DEDICATED 10e6
#define UCT_CXL_IFACE_BANDWIDTH_SHARED    0
#define UCT_CXL_IFACE_OVERHEAD            1e-6

typedef uint64_t uct_cxl_iface_addr_t;

typedef struct uct_cxl_iface {
    uct_base_iface_t super;
    uct_cxl_md_t *md;
    uint64_t id; /* unique iface identifier */
    uct_cxl_fifo_ctrl_t *local_ctrl; /* own control in RAM */
    uct_cxl_fifo_ctrl_t *aperture_ctrl; /* peer control over aperture */
    struct {
        uct_cxl_fifo_elem_t *fifo; /* peer's RX, via aperture */
        uint64_t head; /* next sequence index to send */
        ucs_queue_head_t pending;
    } tx;
    struct {
        uct_cxl_fifo_elem_t *fifo; /* own RX in local RAM */
        uint64_t tail; /* next sequence index to consume */
    } rx;
} uct_cxl_iface_t;

typedef struct uct_cxl_iface_config {
    uct_iface_config_t super;
} uct_cxl_iface_config_t;

ucs_status_t
uct_cxl_iface_query_tl_devices(uct_md_h md,
                               uct_tl_device_resource_t **tl_devices_p,
                               unsigned *num_tl_devices_p);

#endif
