#!/bin/sh
# Release script for input-wacom.
# This is essentially a copy of the X.Org util/modular/release.sh script
# with a few modified parameters. Also see xf86-input-wacom release.sh.

set -e

announce_list="linuxwacom-announce@lists.sourceforge.net"
module=input-wacom
user=${USER}@
host=shell.sourceforge.net
srv_path=/home/frs/project/l/li/linuxwacom/xf86-input-wacom/$module
webpath=sourceforge.net/projects/linuxwacom/files/xf86-input-wacom/$module
remote=origin

usage()
{
    cat <<HELP
Usage: `basename $0` [options] <tag_previous> <tag_current>

Options:
  --force       force overwritting an existing release
  --user <name> username on $host
  --help        this help message
  --ignore-local-changes        don't abort on uncommitted local changes
  --remote      git remote where the change should be pushed (default "origin")
HELP
}

abort_for_changes()
{
    cat <<ERR
Uncommitted changes found. Did you forget to commit? Aborting.
Use --ignore-local-changes to skip this check.
ERR
    exit 1
}

gen_announce_mail()
{
case "$tag_previous" in
initial)
	range="$tag_current"
	;;
*)
	range="$tag_previous".."$tag_current"
	;;
esac

MD5SUM=`which md5sum || which gmd5sum`
SHA1SUM=`which sha1sum || which gsha1sum`

    cat <<RELEASE
Subject: [ANNOUNCE] $module $version
To: $announce_list

`git log --no-merges "$range" | git shortlog`

git tag: $tag_current

http://$webpath/$tarbz2/download
MD5:  `cd $tarball_dir && $MD5SUM $tarbz2`
SHA1: `cd $tarball_dir && $SHA1SUM $tarbz2`

http://$webpath/$targz/download
MD5:  `cd $tarball_dir && $MD5SUM $targz`
SHA1: `cd $tarball_dir && $SHA1SUM $targz`

RELEASE
}

export LC_ALL=C

while [ $# != 0 ]; do
    case "$1" in
    --force)
        force="yes"
        shift
        ;;
    --help)
        usage
        exit 0
        ;;
    --user)
	shift
	user=$1@
	shift
	;;
    --ignore-local-changes)
        ignorechanges=1
        shift
        ;;
    --remote)
        shift
        remote=$1
        shift
        ;;
    --*)
        echo "error: unknown option"
        usage
        exit 1
        ;;
    *)
        tag_previous="$1"
        tag_current="$2"
        shift 2
        if [ $# != 0 ]; then
            echo "error: unknown parameter"
            usage
            exit 1
        fi
        ;;
    esac
done

# Check for uncommitted/queued changes.
if [ "x$ignorechanges" != "x1" ]; then
    set +e
    git diff --exit-code > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        abort_for_changes
    fi
    set -e
fi

# Check if the object has been pushed. Do do so
# 1. Check if the current branch has the object. If not, abort.
# 2. Check if the object is on $remote/branchname. If not, abort.
local_sha=`git rev-list -1 $tag_current`
current_branch=`git branch | grep "\*" | sed -e "s/\* //"`
set +e
git rev-list $current_branch | grep $local_sha > /dev/null
if [ $? -eq 1 ]; then
    echo "Cannot find tag '$tag_current' on current branch. Aborting."
    echo "Switch to the correct branch and re-run the script."
    exit 1
fi

revs=`git rev-list $remote/master..$current_branch | wc -l`
if [ $revs -ne 0 ]; then
    git rev-list $remote/master..$current_branch | grep $local_sha > /dev/null

    if [ $? -ne 1 ]; then
        echo "$remote/master doesn't have object $local_sha"
        echo "for tag '$tag_current'. Did you push branch first? Aborting."
        exit 1
    fi
fi
set -e

tarball_dir="$(dirname $(find . -name config.status))"
module="${tag_current%-*}"
if [ "x$module" = "x$tag_current" ]; then
    # version-number-only tag.
    pwd=`pwd`
    module=`basename $pwd`
    version="$tag_current"
else
    # module-and-version style tag
    version="${tag_current##*-}"
fi

detected_module=`grep 'PACKAGE = ' $tarball_dir/Makefile | sed 's|PACKAGE = ||'`
if [ -f $detected_module-$version.tar.bz2 ]; then
    module=$detected_module
fi

modulever=$module-$version
tarbz2="$modulever.tar.bz2"
targz="$modulever.tar.gz"
announce="$tarball_dir/$modulever.announce"

echo "checking parameters"
if ! [ -f "$tarball_dir/$tarbz2" ] ||
   ! [ -f "$tarball_dir/$targz" ] ||
     [ -z "$tag_previous" ]; then
    echo "error: incorrect parameters!"
    usage
    exit 1
fi

echo "checking for proper current dir"
if ! [ -d .git ]; then
    echo "error: do this from your git dir, weenie"
    exit 1
fi

echo "checking for an existing tag"
if ! git tag -l $tag_current >/dev/null; then
    echo "error: you must tag your release first!"
    exit 1
fi

echo "creating shell on sourceforge for $USER"
echo "Simply log out once you get to the prompt"
ssh ${user/%@},linuxwacom@shell.sourceforge.net create

echo "Sleeping for 30 seconds, because this sometimes helps against sourceforge's random authentication denials"
sleep 30

echo "checking for an existing release"
if ssh $user$host ls $srv_path/$module/$targz >/dev/null 2>&1 ||
ssh $user$host_people ls $srv_path/$module/$tarbz2 >/dev/null 2>&1; then
if [ "x$force" = "xyes" ]; then
echo "warning: overriding released file ... here be dragons."
else
echo "error: file already exists!"
exit 1
fi
fi

echo "generating announce mail template, remember to sign it"
gen_announce_mail >$announce
echo "    at: $announce"

echo "installing release into server"
scp $tarball_dir/$targz $tarball_dir/$tarbz2 $user$host:$srv_path

echo "pushing tag upstream"
git push $remote $tag_current

echo "All done. Please bump configure.ac to x.y.99 now"
