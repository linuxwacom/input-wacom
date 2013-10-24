AC_PREREQ(2.60)
AC_INIT([input-wacom],
	[0.19.1])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([dist-bzip2])
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

dnl Check for kernel build environment
AC_ARG_WITH(kernel,
	AS_HELP_STRING([--with-kernel=dir], [Specify kernel source directory]),
	[WCM_KERNEL_DIR="$withval"])

AC_MSG_CHECKING(for kernel source/headers)
if test "$WCM_KERNEL_DIR" = "yes" -o -z "$WCM_KERNEL_DIR"; then
	WCM_KERNEL_DIR=
	dnl Kernel source not specified, guess where it is
	for i in /lib/modules/`uname -r`/build /usr/src/linux \
		/usr/src/linux-`uname -r` /usr/src/linux-2.6; do
		if test -f "$i/.config"; then
			WCM_KERNEL_DIR=$i
			WCM_ENV_KERNEL=yes
			AC_MSG_RESULT($WCM_KERNEL_DIR)
			break
		fi
	done

	if test "$WCM_ENV_KERNEL" != "yes"; then
		for i in /lib/modules/`uname -r`/source /lib/modules/`uname -r`/build \
			/usr/src/linux /usr/src/linux-`uname -r` \
			/usr/src/linux-2.6; do
			if test -f "$i/include/linux/input.h"; then
				WCM_KERNEL_DIR=$i
				WCM_ENV_KERNEL=yes
				AC_MSG_RESULT($WCM_KERNEL_DIR)
				break
			fi
		done
	fi

	if test -z "$WCM_KERNEL_DIR"; then
		AC_MSG_RESULT(not found)
		echo "***"
		echo "*** WARNING:"
		echo "*** Unable to guess kernel source directory"
		echo "***   Looked at /lib/modules/`uname -r`/source, /lib/modules/`uname -r`/build,"
		echo "***     /usr/src/linux, /usr/src/linux-`uname -r`, and"
		echo "***     /usr/src/linux-2.6"
		echo "*** Kernel modules will not be built"
		echo "***"
	fi
elif test "$WCM_KERNEL_DIR" != "no"; then
	if test -f "$WCM_KERNEL_DIR/include/linux/input.h"; then
		WCM_ENV_KERNEL=yes
		AC_MSG_RESULT([yes, $WCM_KERNEL_DIR])
	else
		AC_MSG_ERROR([Provided kernel path is invalid])
	fi
else
	WCM_KERNEL_DIR=
	AC_MSG_RESULT(no)
fi

dnl Check which version of the driver we should compile
if test -n "$WCM_KERNEL_DIR"; then
	AC_MSG_CHECKING(kernel version)
	MODUTS=
	for a in "$WCM_KERNEL_DIR/include/linux/version.h" \
		"$WCM_KERNEL_DIR/include/generated/utsrelease.h" \
		"$WCM_KERNEL_DIR/include/linux/utsrelease.h"; do
		if test -f $a; then
			MODUTS=`grep UTS_RELEASE $a | sed -e 's/^[[^"]]*"\([[^"]]*\).*$/\1/g'`
		fi
		test -n "$MODUTS" && break
	done
	if test -z "$MODUTS"; then
		AC_MSG_WARN(unable to identify kernel version)
	else
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
			if test $MINOR -ge 7; then
				WCM_KERNEL_VER="3.7"
			else
				WCM_KERNEL_VER="2.6.38"
			fi
		else
			AC_MSG_WARN(kernel version $MODUTS not supported)
		fi
	fi
fi

dnl Separate test output from file-generation output
echo 

WCM_SRC_SUBDIRS=". $WCM_KERNEL_VER"
AC_SUBST(WCM_KERNEL_DIR)
AC_SUBST(WCM_KERNEL_VER)
AC_SUBST(WCM_XFREE86_DIR)

AC_CONFIG_FILES([Makefile
                 2.6.30/Makefile
                 2.6.36/Makefile
                 2.6.38/Makefile
                 3.7/Makefile])
AC_OUTPUT

make

echo ""
echo "----------------------------------------"
echo "  BUILD ENVIRONMENT:"
echo "       linux kernel - $WCM_ISLINUX $WCM_KERNEL_VER"
echo "      kernel source - $WCM_ENV_KERNEL $WCM_KERNEL_DIR"
echo ""

if test "$WCM_ENV_KERNEL" = "yes"; then
	echo "Your wacom.ko is available under "
	echo "    `pwd`/$WCM_KERNEL_VER"
	echo "If you have an USB device, you can copy the driver by:"
	echo "	cp $WCM_KERNEL_VER/wacom.ko /lib/modules/`uname -r`/kernel/drivers/input/tablet"
	echo "If you have a serial device, please copy the driver by:"
	echo "	cp $WCM_KERNEL_VER/wacom_w8001.ko /lib/modules/`uname -r`/kernel/drivers/input/touchscreen"
	echo ""
	echo "NOTE: The kernel drivers included in this package are only"
	echo "tested with the X Wacom driver built from xf86-input-wacom."
	echo " If you are running an X server version older than 1.7, "
	echo "please use the drivers provided by linuxwacom package."
else
	echo "We could not find the kernel development environment to build the driver."
	echo "Please install the kernel source or the kernel development package and try again."
fi
echo ""

