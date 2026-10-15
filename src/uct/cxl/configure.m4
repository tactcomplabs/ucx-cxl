#
# Copyright (c) Tactical Computing Laboratories, LLC. 2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

cxl_happy="no"

AC_ARG_WITH([cxl],
            [AS_HELP_STRING([--with-cxl], [Build CXL Type 2 accelerator transport (default is no).])],
            [], [with_cxl=no])

AS_IF([test "x$with_cxl" != xno],
      [uct_modules="${uct_modules}:cxl"
       cxl_happy="yes"
       AC_DEFINE([HAVE_TL_CXL], [1], [Defined if CXL transport exists])])

AM_CONDITIONAL([HAVE_CXL], [test "x$cxl_happy" != xno])
AC_CONFIG_FILES([src/uct/cxl/Makefile
                 src/uct/cxl/ucx-cxl.pc])
