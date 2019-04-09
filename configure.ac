AC_PREREQ(2.60)
AC_INIT([input-wacom],
	[0.42.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([dist-bzip2 no-dist-gzip foreign])
m4_ifdef([AM_EXTRA_RECURSIVE], AM_EXTRA_RECURSIVE_TARGETS([signature]))
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

dnl Check for RedHat entreprise Linux >= 6.9
AC_MSG_CHECKING(RHEL6 minor release)
RHEL6_RELEASE=[$(sed -n 's/^\(Red Hat Enterprise Linux\|Scientific Linux\|CentOS\) .*release 6.\([0-9]*\).*$/\2/gp' "/etc/redhat-release" 2> /dev/null | head -n1)]
dnl RHEL6_RELEASE=""
if test "$RHEL6_RELEASE" = ""; then
	RHEL6_RELEASE="0"
elif test "$RHEL6_RELEASE" -ge "9"; then
	RHEL6_RELEASE="10"
fi
AC_MSG_RESULT([$RHEL6_RELEASE])

dnl Check for RedHat entreprise Linux >= 7.4
AC_MSG_CHECKING(RHEL7 minor release)
RHEL7_RELEASE=[$(sed -n 's/^\(Red Hat Enterprise\|Scientific\|CentOS\) Linux .*release 7.\([0-9]*\).*$/\2/gp' "/etc/redhat-release" 2> /dev/null | head -n1)]
dnl RHEL7_RELEASE=""
if test "$RHEL7_RELEASE" = ""; then
	RHEL7_RELEASE="0"
elif test "$RHEL7_RELEASE" -gt "3"; then
	RHEL7_RELEASE="4"
fi
AC_MSG_RESULT([$RHEL7_RELEASE])

dnl
dnl # code taken from https://github.com/zfsonlinux/spl/blob/master/config/spl-build.m4
dnl # licensed under GPL-v2.0
dnl

dnl #
dnl # WACOM_LINUX_CONFTEST
dnl #
AC_DEFUN([WACOM_LINUX_CONFTEST], [
cat confdefs.h - <<_ACEOF >conftest.c
$1
_ACEOF
])

dnl #
dnl # WACOM_LANG_PROGRAM(C)([PROLOGUE], [BODY])
dnl #
m4_define([WACOM_LANG_PROGRAM], [
$1
int
main (void)
{
dnl Do *not* indent the following line: there may be CPP directives.
dnl Don't move the `;' right after for the same reason.
$2
  ;
  return 0;
}
])

dnl #
dnl # WACOM_LINUX_COMPILE_IFELSE / like AC_COMPILE_IFELSE
dnl #
AC_DEFUN([WACOM_LINUX_COMPILE_IFELSE], [
	m4_ifvaln([$1], [WACOM_LINUX_CONFTEST([$1])])
	rm -Rf .autoconf/build && mkdir -p .autoconf/build && touch .autoconf/build/conftest.mod.c
	echo "obj-m := conftest.o" >.autoconf/build/Makefile
	modpost_flag=''
	test "x$enable_linux_builtin" = xyes && modpost_flag='modpost=true' # fake modpost stage
	AS_IF(
		[AC_TRY_COMMAND(cp conftest.c .autoconf/build && make [$2] -C $WCM_KERNEL_DIR EXTRA_CFLAGS="-Werror-implicit-function-declaration" M=$PWD/.autoconf/build $modpost_flag) >/dev/null && AC_TRY_COMMAND([$3])],
		[$4],
		[_AC_MSG_LOG_CONFTEST m4_ifvaln([$5],[$5])]
	)
	rm -Rf build
])

dnl #
dnl # WACOM_LINUX_TRY_COMPILE like AC_TRY_COMPILE
dnl #
AC_DEFUN([WACOM_LINUX_TRY_COMPILE],
	[WACOM_LINUX_COMPILE_IFELSE(
	[AC_LANG_SOURCE([WACOM_LANG_PROGRAM([[$1]], [[$2]])])],
	[modules],
	[test -s .autoconf/build/conftest.o],
	[$3], [$4])
])

dnl
dnl # end of copy from ZFS/spl
dnl

dnl RedHat entreprise Linux 7.5 backports powersupply functions from 4.1
AC_MSG_CHECKING(power supply version)
WACOM_LINUX_TRY_COMPILE([
#include <linux/power_supply.h>
],[
struct power_supply_desc test;
],[
	HAVE_POWERSUPPLY_41=yes
	AC_MSG_RESULT([v4.1+])
	AC_DEFINE([WACOM_POWERSUPPLY_41], [], [kernel uses powersupply from v4.1+])
],[
	HAVE_POWERSUPPLY_41=no
	AC_MSG_RESULT([pre-v4.1])
])

dnl Red Hat Enterprise Linux 6.x backports abs accessor functions from 2.6.36
AC_MSG_CHECKING(abs accessor version)
WACOM_LINUX_TRY_COMPILE([
#include <linux/input.h>
],[
struct input_dev test;
input_abs_get_res(&test, 0);
],[
	HAVE_ABSACCESSOR_36=yes
	AC_MSG_RESULT([v2.6.36+])
	AC_DEFINE([WACOM_ABSACCESSOR_36], [], [kernel uses abs accessor macros from v2.6.36+])
],[
	HAVE_ABSACCESSOR_36=no
	AC_MSG_RESULT([pre-v2.6.36])
])

dnl Check which version of the driver we should compile
AC_DEFUN([WCM_EXPLODE], [$(echo "$1" | awk '{split($[0],x,"[[^0-9]]"); printf("%03d%03d%03d\n",x[[1]],x[[2]],x[[3]]);}')])
EXPLODED_VER="WCM_EXPLODE($MODUTS)"
if test "$EXPLODED_VER" -lt "WCM_EXPLODE(2.6.32)"; then
	AC_MSG_ERROR([use the kernel driver included in the 'linuxwacom' package for kernels older than 2.6.32])
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(2.6.38)"; then
	WCM_KERNEL_VER="2.6.32"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(3.7)"; then
	WCM_KERNEL_VER="2.6.38"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(3.17)"; then
	WCM_KERNEL_VER="3.7"
elif test "$EXPLODED_VER" -lt "WCM_EXPLODE(4.5)"; then
	WCM_KERNEL_VER="3.17"
else
	WCM_KERNEL_VER="4.5"
fi

dnl overwrite for RHEL distribution
if test "$RHEL7_RELEASE" -ge "4"; then
        WCM_KERNEL_VER="3.17"
fi

dnl =======================================================
dnl Module signing

MODSIGN_ENABLE=default
MODSIGN_HASHALGO=
MODSIGN_PRIVFILE=
MODSIGN_CERTFILE=

AC_ARG_ENABLE(module-signing,
	AS_HELP_STRING([--disable-module-signing], [Disable automatic module signing]),
	[MODSIGN_ENABLE="$enableval"])

if test "$MODSIGN_ENABLE" = "yes" -o "$MODSIGN_ENABLE" = "default"; then
	AC_MSG_CHECKING(kernel sig_enforce parameter)
	SIG_KERNEL=$(cat /sys/module/module/parameters/sig_enforce 2>/dev/null || echo "(unknown)")
	AC_MSG_RESULT([$SIG_KERNEL])

	AC_MSG_CHECKING(mokutil sb-state)
	SIG_MOK=$(mokutil --sb-state 2>/dev/null || echo "(unknown)")
	AC_MSG_RESULT([$SIG_MOK])

	SIG_REQUIRED=$(( $(echo "$SIG_KERNEL" | grep -q "Y" && echo "1" || echo "0") + \
		         $(echo "$SIG_MOK" | grep -q "enabled" && echo "1" || echo "0") \
		      ))



	AC_ARG_WITH(hash-algorithm,
		AS_HELP_STRING([--with-hash-algorithm=<alg>], [Specify module signing hash algorithm]),
		[MODSIGN_HASHALGO="$withval"])

	if test "$MODSIGN_HASHALGO" = "yes" -o -z "$MODSIGN_HASHALGO"; then
		MODSIGN_HASHALGO="sha512"
	fi

	AC_MSG_CHECKING(for module signing hash algorithm)
	AC_MSG_RESULT([$MODSIGN_HASHALGO])



	# There is no standard location for storing kernel signing keys
	# and certificates. The kernel itself has CONFIG_MODULE_SIG_KEY
	# (which contains a key and cert) which likely points to a file
	# that doesn't exist unless you built the kernel yourself. Most
	# distributions use the "shim" bootloader which allows "machine
	# owner keys" (MOK) to be enrolled by the end-user, but only
	# Ubuntu provides a tool to automatically generate these keys
	# (`update-secureboot-policy --new-key`); other distros rely on
	# the user generating the key/cert themselves and keeping it in a
	# suitably-safe location.
	#
	# The kernel should automatically try to sign modules as part of
	# the `make modules_install` step, so that covers the first case.
	# In the second case the best we can do is try Ubuntu's location.
	AC_ARG_WITH(signing-key,
		AS_HELP_STRING([--with-signing-key=<trusted.priv>], [Specify module signing key location]),
		[MODSIGN_PRIVFILE="$withval"])
	AC_ARG_WITH(signing-cert,
		AS_HELP_STRING([--with-signing-cert=<trusted.der>], [Specify module signing cert location]),
		[MODSIGN_CERTFILE="$withval"])

	if test "$MODSIGN_PRIVFILE" = "yes" -o -z "$MODSIGN_PRIVFILE"; then
		MODSIGN_PRIVFILE=$(ls /var/lib/shim-signed/mok/MOK.priv 2>/dev/null || echo "$MODSIGN_PRIVFILE")
	fi
	if test "$MODSIGN_CERTFILE" = "yes" -o -z "$MODSIGN_CERTFILE"; then
		MODSIGN_CERTFILE=$(ls /var/lib/shim-signed/mok/MOK.der 2>/dev/null || echo "$MODSIGN_CERTFILE")
	fi

	AC_MSG_CHECKING(for module signing key)
	AC_MSG_RESULT([$MODSIGN_PRIVFILE])
	AC_MSG_CHECKING(for module signing certificate)
	AC_MSG_RESULT([$MODSIGN_CERTFILE])

	if test ! -f "$MODSIGN_PRIVFILE" -o ! -f "$MODSIGN_CERTFILE"; then
		AC_MSG_WARN([Module signing key and/or certificate missing.])

		if test "$MODSIGN_ENABLE" = "yes"; then
			AC_MSG_ERROR([Unable to honor explicit request for module signing.])
		fi

		if test "$SIG_REQUIRED" -gt 0; then
			AC_MSG_ERROR([Kernel is configured to only load dnl
signed modules but we are unable to produce a signed module. Either (1) dnl
re-run configure with the options '--with-signing-key=<key>' and dnl
'--with-signing-cert=<cert>' set, (2) indicate you will sign the modules dnl
yourself by re-running configure with the '--disable-module-signing' dnl
option set, or (3) disable the kernel's signed module requirement (e.g. dnl
disable secure boot).])
		fi
	fi
fi


dnl Separate test output from file-generation output
echo 

WCM_SRC_SUBDIRS=". $WCM_KERNEL_VER"
AC_SUBST(WCM_KERNEL_DIR)
AC_SUBST(WCM_KERNEL_VER)
AC_SUBST(RHEL6_RELEASE)
AC_SUBST(RHEL7_RELEASE)
AC_SUBST(MODUTS)
AC_SUBST(MODSIGN_HASHALGO)
AC_SUBST(MODSIGN_PRIVFILE)
AC_SUBST(MODSIGN_CERTFILE)

AC_CONFIG_FILES([Makefile
                 2.6.32/Makefile
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

