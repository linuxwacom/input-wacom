AC_PREREQ(2.60)
AC_INIT([input-wacom],
	[1.5.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([dist-bzip2 no-dist-gzip foreign])
m4_ifdef([AM_EXTRA_RECURSIVE], AM_EXTRA_RECURSIVE_TARGETS([signature]))
AM_MAINTAINER_MODE

AC_PROG_CC

dnl =======================================================
dnl Check if we have a sane build path
SANITY=`readlink -e "$srcdir"`
AC_MSG_CHECKING(if build path '$SANITY' has spaces)
if `echo "$SANITY" | grep -q ' '`; then
	AC_MSG_RESULT([yes])
	AC_MSG_ERROR([Cannot build from a path with a space in it!])
fi
AC_MSG_RESULT([no])


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
	  [/usr/src/linux-$X]
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
#include "linux/module.h"
MODULE_LICENSE("GPL");
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
		[AC_TRY_COMMAND(cp conftest.c .autoconf/build && make [$2] -C $WCM_KERNEL_DIR EXTRA_CFLAGS="-fno-pie -Werror-implicit-function-declaration" KBUILD_MODPOST_WARN=1 M=$PWD/.autoconf/build $modpost_flag) >/dev/null && AC_TRY_COMMAND([$3])],
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

dnl Check which API is available for determining bus type
AC_MSG_CHECKING(hid_is_using_ll_driver)
WACOM_LINUX_TRY_COMPILE([
#include <linux/hid.h>
bool hid_is_using_ll_driver(struct hid_device *hdev, struct hid_ll_driver *driver) { return 0; }
],[
],[
	HAVE_IS_USING_LL_DRIVER=no
	AC_MSG_RESULT([no])
],[
	HAVE_IS_USING_LL_DRIVER=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_USING_LL_DRIVER], [], [kernel defines hid_is_using_ll_driver from v4.14+])
])
AC_MSG_CHECKING(hid_is_usb)
WACOM_LINUX_TRY_COMPILE([
#include <linux/hid.h>
bool hid_is_usb(struct hid_device *hdev) { return 0; }
],[
],[
	HAVE_HID_IS_USB=no
	AC_MSG_RESULT([no])
],[
	HAVE_HID_IS_USB=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_HID_IS_USB], [], [kernel defines hid_is_usb from v5.15+])
])

dnl Check which API style the I2C subsystem uses for probing.
dnl The old-style was deprecated in Linux 4.10 and a "probe_new" function
dnl introduced during the transition. The old-style was removed and
dnl "probe_new" renamed in Linux 6.3.
AC_MSG_CHECKING(legacy I2C probe API)
WACOM_LINUX_TRY_COMPILE([
#include <linux/i2c.h>
int probe_legacy(struct i2c_client *client, const struct i2c_device_id *id);
],[
struct i2c_driver test = { .probe = probe_legacy };
],[
	HAVE_I2C_PROBE_LEGACY=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_PROBE_LEGACY], [], [legacy .probe removed in v6.3 and later])
],[
	HAVE_I2C_PROBE_LEGACY=no
	AC_MSG_RESULT([no])
])

dnl Check if input_set_timestamp is available to us or not. This should
dnl be available in Linux 5.4.0 and later.
AC_MSG_CHECKING(input_set_timestamp)
WACOM_LINUX_TRY_COMPILE([
#include <linux/input.h>
bool input_set_timestamp(struct input_dev *dev, ktime_t timestamp) { return true; }
],[
],[
	HAVE_INPUT_SET_TIMESTAMP=no
	AC_MSG_RESULT([no])
],[
	HAVE_INPUT_SET_TIMESTAMP=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_INPUT_SET_TIMESTAMP], [], [kernel defines input_set_timestamp from v5.4+])
])

dnl Check if resolution_multiplier is available to us or not. This should
dnl be available in Linux 5.0.0 and later.
AC_MSG_CHECKING(resolution_multiplier)
WACOM_LINUX_TRY_COMPILE([
#include <linux/hid.h>
int test(struct hid_usage *usage) { return usage->resolution_multiplier; }
],[
],[
	HAVE_RESOLUTION_MULTIPLIER=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_RESOLUTION_MULTIPLIER], [], [kernel defines resolution_multiplier from v5.0+])
],[
	HAVE_RESOLUTION_MULTIPLIER=no
	AC_MSG_RESULT([no])
])

