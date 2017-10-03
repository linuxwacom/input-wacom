#!/bin/bash
#
#		Creates and upload a git module tarball
#
# Release script for input-wacom.
# This is essentially a copy of the X.Org util/modular/release.sh script
# with a few modified parameters.
#
# Note on portability:
# This script is intended to run on any platform supported by X.Org.
# Basically, it should be able to run in a Bourne shell.
#
#

export LC_ALL=C

#------------------------------------------------------------------------------
#			Function: check_local_changes
#------------------------------------------------------------------------------
#
check_local_changes() {
    git diff --quiet HEAD > /dev/null 2>&1
    if [ $? -ne 0 ]; then
	echo ""
	echo "Uncommitted changes found. Did you forget to commit? Aborting."
	echo ""
	echo "You can perform a 'git stash' to save your local changes and"
	echo "a 'git stash apply' to recover them after the tarball release."
	echo "Make sure to rebuild and run 'make distcheck' again."
	echo ""
	echo "Alternatively, you can clone the module in another directory"
	echo "and run ./configure. No need to build if testing was finished."
	echo ""
	return 1
    fi
    return 0
}

#------------------------------------------------------------------------------
#			Function: check_option_args
#------------------------------------------------------------------------------
#
# perform sanity checks on cmdline args which require arguments
# arguments:
#   $1 - the option being examined
#   $2 - the argument to the option
# returns:
#   if it returns, everything is good
#   otherwise it exit's
check_option_args() {
    option=$1
    arg=$2

    # check for an argument
    if [ x"$arg" = x ]; then
	echo ""
	echo "Error: the '$option' option is missing its required argument."
	echo ""
	usage
	exit 1
    fi

    # does the argument look like an option?
    echo $arg | $GREP "^-" > /dev/null
    if [ $? -eq 0 ]; then
	echo ""
	echo "Error: the argument '$arg' of option '$option' looks like an option itself."
	echo ""
	usage
	exit 1
    fi
}

#------------------------------------------------------------------------------
#			Function: check_modules_specification
#------------------------------------------------------------------------------
#
check_modules_specification() {

if [ x"$MODFILE" = x ]; then
    if [ x"${INPUT_MODULES}" = x ]; then
	echo ""
	echo "Error: no modules specified (blank command line)."
	usage
	exit 1
    fi
fi

}

#------------------------------------------------------------------------------
#			Function: generate_announce
#------------------------------------------------------------------------------
#
generate_announce()
{
    cat <<RELEASE
Subject: [ANNOUNCE] $pkg_name $pkg_version
To: $list_to
Reply-To: $list_cc

`git log --no-merges "$tag_range" | git shortlog`

git tag: $tag_name

RELEASE

    for tarball in $tarbz2 $targz $tarxz; do
	cat <<RELEASE
http://$host_current/$section_path/$tarball
MD5:  `$MD5SUM $tarball`
SHA1: `$SHA1SUM $tarball`
SHA256: `$SHA256SUM $tarball`
PGP:  http://${host_current}/${section_path}/${tarball}.sig

RELEASE
    done
}

#------------------------------------------------------------------------------
#			Function: read_modfile
#------------------------------------------------------------------------------
#
# Read the module names from the file and set a variable to hold them
# This will be the same interface as cmd line supplied modules
#
read_modfile() {

    if [ x"$MODFILE" != x ]; then
	# Make sure the file is sane
	if [ ! -r "$MODFILE" ]; then
	    echo "Error: module file '$MODFILE' is not readable or does not exist."
	    exit 1
	fi
	# read from input file, skipping blank and comment lines
	while read line; do
	    # skip blank lines
	    if [ x"$line" = x ]; then
		continue
	    fi
	    # skip comment lines
	    if echo "$line" | $GREP -q "^#" ; then
		continue;
	    fi
	    INPUT_MODULES="$INPUT_MODULES $line"
	done <"$MODFILE"
    fi
    return 0
}

