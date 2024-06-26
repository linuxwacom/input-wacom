SUBDIRS = @WCM_KERNEL_VER@

DIST_SUBDIRS = 4.18
EXTRA_DIST = git-version-gen \
             inputattach/inputattach.c inputattach/README \
	     inputattach/serio-ids.h

dist-hook:
	./git-version-gen > $(distdir)/version

# HACK: VPATH builds don't work at this time, so short-cicruit the
# distcheck target and directly create the dist tarball
distcheck: dist ;
