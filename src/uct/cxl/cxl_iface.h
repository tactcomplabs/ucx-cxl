#ifndef CXL_IFACE_H_
#define CXL_IFACE_H_

#include <uct/api/uct.h>
#include <uct/base/uct_iface.h>
#include <stdint.h>
#include "cxl_md.h"

typedef uint64_t uct_cxl_iface_addr_t;

typedef struct uct_cxl_iface {
    uct_base_iface_t super;
    uct_cxl_md_t *md; /* or obtained via iface->super.md */
    uct_cxl_fifo_ctl_t *fifo_ctl; /* = md->aperture_base + UCT_CXL_CTRL_OFFSET */
    uct_cxl_fifo_elem_t *fifo; /* = md->aperture_base + UCT_CXL_FIFO_OFFSET */
} uct_cxl_iface_t;

typedef struct uct_cxl_iface_config {
    uct_iface_config_t super;
} uct_cxl_iface_config_t;

ucs_status_t
uct_cxl_iface_query_tl_devices(uct_md_h md,
                               uct_tl_device_resource_t **tl_devices_p,
                               unsigned *num_tl_devices_p);

#endif
