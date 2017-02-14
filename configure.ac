AC_PREREQ(2.60)
AC_INIT([input-wacom],
	[0.34.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([dist-bzip2 no-dist-gzip])
AM_MAINTAINER_MODE

AC_PROG_CC

dnl =======================================================
dnl Check if we should be compiling for linux
AC_MSG_CHECKING(for kernel type)
WCM_KERNEL=`uname -s`
AC_MSG_RESULT($WCM_KERNEL)

AC_MSG_CHECKING(for linux-based kernel)
WCM_ISLINUX=no
if echo $WCM_KERNEL | grep -i linux >/dev/null; then
	WCM_ISLINUX=yes
fi
AC_MSG_RESULT($WCM_ISLINUX)

dnl  kernel source, module versioning, etc
WCM_ENV_KERNEL=
WCM_KERNEL_DIR=
WCM_KERNEL_VER=
MODUTS=

AC_DEFUN([WCM_ISBUILDDIR], [\
    \( -f "$1/.config" -o \
       -f "$1/include/config/auto.conf" -o \
       -f "$1/include/generated/autoconf.h" \) \
])

AC_ARG_WITH(kernel,
	AS_HELP_STRING([--with-kernel=dir], [Specify kernel source directory]),
	[WCM_KERNEL_DIR="$withval"])

AC_ARG_WITH(kernel-version,
	AS_HELP_STRING([--with-kernel-version=version], [Specify kernel version]),
	[MODUTS="$withval"])


dnl Assume the user wants to build for the running kernel version if neither
dnl the kernel directory nor version has been specified
if test "$WCM_KERNEL_DIR" = "yes" -o -z "$WCM_KERNEL_DIR"; then
	if test "$MODUTS" = "yes" -o -z "$MODUTS"; then
		MODUTS=`uname -r`
	fi
fi

dnl Discover the kernel source location if not already set
AC_MSG_CHECKING(for kernel source/headers)
if test "$WCM_KERNEL_DIR" = "yes" -o -z "$WCM_KERNEL_DIR"; then
	if test "$MODUTS" = "yes" -o -z "$MODUTS"; then X=`uname -r`; else X="$MODUTS"; fi
	AC_DEFUN([SEARCH_DIRECTORIES], [[
	  [/lib/modules/$X/source],
	  [/lib/modules/$X/build],
	  [/usr/src/linux],
	  [/usr/src/linux-$X],
	  [/usr/src/linux-2.6]
	]])
	WCM_KERNEL_DIR=
	dnl Kernel source not specified, guess where it is
	m4_foreach([ROOTDIR], SEARCH_DIRECTORIES, [
		if test -z "$WCM_KERNEL_DIR"; then
			if test WCM_ISBUILDDIR(ROOTDIR); then
				WCM_KERNEL_DIR="ROOTDIR"
			fi
		fi
	])
fi
if test -z "$WCM_KERNEL_DIR"; then
	AC_MSG_RESULT([not found])
	AC_MSG_WARN([Unable to find build config in any of: SEARCH_DIRECTORIES])
fi
AC_MSG_RESULT([$WCM_KERNEL_DIR])
if test \! WCM_ISBUILDDIR($WCM_KERNEL_DIR); then
	AC_MSG_WARN([Kernel directory does not appear to have needed config files])
	WCM_ENV_KERNEL="no"
fi

dnl Discover the kernel source version if not already set
AC_MSG_CHECKING(kernel version)
SRC_VERSION=[$(sed -n '/UTS_RELEASE/ s/^[^"]*"\([^"]*\).*$/\1/gp' \
              "$WCM_KERNEL_DIR/include/linux/version.h"           \
              "$WCM_KERNEL_DIR/include/generated/utsrelease.h"    \
              "$WCM_KERNEL_DIR/include/linux/utsrelease.h" 2> /dev/null | head -n1)]
AC_MSG_RESULT([$SRC_VERSION])
if test "$MODUTS" = "yes" -o -z "$MODUTS"; then
	MODUTS="$SRC_VERSION"
elif test "$SRC_VERSION" != "$MODUTS"; then
	AC_MSG_WARN([Spcified '$MODUTS' kernel, but found '$SRC_VERSION' instead])
	WCM_ENV_KERNEL="no"
fi


if test "$WCM_ENV_KERNEL" = "no"; then
	AC_MSG_ERROR([We could not find the development environment to dnl
build modules for the '$MODUTS' kernel within the '$WCM_KERNEL_DIR' dnl
directory. Please install the kernel source or the kernel development dnl
package and try again.])
fi
WCM_ENV_KERNEL="yes"


dnl Check which version of the driver we should compile
AC_DEFUN([WCM_EXPLODE], [$(echo "$1" | awk '{split($[0],x,"[[^0-9]]"); printf("%03d%03d%03d\n",x[[1]],x[[2]],x[[3]]);}')])
EXPLODED_VER="WCM_EXPLODE($MODUTS)"
if test "$EXPLODED_VER" -lt "WCM_EXPLODE(2.6.30)"; then
	AC_MSG_ERROR([use the kernel driver included in the 'linuxwacom' package for kernels older than 2.6.30])
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(2.6.36)"; then
	WCM_KERNEL_VER="2.6.30"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(2.6.38)"; then
	WCM_KERNEL_VER="2.6.36"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(3.7)"; then
	WCM_KERNEL_VER="2.6.38"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(3.17)"; then
	WCM_KERNEL_VER="3.7"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(4.5)"; then
	WCM_KERNEL_VER="3.17"
else
	WCM_KERNEL_VER="4.5"
fi

dnl Separate test output from file-generation output
echo 

WCM_SRC_SUBDIRS=". $WCM_KERNEL_VER"
AC_SUBST(WCM_KERNEL_DIR)
AC_SUBST(WCM_KERNEL_VER)
AC_SUBST(MODUTS)

AC_CONFIG_FILES([Makefile
                 2.6.30/Makefile
                 2.6.36/Makefile
                 2.6.38/Makefile
                 3.7/Makefile
                 3.17/Makefile
                 4.5/Makefile])
AC_OUTPUT

AC_MSG_NOTICE([
----------------------------------------
  BUILD ENVIRONMENT:
       linux kernel - $WCM_ISLINUX $WCM_KERNEL_VER
      kernel source - $WCM_ENV_KERNEL $WCM_KERNEL_DIR

NOTE: The kernel drivers included in this package are only tested with the
X Wacom driver built from xf86-input-wacom. If you are running an X server
version older than 1.7, please use the drivers provided by the linuxwacom
package.

Please run 'make && make install'.])

