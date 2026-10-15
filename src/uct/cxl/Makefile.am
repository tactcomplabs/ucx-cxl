#
# Copyright (c) Tactical Computing Laboratories, LLC. 2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

if HAVE_CXL

module_LTLIBRARIES     = libuct_cxl.la
libuct_cxl_la_CFLAGS   = $(BASE_CFLAGS) $(LT_CFLAGS)
libuct_cxl_la_CPPFLAGS = $(BASE_CPPFLAGS)
libuct_cxl_la_LIBADD   = $(top_builddir)/src/ucs/libucs.la \
                         $(top_builddir)/src/uct/libuct.la
libuct_cxl_la_LDFLAGS  = -version-info $(SOVERSION) $(UCT_MODULE_LDFLAGS) \
                         $(UCX_LT_RELEASE)

noinst_HEADERS = \
	cxl_ep.h \
	cxl_iface.h \
	cxl_md.h

libuct_cxl_la_SOURCES = \
	cxl_ep.c \
	cxl_iface.c \
	cxl_md.c

PKG_CONFIG_NAME=cxl

include $(top_srcdir)/config/module.am
include $(top_srcdir)/config/module-pkg-config.am

endif
