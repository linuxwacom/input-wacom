SUBDIRS = @WCM_KERNEL_VER@

DIST_SUBDIRS = 2.6.30 2.6.36 2.6.38 3.7 3.17
EXTRA_DIST = inputattach/inputattach.c inputattach/README \
	     inputattach/serio-ids.h

# HACK: VPATH builds don't work at this time, so short-cicruit the
# distcheck target and directly create the dist tarball
distcheck: dist ;