#------------------------------------------------------------------------------
#			Function: print_epilog
#------------------------------------------------------------------------------
#
print_epilog() {

    epilog="========  Successful Completion"
    if [ x"$NO_QUIT" != x ]; then
	if [ x"$failed_modules" != x ]; then
	    epilog="========  Partial Completion"
	fi
    elif [ x"$failed_modules" != x ]; then
	epilog="========  Stopped on Error"
    fi

    echo ""
    echo "$epilog `date`"

    # Report about modules that failed for one reason or another
    if [ x"$failed_modules" != x ]; then
	echo "	List of failed modules:"
	for mod in $failed_modules; do
	    echo "	$mod"
	done
	echo "========"
	echo ""
    fi
}

#------------------------------------------------------------------------------
#			Function: process_modules
#------------------------------------------------------------------------------
#
# Loop through each module to release
# Exit on error if --no-quit was not specified
#
process_modules() {
    for MODULE_RPATH in ${INPUT_MODULES}; do
	if ! process_module ; then
	    echo "Error: processing module \"$MODULE_RPATH\" failed."
	    failed_modules="$failed_modules $MODULE_RPATH"
	    if [ x"$NO_QUIT" = x ]; then
		print_epilog
		exit 1
	    fi
	fi
    done
}

#------------------------------------------------------------------------------
#			Function: get_section
#------------------------------------------------------------------------------
# Code 'return 0' on success
# Code 'return 1' on error
# Sets global variable $section
get_section() {
    local module_url
    local full_module_url

    # Obtain the git url in order to find the section to which this module belongs
    full_module_url=`git config --get remote.$remote_name.url | sed 's:\.git$::'`
    if [ $? -ne 0 ]; then
	echo "Error: unable to obtain git url for remote \"$remote_name\"."
	return 1
    fi

    # The last part of the git url will tell us the section. Look for xorg first
    echo "$full_module_url"
    module_url=`echo "$full_module_url" | $GREP -o "/xorg/.*"`
    if [ $? -eq 0 ]; then
	module_url=`echo $module_url | cut -d'/' -f3,4`
    else
	# The look for mesa, xcb, etc...
	module_url=`echo "$full_module_url" | $GREP -o -e "/mesa/.*" -e "/xcb/.*" -e "/xkeyboard-config" -e "/nouveau/xf86-video-nouveau" -e "/libevdev" -e "/wayland/.*" -e "/evemu" -e "/linuxwacom/.*"`
	if [ $? -eq 0 ]; then
	     module_url=`echo $module_url | cut -d'/' -f2,3`
	else
	    echo "Error: unable to locate a valid project url from \"$full_module_url\"."
	    echo "Cannot establish url as one of linuxwacom, xorg, mesa, xcb, xf86-video-nouveau, xkeyboard-config or wayland"
	    cd $top_src
	    return 1
	fi
    fi

    # Find the section (subdirs) where the tarballs are to be uploaded
    # The module relative path can be app/xfs, xserver, or mesa/drm for example
    section=`echo $module_url | cut -d'/' -f1`
    if [ $? -ne 0 ]; then
	echo "Error: unable to extract section from $module_url first field."
	return 1
    fi

    if [ x"$section" = xlinuxwacom ]; then
	section=`echo $module_url | cut -d'/' -f2`
	if [ $? -ne 0 ]; then
	    echo "Error: unable to extract section from $module_url second field."
	    return 1
	fi
    fi

    return 0
}

#                       Function: sign_or_fail
#------------------------------------------------------------------------------
#
# Sign the given file, if any
# Output the name of the signature generated to stdout (all other output to
# stderr)
# Return 0 on success, 1 on fail
#
sign_or_fail() {
    if [ -n "$1" ]; then
	sig=$1.sig
	rm -f $sig
	$GPG -b $1 1>&2
	if [ $? -ne 0 ]; then
	    echo "Error: failed to sign $1." >&2
	    return 1
	fi
	echo $sig
    fi
    return 0
}