dnl Check if LED triggers define brightness. This should be in Linux
dnl 6.10 and later.
AC_MSG_CHECKING(trigger brightness)
WACOM_LINUX_TRY_COMPILE([
#include <linux/leds.h>
int test(struct led_trigger *trigger) { return trigger->brightness; }
],[
],[
	HAVE_TRIGGER_BRIGHTNESS=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_TRIGGER_BRIGHTNESS], [], [kernel defines trigger brightness from v6.10+])
],[
	HAVE_TRIGGER_BRIGHTNESS=no
	AC_MSG_RESULT([no])
])

dnl Check if unaligned.h has been moved to linux/. This should be in Linux
dnl 6.12 and later.
AC_MSG_CHECKING(for linux/unaligned.h)
WACOM_LINUX_TRY_COMPILE([
#include <linux/unaligned.h>
],[
],[
	HAVE_LINUX_UNALIGNED=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_LINUX_UNALIGNED], [], [kernel defines linux/unaligned.h v6.12+])
],[
	HAVE_LINUX_UNALIGNED=no
	AC_MSG_RESULT([no])
])

dnl Check if del_timer_sync has been renamed to timer_delete_sync or
dnl not. This is the case in Linux 6.1.84 and later.
AC_MSG_CHECKING(timer_delete_sync)
WACOM_LINUX_TRY_COMPILE([
#include <linux/timer.h>
bool timer_delete_sync(struct timer_list *timer) { return true; }
],[
],[
	HAVE_TIMER_DELETE_SYNC=no
	AC_MSG_RESULT([no])
],[
	HAVE_TIMER_DELETE_SYNC=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_TIMER_DELETE_SYNC], [], [kernel defines timer_delete_sync from v6.1.84+])
])

dnl Check if from_timer has been renamed to timer_container_of or
dnl not. This is the case in Linux 6.16 and later.
AC_MSG_CHECKING(timer_container_of)
WACOM_LINUX_TRY_COMPILE([
#include <linux/timer.h>
#ifndef timer_container_of
#error "timer_container_of is not defined"
#endif
],[
],[
	HAVE_TIMER_CONTAINER_OF=yes
	AC_MSG_RESULT([yes])
	AC_DEFINE([WACOM_TIMER_ADDR_CONTAINER_RENAME], [], [kernel defines timer_container_of from v6.16])
],[
	HAVE_TIMER_CONTAINER_OF=no
	AC_MSG_RESULT([no])
])

dnl Check which version of the driver we should compile
AC_DEFUN([WCM_EXPLODE], [$(echo "$1" | awk '{split($[0],x,"[[^0-9]]"); printf("%03d%03d%03d\n",x[[1]],x[[2]],x[[3]]);}')])
EXPLODED_VER="WCM_EXPLODE($MODUTS)"
if test "$EXPLODED_VER" -lt "WCM_EXPLODE(4.18)"; then
	AC_MSG_ERROR(m4_normalize([Kernels older than 4.18 are no longer supported by input-wacom.
		 For newer Wacom models, please upgrade your system to a newer kernel.
		 For old Wacom models, 'input-wacom-1.2.0' may still support the device]))
else
	WCM_KERNEL_VER="4.18"
fi

dnl =======================================================
dnl Module signing
AC_DEFUN([WACOM_LINUX_READ_CONFIG], [grep -sh '^$1='  $WCM_KERNEL_DIR/.config /boot/config-$MODUTS | head -n1 | cut -d= -f2- | sed -e 's/^"\(.*\)"$/\1/'])

AC_DEFUN([WACOM_LINUX_FILE_IF_EXISTS], [test -f "$1" && readlink -e "$1"])

AC_DEFUN([WACOM_LINUX_CHECK_KEYCERT],
if test "$MODSIGN_PRIVFILE" = "yes" -o -z "$MODSIGN_PRIVFILE"; then
	AC_MSG_CHECKING(for $1 key at $2)
	KEYFILE=$(WACOM_LINUX_FILE_IF_EXISTS([$2]))
	RESULT=$(test -z "$KEYFILE" && echo "no" || echo "yes")
	AC_MSG_RESULT([$RESULT])
	AC_MSG_CHECKING(for $1 cert at $3)
	CERTFILE=$(WACOM_LINUX_FILE_IF_EXISTS([$3]))
	RESULT=$(test -z "$CERTFILE" && echo "no" || echo "yes")
	AC_MSG_RESULT([$RESULT])
	if test -n "$KEYFILE" -a -n "$CERTFILE"; then
		MODSIGN_PRIVFILE="$KEYFILE"
		MODSIGN_CERTFILE="$CERTFILE";
	fi
fi)


