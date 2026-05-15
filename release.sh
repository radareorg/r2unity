#!/bin/sh

# Usage: sys/release-notes.sh 4.5.1      # from HEAD to 4.5.1
#   $ sys/release-notes.sh 4.5.1 -v      # same as above but include untagged commits
#   $ sys/release-notes.sh 4.5.0 4.5.1   # from 4.5.0 to 4.5.1
#

release_notes() {
if [ -n "`git log -n 1 | grep Release`" ]; then
	VERS=`git tag --sort=committerdate | grep -v conti | tail -n 1`
	PREV=`git tag --sort=committerdate | grep -v conti | tail -n 2 | head -n1`
else
	VERS=HEAD
	PREV=`git tag --sort=committerdate | grep -v conti | tail -n 1`
fi

[ -n "$1" ] && PREV="$1"
[ -n "$2" ] && VERS="$2"
[ -z "$PREV" ] && PREV="$(git rev-list --max-parents=0 HEAD)"


git log ${PREV}..${VERS} > .l
# When HEAD contains a tag do this magic
if [ ! -s .l ]; then
  VERS=$PREV
  PREV=`git tag --sort=committerdate | grep -v conti | tail -n 2 | head -n1`
  git log ${PREV}..${VERS} > .l
fi
grep ^Author .l | cut -d : -f 2- | sed -e 's,radare,pancake,' | sort -u > .A
CODENAME="`git log | grep -i 'Release' | head -n 1 | cut -d - -f 2- | cut -d ' ' -f 3-`"

echo "## Release Notes"
echo
echo "Codename: ${CODENAME}"
echo "Version: ${VERS}"
echo "Previous: ${PREV}"
printf "Commits: "
grep ^commit .l | wc -l | xargs echo
echo "Contributors: `wc -l .A | awk '{print $1}'`"
echo
echo "## Highlights"

echo "<details><summary>More details</summary><p>"
echo
echo "## Contributors"
echo
cat .A | perl -ne '/([^<]+)(.*)$/;$a=$1;$b=$2;$a=~s/^\s+|\s+$//g;$b=~s/[<>\s]//g;print "[$a](mailto:$b) "'
echo
echo

echo "## Changes"
echo
cat .l | grep -v ^commit | grep -v ^Author | grep -v ^Date > .x
cat .x | grep '##' | perl -ne '/##([^ ]*)/; if ($1) {print "$1\n";}' | sort -u > .y
for a in `cat .y` ; do
	echo "**$a**"
	echo
	cat .x | grep "##$a" | sed -e 's/##.*//g' | perl -ne '{ s/^\s+//; print "* $_"; }'
	echo
done

echo "## Raw"
cat .x | grep -v '##' | sed -e 's,^ *,,g' | grep -v "^$" | \
      perl -ne 'if (/^\*/) { print "$_"; } else { print "* $_";}'
echo
rm -f .x .y .l .A

echo '</p></details>'
}

release_version() {
	if [ -z "$1" ]; then
		cat meson.build | grep version | cut -d "'" -f2 | head -n1
	else
		meson rewrite kwargs set project / version "$1"
	fi
}

help_message() {
	echo "Usage ./release.sh [-v|-n] (version | notes)"
	echo "Change app version with './release.sh -v 1.2.3'"
}

case "$1" in
-n|notes) release_notes ; ;;
-v|version) release_version "$2" ; ;;
*) help_message ; ;;
esac
