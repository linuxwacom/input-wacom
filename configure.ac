AC_PREREQ(2.60)
AC_INIT([input-wacom],
	[0.27.0])
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
WCM_ENV_KERNEL=no
WCM_KERNEL_DIR=
WCM_KERNEL_VER=
MODUTS=

dnl Check which version of the driver we should compile
AC_ARG_WITH(kernel-version,
	AS_HELP_STRING([--with-kernel-version=version], [Specify kernel version]),
	[MODUTS="$withval"],
	[MODUTS=`uname -r`])

AC_MSG_CHECKING(kernel version)
AC_MSG_RESULT([$MODUTS])
MINOR=`echo $MODUTS | sed 's/[[0-9]]*\.[[0-9]]*\.\([[0-9]]*\).*/\1/'`
if echo $MODUTS | grep "^2.4" >/dev/null; then
	AC_MSG_WARN([kernel 2.4.x is not supported])
elif echo $MODUTS | grep "^2.6" >/dev/null; then
	if test $MINOR -ge 26; then
		case $MINOR in
			26|27|28|29|30|31|32|33|34|35) WCM_KERNEL_VER="2.6.30";;
			36|37) WCM_KERNEL_VER="2.6.36";;
			*) WCM_KERNEL_VER="2.6.38";;
		esac
	else
		AC_MSG_WARN([kernels older than 2.6.26 is supported by linuxwacom])
	fi
elif echo $MODUTS | grep "^3." >/dev/null; then
	MINOR=`echo $MODUTS | sed 's/[[0-9]]*\.\([[0-9]]*\).*/\1/'`
	if test $MINOR -ge 17; then
		WCM_KERNEL_VER="3.17"
	elif test $MINOR -ge 7; then
		WCM_KERNEL_VER="3.7"
	else
		WCM_KERNEL_VER="2.6.38"
	fi
elif echo $MODUTS | grep "^4." >/dev/null; then
	WCM_KERNEL_VER="3.17"
else
	AC_MSG_WARN(kernel version $MODUTS not supported)
fi

dnl Check for kernel build environment
AC_ARG_WITH(kernel,
	AS_HELP_STRING([--with-kernel=dir], [Specify kernel source directory]),
	[WCM_KERNEL_DIR="$withval"])

AC_MSG_CHECKING(for kernel source/headers)
if test "$WCM_KERNEL_DIR" = "yes" -o -z "$WCM_KERNEL_DIR"; then
	AC_DEFUN([SEARCH_DIRECTORIES], [[
	  [/lib/modules/$MODUTS/source],
	  [/lib/modules/$MODUTS/build],
	  [/usr/src/linux],
	  [/usr/src/linux-$MODUTS],
	  [/usr/src/linux-2.6]
	]])
	WCM_KERNEL_DIR=
	dnl Kernel source not specified, guess where it is
	m4_foreach([ROOTDIR], SEARCH_DIRECTORIES, [
		if test -z "$WCM_KERNEL_DIR"; then
			if test -f "ROOTDIR/.config"; then
				WCM_KERNEL_DIR="ROOTDIR"
				AC_MSG_RESULT($WCM_KERNEL_DIR)
			fi
		fi
	])

	m4_foreach([ROOTDIR], SEARCH_DIRECTORIES, [
		if test -z "$WCM_KERNEL_DIR"; then
			if test -f "ROOTDIR/include/linux/input.h"; then
				WCM_KERNEL_DIR="ROOTDIR"
				AC_MSG_RESULT($WCM_KERNEL_DIR)
			fi
		fi
	])

	if test -z "$WCM_KERNEL_DIR"; then
		AC_MSG_RESULT([not found])
		AC_MSG_NOTICE([Unable to find headers in any of: SEARCH_DIRECTORIES])
	fi
elif test "$WCM_KERNEL_DIR" != "no"; then
	AC_MSG_RESULT([$WCM_KERNEL_DIR])
	if test \! -f "$WCM_KERNEL_DIR/include/linux/input.h"; then
		AC_MSG_NOTICE([Kernel directory does not appear to contain headers])
	fi
fi

dnl Final sanity check to make sure the kernel versions match
for F in "$WCM_KERNEL_DIR/include/linux/version.h" \
	"$WCM_KERNEL_DIR/include/generated/utsrelease.h" \
	"$WCM_KERNEL_DIR/include/linux/utsrelease.h"; do
	if test -f $F; then
		if test "$MODUTS" = "`grep UTS_RELEASE $F | sed -e 's/^[[^"]]*"\([[^"]]*\).*$/\1/g'`"; then
			WCM_ENV_KERNEL=yes
		fi
	fi
done

if test "$WCM_ENV_KERNEL" = "no"; then
	AC_MSG_ERROR([We could not find the development environment to dnl
build modules for the '$MODUTS' kernel. Please install the kernel source or dnl
the kernel development package and try again.])
fi

dnl Separate test output from file-generation output
echo 

WCM_SRC_SUBDIRS=". $WCM_KERNEL_VER"
AC_SUBST(WCM_KERNEL_DIR)
AC_SUBST(WCM_KERNEL_VER)

AC_CONFIG_FILES([Makefile
                 2.6.30/Makefile
                 2.6.36/Makefile
                 2.6.38/Makefile
                 3.7/Makefile
                 3.17/Makefile])
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