#------------------------------------------------------------------------------
#			Function: process_module
#------------------------------------------------------------------------------
# Code 'return 0' on success to process the next module
# Code 'return 1' on error to process next module if invoked with --no-quit
#
process_module() {

    top_src=`pwd`
    echo ""
    echo "========  Processing \"$top_src/$MODULE_RPATH\""

    # This is the location where the script has been invoked
    if [ ! -d $MODULE_RPATH ] ; then
	echo "Error: $MODULE_RPATH cannot be found under $top_src."
	return 1
    fi

    # Change directory to be in the git module
    cd $MODULE_RPATH
    if [ $? -ne 0 ]; then
	echo "Error: failed to cd to $MODULE_RPATH."
	return 1
    fi

    # ----- Now in the git module *root* directory ----- #

    # Check that this is indeed a git module
    if [ ! -d .git ]; then
	echo "Error: there is no git module here: `pwd`"
	return 1
    fi

    # Change directory to be in the git build directory (could be out-of-source)
    # More than one can be found when distcheck has run and failed
    configNum=`find . -name config.status -type f | wc -l | sed 's:^ *::'`
    if [ $? -ne 0 ]; then
	echo "Error: failed to locate config.status."
	echo "Has the module been configured?"
	return 1
    fi
    if [ x"$configNum" = x0 ]; then
	echo "Error: failed to locate config.status, has the module been configured?"
	return 1
    fi
    if [ x"$configNum" != x1 ]; then
	echo "Error: more than one config.status file was found,"
	echo "       clean-up previously failed attempts at distcheck"
	return 1
    fi
    status_file=`find . -name config.status -type f`
    if [ $? -ne 0 ]; then
	echo "Error: failed to locate config.status."
	echo "Has the module been configured?"
	return 1
    fi
    build_dir=`dirname $status_file`
    cd $build_dir
    if [ $? -ne 0 ]; then
	echo "Error: failed to cd to $MODULE_RPATH/$build_dir."
	cd $top_src
	return 1
    fi

    # ----- Now in the git module *build* directory ----- #

    # Check for uncommitted/queued changes.
    check_local_changes
    if [ $? -ne 0 ]; then
	cd $top_src
	return 1
    fi

    # Determine what is the current branch and the remote name
    current_branch=`git branch | $GREP "\*" | sed -e "s/\* //"`
    remote_name=`git config --get branch.$current_branch.remote`
    remote_branch=`git config --get branch.$current_branch.merge | cut -d'/' -f3,4`
    echo "Info: working off the \"$current_branch\" branch tracking the remote \"$remote_name/$remote_branch\"."

    # Obtain the section
    get_section
    if [ $? -ne 0 ]; then
	cd $top_src
	return 1
    fi

    # Run 'make dist/distcheck' to ensure the tarball matches the git module content
    # Important to run make dist/distcheck before looking in Makefile, may need to reconfigure
    echo "Info: running \"make $MAKE_DIST_CMD\" to create tarballs:"
    ${MAKE} $MAKEFLAGS $MAKE_DIST_CMD > /dev/null
    if [ $? -ne 0 ]; then
	echo "Error: \"$MAKE $MAKEFLAGS $MAKE_DIST_CMD\" failed."
	cd $top_src
	return 1
    fi

    # Find out the tarname from the makefile
    pkg_name=`$GREP '^PACKAGE = ' Makefile | sed 's|PACKAGE = ||'`
    pkg_version=`$GREP '^VERSION = ' Makefile | sed 's|VERSION = ||'`
    tar_name="$pkg_name-$pkg_version"
    targz=$tar_name.tar.gz
    tarbz2=$tar_name.tar.bz2
    tarxz=$tar_name.tar.xz

    [ -e $targz ] && ls -l $targz || unset targz
    [ -e $tarbz2 ] && ls -l $tarbz2 || unset tarbz2
    [ -e $tarxz ] && ls -l $tarxz || unset tarxz

    if [ -z "$targz" -a -z "$tarbz2" -a -z "$tarxz" ]; then
	echo "Error: no compatible tarballs found."
	cd $top_src
	return 1
    fi

    tag_name="$tar_name"

    gpgsignerr=0
    siggz="$(sign_or_fail ${targz})"
    gpgsignerr=$((${gpgsignerr} + $?))
    sigbz2="$(sign_or_fail ${tarbz2})"
    gpgsignerr=$((${gpgsignerr} + $?))
    sigxz="$(sign_or_fail ${tarxz})"
    gpgsignerr=$((${gpgsignerr} + $?))
    if [ ${gpgsignerr} -ne 0 ]; then
        echo "Error: unable to sign at least one of the tarballs."
        cd $top_src
        return 1
    fi

    # Obtain the top commit SHA which should be the version bump
    # It should not have been tagged yet (the script will do it later)
    local_top_commit_sha=`git  rev-list --max-count=1 HEAD`
    if [ $? -ne 0 ]; then
	echo "Error: unable to obtain the local top commit id."
	cd $top_src
	return 1
    fi

    # Check that the top commit looks like a version bump
    git diff --unified=0 HEAD^ | $GREP -F $pkg_version >/dev/null 2>&1
    if [ $? -ne 0 ]; then
	# Wayland repos use  m4_define([wayland_major_version], [0])
	git diff --unified=0 HEAD^ | $GREP -E "(major|minor|micro)_version" >/dev/null 2>&1
	if [ $? -ne 0 ]; then
	    echo "Error: the local top commit does not look like a version bump."
	    echo "       the diff does not contain the string \"$pkg_version\"."
	    local_top_commit_descr=`git log --oneline --max-count=1 $local_top_commit_sha`
	    echo "       the local top commit is: \"$local_top_commit_descr\""

	    if [ x"$DRY_RUN" = x ]; then
	        cd $top_src
	        return 1
	    fi
	fi
    fi

    # Check that the top commit has been pushed to remote
    remote_top_commit_sha=`git  rev-list --max-count=1 $remote_name/$remote_branch`
    if [ $? -ne 0 ]; then
	echo "Error: unable to obtain top commit from the remote repository."
	cd $top_src
	return 1
    fi
    if [ x"$remote_top_commit_sha" != x"$local_top_commit_sha" ]; then
	echo "Error: the local top commit has not been pushed to the remote."
	local_top_commit_descr=`git log --oneline --max-count=1 $local_top_commit_sha`
	echo "       the local top commit is: \"$local_top_commit_descr\""

	if [ x"$DRY_RUN" = x ]; then
	    cd $top_src
	    return 1
	fi
    fi

    # If a tag exists with the the tar name, ensure it is tagging the top commit
    # It may happen if the version set in configure.ac has been previously released
    tagged_commit_sha=`git  rev-list --max-count=1 $tag_name 2>/dev/null`
    if [ $? -eq 0 ]; then
	# Check if the tag is pointing to the top commit
	if [ x"$tagged_commit_sha" != x"$remote_top_commit_sha" ]; then
	    echo "Error: the \"$tag_name\" already exists."
	    echo "       this tag is not tagging the top commit."
	    remote_top_commit_descr=`git log --oneline --max-count=1 $remote_top_commit_sha`
	    echo "       the top commit is: \"$remote_top_commit_descr\""
	    local_tag_commit_descr=`git log --oneline --max-count=1 $tagged_commit_sha`
	    echo "       tag \"$tag_name\" is tagging some other commit: \"$local_tag_commit_descr\""
	    cd $top_src
	    return 1
	else
	    echo "Info: module already tagged with \"$tag_name\"."
	fi
    else
	# Tag the top commit with the tar name
	if [ x"$DRY_RUN" = x ]; then
	    git tag -s -m $tag_name $tag_name
	    if [ $? -ne 0 ]; then
		echo "Error:  unable to tag module with \"$tag_name\"."
		cd $top_src
		return 1
	    else
		echo "Info: module tagged with \"$tag_name\"."
	    fi
	else
	    echo "Info: skipping the commit tagging in dry-run mode."
	fi
    fi

    # --------- Now the tarballs are ready to upload ----------

    # Some hostnames are also used as /srv subdirs
    host_linuxwacom="shell.sourceforge.net"

    section_path=archive/individual/$section
    srv_path="/srv/$host_current/$section_path"

    if [ x"$section" = xxf86-input-wacom ] ||
       [ x"$section" = xinput-wacom ] ||
       [ x"$section" = xlibwacom ]; then
        # input-wacom files are in a subdirectory for whatever reason
        if [ x"$section" = xinput-wacom ]; then
            section="xf86-input-wacom/input-wacom"
        fi

        hostname=$host_linuxwacom
        host_current="sourceforge.net"
        section_path="projects/linuxwacom/files/$section"
        srv_path="/home/frs/project/linuxwacom/$section"
        list_to="linuxwacom-announce@lists.sourceforge.net"
        list_cc="linuxwacom-discuss@lists.sourceforge.net"

        echo "creating shell on sourceforge for $USER"
        ssh ${USER_NAME%@},linuxwacom@$hostname create
        #echo "Simply log out once you get to the prompt"
        #ssh -t ${USER_NAME%@},linuxwacom@$hostname create
        #echo "Sleeping for 30 seconds, because this sometimes helps against sourceforge's random authentication denials"
        #sleep 30
    fi

    # Use personal web space on the host for unit testing (leave commented out)
    # srv_path="~/public_html$srv_path"

    # Check that the server path actually does exist
    ssh $USER_NAME$hostname ls $srv_path >/dev/null 2>&1 ||
    if [ $? -ne 0 ]; then
	echo "Error: the path \"$srv_path\" on the web server does not exist."
	cd $top_src
	return 1
    fi

    # Check for already existing tarballs
    for tarball in $targz $tarbz2 $tarxz; do
	ssh $USER_NAME$hostname ls $srv_path/$tarball  >/dev/null 2>&1
	if [ $? -eq 0 ]; then
	    if [ "x$FORCE" = "xyes" ]; then
		echo "Warning: overwriting released tarballs due to --force option."
	    else
		echo "Error: tarball $tar_name already exists. Use --force to overwrite."
		cd $top_src
		return 1
	    fi
	fi
    done

    # Upload to host using the 'scp' remote file copy program
    if [ x"$DRY_RUN" = x ]; then
	echo "Info: uploading tarballs to web server:"
	scp $targz $tarbz2 $tarxz $siggz $sigbz2 $sigxz $USER_NAME$hostname:$srv_path
	if [ $? -ne 0 ]; then
	    echo "Error: the tarballs uploading failed."
	    cd $top_src
	    return 1
	fi
    else
	echo "Info: skipping tarballs uploading in dry-run mode."
	echo "      \"$srv_path\"."
    fi

    # Pushing the top commit tag to the remote repository
    if [ x$DRY_RUN = x ]; then
	echo "Info: pushing tag \"$tag_name\" to remote \"$remote_name\":"
	git push $remote_name $tag_name
	if [ $? -ne 0 ]; then
	    echo "Error: unable to push tag \"$tag_name\" to the remote repository."
	    echo "       it is recommended you fix this manually and not run the script again"
	    cd $top_src
	    return 1
	fi
    else
	echo "Info: skipped pushing tag \"$tag_name\" to the remote repository in dry-run mode."
    fi

    MD5SUM=`which md5sum || which gmd5sum`
    SHA1SUM=`which sha1sum || which gsha1sum`
    SHA256SUM=`which sha256sum || which gsha256sum`

    # --------- Generate the announce e-mail ------------------
    # Failing to generate the announce is not considered a fatal error

    # Git-describe returns only "the most recent tag", it may not be the expected one
    # However, we only use it for the commit history which will be the same anyway.
    tag_previous=`git describe --abbrev=0 HEAD^ 2>/dev/null`
    # Git fails with rc=128 if no tags can be found prior to HEAD^
    if [ $? -ne 0 ]; then
	if [ $? -ne 0 ]; then
	    echo "Warning: unable to find a previous tag."
	    echo "         perhaps a first release on this branch."
	    echo "         Please check the commit history in the announce."
	fi
    fi
    if [ x"$tag_previous" != x ]; then
	# The top commit may not have been tagged in dry-run mode. Use commit.
	tag_range=$tag_previous..$local_top_commit_sha
    else
	tag_range=$tag_name
    fi
    generate_announce > "$tar_name.announce"
    echo "Info: [ANNOUNCE] template generated in \"$tar_name.announce\" file."
    echo "      Please pgp sign and send it."

    # --------- Successful completion --------------------------
    cd $top_src
    return 0

}

