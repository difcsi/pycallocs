AC_CONFIG_MACRO_DIR([m4])
AC_PREREQ([2.69])
AC_INIT([pycallocs], [0.1], [zoltan.meszaros@kcl.ac.uk])
AC_CONFIG_SRCDIR([allocs_module.c])
AC_CONFIG_HEADERS([config.h])
AC_PREFIX_DEFAULT(/usr/local)

AS_IF([test "x$prefix" = xNONE], [prefix=/usr/local])

CFLAGS="-I$prefix/include $CFLAGS"
CXXFLAGS="-I$prefix/include $CXXFLAGS"
CPPFLAGS="-I$prefix/include $CPPFLAGS"
LDFLAGS="-L$prefix/lib $LDFLAGS"
PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$PKG_CONFIG_PATH"
export CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_PATH

AC_LANG([C])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wno-portability])
AM_MAINTAINER_MODE
AC_PROG_CC
AC_PROG_CC_C99

AC_C_INLINE
AC_TYPE_SIZE_T
AC_CHECK_TYPES([ptrdiff_t])
AC_PROG_LN_S
AC_PROG_INSTALL

# Check for Python
AC_ARG_VAR([PYTHON], [Python interpreter to use])
AS_IF([test "x$PYTHON" = x], [PYTHON=python3])
AC_MSG_CHECKING([for Python])
AS_IF([$PYTHON --version >/dev/null 2>&1],
      [AC_MSG_RESULT([$$PYTHON])],
      [AC_MSG_FAILURE([Python not found. Try setting PYTHON=/path/to/python3])])

# Check for Python development headers
AC_MSG_CHECKING([for Python.h])
PYTHON_INCLUDE=$($PYTHON -c "import sysconfig; print(sysconfig.get_path('include'))" 2>/dev/null)
AS_IF([test -f "$PYTHON_INCLUDE/Python.h"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_FAILURE([Python development headers not found. Try installing python3-dev or python3-devel])])
AC_SUBST([PYTHON_INCLUDE])

# Check for required Python modules
AC_MSG_CHECKING([for Python setuptools])
AS_IF([$PYTHON -c "import setuptools" 2>/dev/null],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_FAILURE([Python setuptools not found. Try: pip install setuptools])])

# Lifetime policies are mandatory for pycallocs
AC_DEFINE([LIFETIME_POLICIES], [4], [Lifetime policies are required and enabled with 4 policies])
lifetime_policies=4

# Debug mode
AC_ARG_ENABLE([debug], 
              AS_HELP_STRING([--enable-debug], [Enable debug mode]),
              [enable_debug=${enableval}], [enable_debug=no])
AS_IF([test "x$enable_debug" = "xyes"],
      [AC_DEFINE([DEBUG],1,[Debug mode enabled])])
AM_CONDITIONAL(DEBUG, [test "x$enable_debug" = "xyes"])

# Find liballocs
AC_ARG_WITH([liballocs],
            [AS_HELP_STRING([--with-liballocs=DIR],
              [where to find liballocs (build directory)])],
            [
                if test -d "$withval"
                then
                        ac_liballocs="$withval"
                else
                        AC_MSG_ERROR([--with-liballocs expected directory name])
                fi
            ],
            [ac_liballocs=$(realpath $srcdir)/contrib/liballocs])

AC_MSG_CHECKING([for liballocs])
AS_IF([test -f "$ac_liballocs/config.mk"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_FAILURE([liballocs not configured. Run: cd $ac_liballocs && ./configure --enable-lifetime-policies && make])])

# Verify liballocs has lifetime policies enabled
AC_MSG_CHECKING([for lifetime policies in liballocs])
AS_IF([grep -q "LIFETIME_POLICIES" "$ac_liballocs/config.h" 2>/dev/null],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_FAILURE([liballocs must be built with --enable-lifetime-policies. Reconfigure: cd $ac_liballocs && ./configure --enable-lifetime-policies && make])])

AC_MSG_CHECKING([for liballocs library])
AS_IF([test -f "$ac_liballocs/lib/liballocs.so"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_FAILURE([liballocs not built. Run: cd $ac_liballocs && make])])

# Find allocscc
allocscc="$ac_liballocs/tools/lang/c/bin/allocscc"
AC_MSG_CHECKING([for allocscc])
AS_IF([test -x "$allocscc"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_FAILURE([allocscc not found at $allocscc])])

# Get paths from liballocs
librunt="$ac_liballocs/contrib/libsystrap/contrib/librunt"
libsystrap="$ac_liballocs/contrib/libsystrap"
liballocstool="$ac_liballocs/contrib/liballocstool"

AC_SUBST([liballocs], [$ac_liballocs])
AC_SUBST([allocscc], [$allocscc])
AC_SUBST([librunt], [$librunt])
AC_SUBST([libsystrap], [$libsystrap])
AC_SUBST([liballocstool], [$liballocstool])
AC_SUBST([lifetime_policies], [$lifetime_policies])

AC_CONFIG_FILES([Makefile config.mk])
AC_OUTPUT
