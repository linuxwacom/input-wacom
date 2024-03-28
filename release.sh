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
#			Function: check_for_jq
#------------------------------------------------------------------------------
#
check_for_jq() {
    command -v jq >/dev/null 2>&1 || { echo >&2 "This script requires jq but it is not installed. Exiting."; exit 1;}
}

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
    if [ -z "$arg" ]; then
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
#			Function: check_json_message
#------------------------------------------------------------------------------
#
# if we get json with a "message" from github there was an error
# $1 the JSON to parse
check_json_message() {

    message=`echo $1 | jq ".message"`
    if [ "$message" != "null" ] ; then
        echo "Github release error: $1"
        exit 1
    fi
}

#------------------------------------------------------------------------------
#			Function: release_to_github
#------------------------------------------------------------------------------
#
release_to_github() {
    # Creating a release on Github automatically creates a tag.

    # dependency 'jq' for reading the json github sends us back

    # note git_username should include the suffix ":KEY" if the user has enabled 2FA
    # example skomra:de0e4dc3efbf2d008053027708227b365b7f80bf

    GH_REPO="linuxwacom"
    PROJECT="$1"
    release_description="Temporary Empty Release Description"
    release_descr=$(jq -n --arg release_description "$release_description" '$release_description')

    # Create a Release
    api_json=$(printf '{"tag_name": "%s",
                        "target_commitish": "master",
                        "name": "%s",
                        "body": %s,
                        "draft": false,
                        "prerelease": false}' "$tar_name" "$tar_name" "$release_descr")
    create_result=$(curl -s --data "$api_json" \
        -H "Accept: application/vnd.github+json" \
        -H "Authorization: Bearer $TOKEN" \
        https://api.github.com/repos/$GH_REPO/$PROJECT/releases)
    GH_RELEASE_ID=`echo $create_result | jq '.id'`

    check_json_message "$create_result"

    # Upload the tar to the release
    upload_result=$(curl -s \
        -H "Content-Type: application/x-bzip" \
        -H "Accept: application/vnd.github+json" \
        -H "Authorization: Bearer $TOKEN" \
        --data-binary @$tarbz2 \
        "https://uploads.github.com/repos/$GH_REPO/$PROJECT/releases/$GH_RELEASE_ID/assets?name=$tarbz2")
    DL_URL=`echo $upload_result | jq -r '.browser_download_url'`

    check_json_message "$upload_result"

    # Upload the sig to the release
    sig_result=$(curl -s \
        -H "Accept: application/vnd.github+json" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/pgp-signature" \
        --data-binary @$tarbz2.sig \
        "https://uploads.github.com/repos/$GH_REPO/$PROJECT/releases/$GH_RELEASE_ID/assets?name=$tarbz2.sig")
    PGP_URL=`echo $sig_result | jq -r '.browser_download_url'`

    check_json_message "$sig_result"

    echo "Github release created"
}

#------------------------------------------------------------------------------
#			Function: generate_announce
#------------------------------------------------------------------------------
#
generate_announce()
{
    MD5SUM=`which md5sum || which gmd5sum`
    SHA1SUM=`which sha1sum || which gsha1sum`
    SHA256SUM=`which sha256sum || which gsha256sum`

    cat <<RELEASE
Subject: [ANNOUNCE] $pkg_name $pkg_version

`git log --no-merges "$tag_range" | git shortlog`

git tag: $tag_name

RELEASE

	cat <<RELEASE
$DL_URL
 MD5:  `$MD5SUM $tarbz2`
 SHA1: `$SHA1SUM $tarbz2`
 SHA256: `$SHA256SUM $tarbz2`
 PGP: $PGP_URL

RELEASE
}

#------------------------------------------------------------------------------
#			Function: read_modfile
#------------------------------------------------------------------------------
#
# Read the module names from the file and set a variable to hold them
# This will be the same interface as cmd line supplied modules
#
read_modfile() {

    if [ -n "$MODFILE" ]; then
	# Make sure the file is sane
	if [ ! -r "$MODFILE" ]; then
	    echo "Error: module file '$MODFILE' is not readable or does not exist."
	    exit 1
	fi
	# read from input file, skipping blank and comment lines
	while read line; do
	    # skip blank lines
	    if [ -z "$line" ]; then
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
    if [ -n "$NO_QUIT" ]; then
	if [ -n "$failed_modules" ]; then
	    epilog="========  Partial Completion"
	fi
    elif [ -n "$failed_modules" ]; then
	epilog="========  Stopped on Error"
    fi

    echo ""
    echo "$epilog `date`"

    # Report about modules that failed for one reason or another
    if [ -n "$failed_modules" ]; then
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
	    if [ -z "$NO_QUIT" ]; then
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
	module_url=`echo "$full_module_url" | $GREP -o -e "linuxwacom/.*"`
	if [ $? -eq 0 ]; then
	     module_url=`echo $module_url | cut -d'/' -f2,3`
	else
	    echo "Error: unable to locate a valid project url from \"$full_module_url\"."
	    echo "Cannot establish url as linuxwacom"
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

    if [ "$section" = "linuxwacom" ]; then
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
    if [ $? -ne 0 -o "$configNum" = "0" ]; then
	echo "Error: failed to locate config.status."
	echo "Has the module been configured?"
	return 1
    elif [ "$configNum" != "1" ]; then
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
    if [ "$remote_top_commit_sha" != "$local_top_commit_sha" ]; then
	echo "Error: the local top commit has not been pushed to the remote."
	local_top_commit_descr=`git log --oneline --max-count=1 $local_top_commit_sha`
	echo "       the local top commit is: \"$local_top_commit_descr\""

	if [ -z "$DRY_RUN" ]; then
	    cd $top_src
	    return 1
	fi
    fi

    # If a tag exists with the the tar name, ensure it is tagging the top commit
    # It may happen if the version set in configure.ac has been previously released
    tagged_commit_sha=`git  rev-list --max-count=1 $tag_name 2>/dev/null`
    if [ $? -eq 0 ]; then
	# Check if the tag is pointing to the top commit
	if [ "$tagged_commit_sha" != "$remote_top_commit_sha" ]; then
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
	if [ -z "$DRY_RUN" ]; then
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

    # Pushing the top commit tag to the remote repository
    if [ -z $DRY_RUN ]; then
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

    if [ -z $DRY_RUN ]; then
        release_to_github $pkg_name
    else
	echo "Info: skipped pushing release to github in dry-run mode."
    fi

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
    if [ -n "$tag_previous" ]; then
	# The top commit may not have been tagged in dry-run mode. Use commit.
	tag_range=$tag_previous..$local_top_commit_sha
    else
	tag_range=$tag_name
    fi
    generate_announce > "$tar_name.announce"
    echo "Info: [ANNOUNCE] template generated in \"$tar_name.announce\" file."
    echo "      Please edit the .announce file to add a description of what's interesting and then"
    echo "      pgp sign and send it."

    # --------- Update the "body" text of the Github release with the .announce file -----------------

    if [ -n "$GH_RELEASE_ID" ]; then
        # Read the announce email and then escape it as a string in order to add it to the JSON
        read -r -d '' release_description <"$tar_name.announce"
        release_descr=$(jq -n --arg release_description "$release_description" '$release_description')
        api_json=$(printf '{"tag_name": "%s",
                            "target_commitish": "master",
                            "name": "%s",
                            "body": %s,
                            "draft": false,
                            "prerelease": false}' "$tar_name" "$tar_name" "$release_descr")
        create_result=$(curl -s -X PATCH --data "$api_json" \
            -H "Accept: application/vnd.github+json" \
            -H "Authorization: Bearer $TOKEN" \
            https://api.github.com/repos/$GH_REPO/$PROJECT/releases/$GH_RELEASE_ID)

        check_json_message "$create_result"
        echo "Git shortlog posted to the release at Github, please edit the release to add a description of what's interesting."
    fi

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

Usage: $basename [options] [path...]

Where "path" is a relative path to a git module, including '.' (the default).

Options:
  --dist                 make 'dist' instead of 'distcheck'; use with caution
  --distcheck            Default, ignored for compatibility
  --dry-run              Does everything except tagging and uploading tarballs
  --help                 Display this help and exit successfully
  --modfile <file>       Release the git modules specified in <file>
  --no-quit              Do not quit after error; just print error message
  --token <tokenval>     GitHub personal access token value

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

# Check if the json parser 'jq' is installed
check_for_jq

# Choose which grep program to use (on Solaris, must be gnu grep)
if [ -z "$GREP" ] ; then
    if [ -x /usr/gnu/bin/grep ] ; then
	GREP=/usr/gnu/bin/grep
    else
	GREP=grep
    fi
fi

# Find path for GnuPG v2
if [ -z "$GPG" ] ; then
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
    # Do not quit after error; just print error message
    --no-quit)
	NO_QUIT=yes
	;;
    # Personal GitHub Access Token to create the release
    --token)
        TOKEN=$2
	shift
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
	if [ -n "${MODFILE}" ]; then
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
if [ -z "$INPUT_MODULES" ]; then
    echo ""
    echo "No modules specified, using \$PWD."
    INPUT_MODULES=" ."
fi

# Read the module file and normalize input in INPUT_MODULES
read_modfile

# Loop through each module to release
# Exit on error if --no-quit no specified
process_modules

# Print the epilog with final status
print_epilog
