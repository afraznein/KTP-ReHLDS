#!/bin/bash

init()
{
	SOURCE_DIR="$@"
	GIT_DIR="$SOURCE_DIR"
	VERSION_FILE="$SOURCE_DIR/rehlds/version/version.h"
	APPVERSION_FILE="$SOURCE_DIR/rehlds/version/appversion.h"

	if test -z "`git --version`"; then
		echo "Please install git client"
		echo "sudo apt-get install git"
		exit -1
	fi

	# Read old version
	if [ -e "$APPVERSION_FILE" ]; then
		OLD_VERSION=$(cat "$APPVERSION_FILE" | grep -wi '#define APP_VERSION' | sed -e 's/#define APP_VERSION[ \t\r\n\v\f]\+\(.*\)/\1/i' -e 's/\r//g')
		if [ $? -ne 0 ]; then
			OLD_VERSION=""
		else
			# Remove quotes
			OLD_VERSION=$(echo $OLD_VERSION | xargs)
		fi
	fi

	# Get major, minor and maintenance information from gradle.properties
	MAJOR=$(cat "$VERSION_FILE" | grep -wi 'VERSION_MAJOR' | sed -e 's/.*VERSION_MAJOR.*[^0-9]\([0-9][0-9]*\).*/\1/i' -e 's/\r//g')
	if [ $? -ne 0 -o "$MAJOR" = "" ]; then
		MAJOR=0
	fi

	MINOR=$(cat "$VERSION_FILE" | grep -wi 'VERSION_MINOR' | sed -e 's/.*VERSION_MINOR.*[^0-9]\([0-9][0-9]*\).*/\1/i' -e 's/\r//g')
	if [ $? -ne 0 -o "$MINOR" = "" ]; then
		MINOR=0
	fi

	MAINTENANCE=$(cat "$VERSION_FILE" | grep -i 'VERSION_MAINTENANCE' | sed -e 's/.*VERSION_MAINTENANCE.*[^0-9]\([0-9][0-9]*\).*/\1/i' -e 's/\r//g')
	if [ $? -ne 0 -o "$MAINTENANCE" = "" ]; then
		MAINTENANCE=0
	fi

	check_git_provenance

	# The opt-out stamps what the build is, rather than a plausible-looking zero.
	if [ -n "$NO_GIT" ]; then
		COMMIT_COUNT=0
		COMMIT_SHA=nogit
		COMMIT_URL=
		NEW_VERSION="$MAJOR.$MINOR.$MAINTENANCE.0-nogit"
		if [ "$NEW_VERSION" != "$OLD_VERSION" ]; then
			update_appversion
		fi
		return 0
	fi

	BRANCH_NAME=$(git -C "$GIT_DIR/" rev-parse --abbrev-ref HEAD)
	if [ $? -ne 0 -o "$BRANCH_NAME" = "" ]; then
		BRANCH_NAME=master
	fi

	COMMIT_COUNT=$(git -C "$GIT_DIR/" rev-list --count $BRANCH_NAME)
	if [ $? -ne 0 -o "$COMMIT_COUNT" = "" ]; then
		COMMIT_COUNT=0
	fi

	#
	# Configure remote url repository
	#
	# Get remote name by current branch
	BRANCH_REMOTE=$(git -C "$GIT_DIR/" config branch.$BRANCH_NAME.remote)
	if [ $? -ne 0 -o "$BRANCH_REMOTE" = "" ]; then
		BRANCH_REMOTE=origin
	fi

	# Get commit id
	COMMIT_SHA=$(git -C "$GIT_DIR/" rev-parse --verify HEAD)
	COMMIT_SHA=${COMMIT_SHA:0:7}

	# Get remote url
	COMMIT_URL=$(git -C "$GIT_DIR/" config remote.$BRANCH_REMOTE.url)

	URL_CONSTRUCT=0

	if [[ "$COMMIT_URL" == *"git@"* ]]; then
		URL_CONSTRUCT=1

		# Strip prefix 'git@'
		COMMIT_URL=${COMMIT_URL#git@}

		# Strip postfix '.git'
		COMMIT_URL=${COMMIT_URL%.git}

		# Replace ':' to '/'
		COMMIT_URL=${COMMIT_URL/:/\/}

	elif [[ "$COMMIT_URL" == *"https://"* ]]; then
		URL_CONSTRUCT=1

		# Strip prefix 'https://'
		COMMIT_URL=${COMMIT_URL#https://}

		# Strip postfix '.git'
		COMMIT_URL=${COMMIT_URL%.git}
	fi

	if test "$URL_CONSTRUCT" -eq 1; then
		# Append extra string
		if [[ "$COMMIT_URL" == *"bitbucket.org"* ]]; then
			COMMIT_URL=$(echo https://$COMMIT_URL/commits/)
		else
			COMMIT_URL=$(echo https://$COMMIT_URL/commit/)
		fi
	fi

	#
	# Detect local modifications
	#
	if [ `git -C "$GIT_DIR/" ls-files -m | wc -l` = 0 ]; then
		MODIFIED=
	else
		MODIFIED=+m
	fi

	NEW_VERSION="$MAJOR.$MINOR.$MAINTENANCE.$COMMIT_COUNT-dev$MODIFIED"

	# Update appversion.h if version has changed or modifications/mixed revisions detected
	if [ "$NEW_VERSION" != "$OLD_VERSION" ]; then
		update_appversion
	fi
}

# A build that cannot read its own commit refuses. Every git call in init() has a
# silent fallback, so a provenance-blind engine md5-verifies identically to a good
# one, and a truncated history stamps a build number that looks real.
check_git_provenance()
{
	local probe_err probe_sha probe_count probe_shallow reason

	probe_err=$(git -C "$GIT_DIR/" rev-parse --verify HEAD 2>&1 >/dev/null)
	probe_sha=$(git -C "$GIT_DIR/" rev-parse --verify HEAD 2>/dev/null)
	probe_count=$(git -C "$GIT_DIR/" rev-list --count HEAD 2>/dev/null)

	# Blank on git older than 2.15, which the commit-count floor below still covers.
	probe_shallow=$(git -C "$GIT_DIR/" rev-parse --is-shallow-repository 2>/dev/null)

	# A depth-limited clone answers 1, so rejecting only 0 passes a truncated
	# history through as a build number nothing downstream can tell is wrong.
	if [ -n "$probe_sha" ] && [ "$probe_shallow" != "true" ] && \
	   [ -n "$probe_count" ] && [ "$probe_count" -gt 1 ]; then
		NO_GIT=
		return 0
	fi

	if [ -z "$probe_sha" ]; then
		reason="git could not name HEAD"
	elif [ "$probe_shallow" = "true" ]; then
		reason="shallow clone, so the commit count is truncated"
	else
		reason="commit count ${probe_count:-<none>} cannot be a real history"
	fi

	{
	echo ""
	echo "========================================"
	echo "[appversion] REFUSING: $reason"
	echo "========================================"
	echo "  source dir  : $SOURCE_DIR"
	echo "  HEAD sha    : ${probe_sha:-<none>}"
	echo "  commit count: ${probe_count:-<none>}"
	echo "  shallow     : ${probe_shallow:-false}"
	if [ -n "$probe_err" ]; then
		echo "  git said    : $probe_err"
	fi
	echo ""
	echo "Writing APP_VERSION with a build number that is 0, or 1 because the history"
	echo "was truncated, produces a working engine that cannot be traced to a commit,"
	echo "and its md5 verifies exactly like a good one. Refusing instead of guessing."
	echo ""
	echo "A SHALLOW clone is the quiet one: 'git rev-list --count' answers 1 rather"
	echo "than failing, so the build number looks real. Fetch the whole history --"
	echo "'git fetch --unshallow', or 'fetch-depth: 0' on actions/checkout."
	echo ""
	echo "A DOUBLED path in the git error above means a WSL build of a Windows git"
	echo "worktree: the worktree's .git is a file holding a Windows gitdir path, which"
	echo "git inside WSL reads as relative. Clone into the WSL filesystem and build"
	echo "there -- see CLAUDE.md, section 'Build provenance'."
	echo ""
	echo "For a real build outside any checkout (source tarball, vendored copy), set"
	echo "REHLDS_ALLOW_NO_GIT=1. Everything above is still true: the version is then"
	echo "stamped '-nogit' and the binary says so wherever it prints its version."
	echo ""
	} 1>&2

	# Permitted, never silent: the refusal above still prints, and the stamp ships.
	if [ -n "$REHLDS_ALLOW_NO_GIT" ]; then
		echo "[appversion] REHLDS_ALLOW_NO_GIT is set -- continuing with a -nogit stamp." 1>&2
		NO_GIT=1
		return 0
	fi

	exit 1
}

update_appversion()
{
	day=$(date +%d)
	year=$(date +%Y)
	hours=$(date +%H:%M:%S)
	month=$(LANG=en_us_88591; date +"%b")

	# Write appversion.h
	echo Updating appversion.h, new version is '"'$NEW_VERSION'"', the old one was $OLD_VERSION

	echo -e "#ifndef __APPVERSION_H__\r">"$APPVERSION_FILE"
	echo -e "#define __APPVERSION_H__\r">>"$APPVERSION_FILE"
	echo -e "\r">>"$APPVERSION_FILE"
	echo -e "//\r">>"$APPVERSION_FILE"
	echo -e "// This file is generated automatically.\r">>"$APPVERSION_FILE"
	echo -e "// Don't edit it.\r">>"$APPVERSION_FILE"
	echo -e "//\r">>"$APPVERSION_FILE"
	echo -e "\r">>"$APPVERSION_FILE"
	echo -e "// Version defines\r">>"$APPVERSION_FILE"
	echo -e '#define APP_VERSION "'$NEW_VERSION'"\r'>>"$APPVERSION_FILE"

	echo -e "#define APP_VERSION_C $MAJOR,$MINOR,$MAINTENANCE,$COMMIT_COUNT\r">>"$APPVERSION_FILE"
	echo -e '#define APP_VERSION_STRD "'$MAJOR.$MINOR.$MAINTENANCE.$COMMIT_COUNT'"\r'>>"$APPVERSION_FILE"
	echo -e "#define APP_VERSION_FLAGS 0x0L\r">>"$APPVERSION_FILE"
	echo -e "\r">>"$APPVERSION_FILE"
	echo -e '#define APP_COMMIT_DATE "'$month $day $year'"\r'>>"$APPVERSION_FILE"
	echo -e '#define APP_COMMIT_TIME "'$hours'"\r'>>"$APPVERSION_FILE"
	echo -e "\r">>"$APPVERSION_FILE"

	echo -e '#define APP_COMMIT_SHA "'$COMMIT_SHA'"\r'>>"$APPVERSION_FILE"
	echo -e '#define APP_COMMIT_URL "'$COMMIT_URL'"\r'>>"$APPVERSION_FILE"
	echo -e "\r">>"$APPVERSION_FILE"
	echo -e "#endif //__APPVERSION_H__\r">>"$APPVERSION_FILE"
}

# Initialise
init $*

# Exit normally
exit 0