#------------------------------------------------------------------------------
#			Function: usage
#------------------------------------------------------------------------------
# Displays the script usage and exits successfully
#
usage() {
    basename="`expr "//$0" : '.*/\([^/]*\)'`"
    cat <<HELP

Usage: $basename [options] path...

Where "path" is a relative path to a git module, including '.'.

Options:
  --dist              make 'dist' instead of 'distcheck'; use with caution
  --distcheck         Default, ignored for compatibility
  --dry-run           Does everything except tagging and uploading tarballs
  --force             Force overwriting an existing release
  --help              Display this help and exit successfully
  --modfile <file>    Release the git modules specified in <file>
  --moduleset <file>  The jhbuild moduleset full pathname to be updated
  --no-quit           Do not quit after error; just print error message
  --user <name>@      Username of your fdo account if not configured in ssh

Environment variables defined by the "make" program and used by release.sh:
  MAKE        The name of the make command [make]
  MAKEFLAGS:  Options to pass to all \$(MAKE) invocations

HELP
}

#------------------------------------------------------------------------------
#			Script main line
#------------------------------------------------------------------------------
#

# Choose which make program to use (could be gmake)
MAKE=${MAKE:="make"}

# Choose which grep program to use (on Solaris, must be gnu grep)
if [ "x$GREP" = "x" ] ; then
    if [ -x /usr/gnu/bin/grep ] ; then
	GREP=/usr/gnu/bin/grep
    else
	GREP=grep
    fi