MODSIGN_ENABLE=default
MODSIGN_HASHALGO=
MODSIGN_PRIVFILE=
MODSIGN_CERTFILE=

AC_ARG_ENABLE(module-signing,
	AS_HELP_STRING([--disable-module-signing], [Disable automatic module signing]),
	[MODSIGN_ENABLE="$enableval"])

if test "$MODSIGN_ENABLE" = "yes" -o "$MODSIGN_ENABLE" = "default"; then
	AC_MSG_CHECKING(CONFIG_MODULE_SIG)
	MODSIGN_VERIFY=$(WACOM_LINUX_READ_CONFIG([CONFIG_MODULE_SIG]))
	AC_MSG_RESULT([$MODSIGN_VERIFY])

	AC_MSG_CHECKING(CONFIG_MODULE_SIG_FORCE)
	MODSIGN_ENFORCE=$(WACOM_LINUX_READ_CONFIG([CONFIG_MODULE_SIG_FORCE]))
	AC_MSG_RESULT([$MODSIGN_ENFORCE])

	AC_MSG_CHECKING(CONFIG_LOCK_DOWN_KERNEL)
	MODSIGN_LOCKDOWN=$(WACOM_LINUX_READ_CONFIG([CONFIG_LOCK_DOWN_KERNEL]))
	AC_MSG_RESULT([$MODSIGN_LOCKDOWN])

	AC_MSG_CHECKING(CONFIG_LOCK_DOWN_IN_EFI_SECURE_BOOT)
	MODSIGN_LOCKDOWN_EFI=$(WACOM_LINUX_READ_CONFIG([CONFIG_LOCK_DOWN_KERNEL_IN_EFI_SECURE_BOOT]))
	AC_MSG_RESULT([$MODSIGN_LOCKDOWN_EFI])

	AC_MSG_CHECKING(secure boot state)
	SECUREBOOT_STATE="0"
	test "$SECUREBOOT_STATE" = "1" || SECUREBOOT_STATE=$(efivar -d -n 8be4df61-93ca-11d2-aa0d-00e098032b8c-SecureBoot 2>/dev/null)
	test "$SECUREBOOT_STATE" = "1" || SECUREBOOT_STATE=$(hexdump -e '1/4 "%d"' /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c 2>/dev/null | tail -c1)
	test "$SECUREBOOT_STATE" = "1" || SECUREBOOT_STATE=$(bootctl status 2>/dev/null | sed -n 's/\s*Secure Boot: enabled/1/p; s/\s*Secure Boot: disabled/0/p')
	test "$SECUREBOOT_STATE" = "1" || SECUREBOOT_STATE=$(mokutil --sb-state 2>/dev/null | sed -n 's/SecureBoot enabled/1/p; s/SecureBoot disabled/0/p')
	test "$SECUREBOOT_STATE" = "1" && SECUREBOOT_STATE="on" || SECUREBOOT_STATE="off"
	AC_MSG_RESULT([$SECUREBOOT_STATE])

	AC_MSG_CHECKING(if modules must be signed)
	MODSIGN_REQUIRED="no"
	if test "$MODSIGN_VERIFY" = "y" -o "$MODSIGN_LOCKDOWN" = "y" -o "$MODSIGN_LOCKDOWN_EFI"; then
		MODSIGN_REQUIRED="recommended"
	fi
	if test "$MODSIGN_ENFORCE" = "y" -o \( "$MODSIGN_LOCKDOWN_EFI" = "y" -a "$SECUREBOOT_STATE" = "on" \); then
		MODSIGN_REQUIRED="yes"
	fi
	AC_MSG_RESULT([$MODSIGN_REQUIRED])



	AC_ARG_WITH(hash-algorithm,
		AS_HELP_STRING([--with-hash-algorithm=<alg>], [Specify module signing hash algorithm]),
		[MODSIGN_HASHALGO="$withval"])

	if test "$MODSIGN_HASHALGO" = "yes" -o -z "$MODSIGN_HASHALGO"; then
		AC_MSG_CHECKING(CONFIG_MODULE_SIG_HASH)
		MODSIGN_HASHALGO=$(WACOM_LINUX_READ_CONFIG([CONFIG_MODULE_SIG_HASH]))
		AC_MSG_RESULT([$MODSIGN_HASHALGO])
	fi
	if test "$MODSIGN_HASHALGO" = "yes" -o -z "$MODSIGN_HASHALGO"; then
		MODSIGN_HASHALGO="sha512"
	fi

	AC_MSG_CHECKING(for module signing hash algorithm)
	AC_MSG_RESULT([$MODSIGN_HASHALGO])



	AC_ARG_WITH(signing-key,
		AS_HELP_STRING([--with-signing-key=<trusted.priv>], [Specify module signing key location]),
		[MODSIGN_PRIVFILE="$withval"])
	AC_ARG_WITH(signing-cert,
		AS_HELP_STRING([--with-signing-cert=<trusted.der>], [Specify module signing cert location]),
		[MODSIGN_CERTFILE="$withval"])

	HASPRIVFILE=$(test "$MODSIGN_PRIVFILE" = "yes" -o -z "$MODSIGN_PRIVFILE" && echo 0 || echo 1)
	HASCERTFILE=$(test "$MODSIGN_CERTFILE" = "yes" -o -z "$MODSIGN_CERTFILE" && echo 0 || echo 1)
	if test "$HASPRIVFILE" -ne "$HASCERTFILE"; then
		AC_MSG_ERROR([Options '--with-signing-key' and '--with-signing-cert' must either both be set or both be unset.])
	elif test "$HASPRIVFILE" -eq 1; then
		# Try to get absolute path, if possible
		MODSIGN_PRIVFILE=$(WACOM_LINUX_FILE_IF_EXISTS([$MODSIGN_PRIVFILE]) || echo "$MODSIGN_PRIVFILE")
		MODSIGN_CERTFILE=$(WACOM_LINUX_FILE_IF_EXISTS([$MODSIGN_CERTFILE]) || echo "$MODSIGN_PRIVFILE")
	else
		WACOM_LINUX_CHECK_KEYCERT([kernel autogenerated], $WCM_KERNEL_DIR/$(WACOM_LINUX_READ_CONFIG([CONFIG_MODULE_SIG_KEY])), [$WCM_KERNEL_DIR/certs/signing_key.x509])
		WACOM_LINUX_CHECK_KEYCERT([shim MOK], [/var/lib/shim-signed/mok/MOK.priv], [/var/lib/shim-signed/mok/MOK.der])
		WACOM_LINUX_CHECK_KEYCERT([rEFInd MOK], [/etc/refind.d/keys/refind_local.key], [/etc/refind.d/keys/refind_local.cer])
	fi

	AC_MSG_CHECKING(for module signing key)
	AC_MSG_RESULT([$MODSIGN_PRIVFILE])
	AC_MSG_CHECKING(for module signing certificate)
	AC_MSG_RESULT([$MODSIGN_CERTFILE])

	if test -z "$MODSIGN_PRIVFILE" -o -z "$MODSIGN_CERTFILE"; then
		if test "$MODSIGN_ENABLE" = "yes"; then
			AC_MSG_ERROR([Unable to honor explicit request for module signing.])
		fi

		if test "$MODSIGN_REQUIRED" = "yes"; then
			AC_MSG_ERROR([Kernel is configured to only load dnl
signed modules but we are unable to produce a signed module. Either (1) dnl
re-run configure with the options '--with-signing-key=<key>' and dnl
'--with-signing-cert=<cert>' set, (2) indicate you will sign the modules dnl
yourself by re-running configure with the '--disable-module-signing' dnl
option set, or (3) disable the kernel's signed module requirement (e.g. dnl
disable secure boot).])
		elif test "$MODSIGN_REQUIRED" = "recommended"; then
			AC_MSG_WARN([Kernel module signing is recommended dnl
but we are unable to find signing keys. Consider re-running configure dnl
with the options '--with-signing-key=<key>' and dnl
'--with-signing-cert=<cert>' set.])
		fi
	elif test ! -f "$MODSIGN_PRIVFILE" -o ! -f "$MODSIGN_CERTFILE"; then
		AC_MSG_WARN([Unable to read signing key or certificate. dnl
Please be sure the files exist before running "make install".])
	fi
fi


dnl Separate test output from file-generation output
echo 

WCM_SRC_SUBDIRS=". $WCM_KERNEL_VER"
AC_SUBST(WCM_KERNEL_DIR)
AC_SUBST(WCM_KERNEL_VER)
AC_SUBST(MODUTS)
AC_SUBST(MODSIGN_HASHALGO)
AC_SUBST(MODSIGN_PRIVFILE)
AC_SUBST(MODSIGN_CERTFILE)

AC_CONFIG_FILES([Makefile
                 4.18/Makefile])
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

