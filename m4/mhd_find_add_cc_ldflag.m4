# SYNOPSIS
#
#   MHD_FIND_ADD_CC_LDFLAG([VARIABLE-TO-EXTEND],
#                         [FLAG1-TO-TEST], [FLAG2-TO-TEST], ...)
#
# DESCRIPTION
#
#   This macro checks whether the specific compiler flags are supported.
#   The flags are checked one-by-one. The checking is stopped when the first
#   supported flag found.
#   The checks are performing by appending FLAGx-TO-TEST to the value of
#   VARIABLE-TO-EXTEND (LDFLAGS if not specified), then prepending result to
#   LDFLAGS (unless VARIABLE-TO-EXTEND is LDFLAGS), and then performing compile
#   and link test. If test succeed without warnings, then the flag is added to
#   VARIABLE-TO-EXTEND and next flags are not checked. If compile-link cycle
#   cannot be performed without warning with all tested flags, no flag is
#   added to the VARIABLE-TO-EXTEND.
#
#   Example usage:
#
#     MHD_CHECK_CC_LDFLAG([additional_LDFLAGS],
#                         [-Wl,--strip-all], [-Wl,--strip-debug])
#
#   Note: Unlike others MHD_CHECK_*CC_LDFLAG* macro, this macro uses another
#   order of parameters.
#
# LICENSE
#
#   Copyright (c) 2022 Karlson2k (Evgeny Grin) <k2k@narod.ru>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

#serial 2

AC_DEFUN([MHD_FIND_ADD_CC_LDFLAG],[dnl
_MHD_FIND_ADD_CC_XFLAG([[LDFLAGS]],[],[],$@)])