fi

# Find path for GnuPG v2
if [ "x$GPG" = "x" ] ; then
    if [ -x /usr/bin/gpg2 ] ; then
	GPG=/usr/bin/gpg2
    else
	GPG=gpg
    fi
fi

# Set the default make tarball creation command
MAKE_DIST_CMD=distcheck

# Process command line args
while [ $# != 0 ]
do
    case $1 in
    # Use 'dist' rather than 'distcheck' to create tarballs
    # You really only want to do this if you're releasing a module you can't
    # possibly build-test.  Please consider carefully the wisdom of doing so.
    --dist)
	MAKE_DIST_CMD=dist
	;;
    # Use 'distcheck' to create tarballs
    --distcheck)
	MAKE_DIST_CMD=distcheck
	;;
    # Does everything except uploading tarball
    --dry-run)
	DRY_RUN=yes
	;;
    # Force overwriting an existing release
    # Use only if nothing changed in the git repo
    --force)
	FORCE=yes
	;;
    # Display this help and exit successfully
    --help)
	usage
	exit 0
	;;
    # Release the git modules specified in <file>
    --modfile)
	check_option_args $1 $2
	shift
	MODFILE=$1
	;;
    # The jhbuild moduleset to update with relase info
    --moduleset)
	check_option_args $1 $2
	shift
	JH_MODULESET=$1
	;;
    # Do not quit after error; just print error message
    --no-quit)
	NO_QUIT=yes
	;;
    # Username of your fdo account if not configured in ssh
    --user)
	check_option_args $1 $2
	shift
	USER_NAME=$1
	;;
    --*)
	echo ""
	echo "Error: unknown option: $1"
	echo ""
	usage
	exit 1
	;;
    -*)
	echo ""
	echo "Error: unknown option: $1"
	echo ""
	usage
	exit 1
	;;
    *)
	if [ x"${MODFILE}" != x ]; then
	    echo ""
	    echo "Error: specifying both modules and --modfile is not permitted"
	    echo ""
	    usage
	    exit 1
	fi
	INPUT_MODULES="${INPUT_MODULES} $1"
	;;
    esac

    shift
done

# If no modules specified (blank cmd line) display help
check_modules_specification

# Read the module file and normalize input in INPUT_MODULES
read_modfile

# Loop through each module to release
# Exit on error if --no-quit no specified
process_modules

# Print the epilog with final status
print_epilog
