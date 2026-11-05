#!/usr/bin/env bash
# Brings the newest OBS (upstream/master) into our master, rebuilds it, and
# stops at the first thing that needs a human. Run it often: small merges
# are quick, big ones are slow.
#
#   fork-tools/sync-upstream.sh            merge + build
#   fork-tools/sync-upstream.sh --build    only build (after fixing conflicts)
#   fork-tools/sync-upstream.sh --push     merge + build + push to origin

set -euo pipefail

UPSTREAM_URL="https://github.com/obsproject/obs-studio.git"
BUILD_DIR="build_macos"
CONFIG="RelWithDebInfo"

cd "$(git rev-parse --show-toplevel)"

build_only=false
push=false
for arg in "$@"; do
	case "$arg" in
	--build) build_only=true ;;
	--push) push=true ;;
	*) echo "Unknown option: $arg" >&2; exit 1 ;;
	esac
done

# git remembers how we solved a conflict once and solves it the same way next time
git config rerere.enabled true
git config rerere.autoupdate true

rebuild() {
	# Xcode does not always notice a changed .ui form, and a stale object file
	# crashes the app (we hit this with the Settings dialog). Touch every source
	# that includes a form that changed in this merge.
	local changed_forms
	changed_forms=$(git diff --name-only ORIG_HEAD HEAD -- '*.ui' 2>/dev/null || true)
	for form in $changed_forms; do
		local header
		header="ui_$(basename "$form" .ui).h"
		grep -rl --include='*.cpp' "$header" frontend 2>/dev/null | while read -r source; do
			touch "$source"
		done
	done

	git submodule update --init --recursive
	echo "==> Configuring"
	# OBS reads its version from git tags and only understands "33.0.1"-style ones,
	# so our own "v1.0.0" release tags are skipped here
	local obs_version
	obs_version=$(git describe --always --tags --match '[0-9]*')
	cmake --preset macos "-DOBS_VERSION_OVERRIDE=${obs_version}" > /dev/null
	echo "==> Building (this takes a few minutes the first time)"
	cmake --build "$BUILD_DIR" --config "$CONFIG" -- -quiet
	echo "==> Build OK: $BUILD_DIR/frontend/$CONFIG/OBS.app"
}

if $build_only; then
	rebuild
	exit 0
fi

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	echo "You have uncommitted changes. Commit or stash them first." >&2
	exit 1
fi

git remote get-url upstream > /dev/null 2>&1 || git remote add upstream "$UPSTREAM_URL"
echo "==> Fetching OBS"
git fetch upstream
git checkout master

behind=$(git rev-list --count master..upstream/master)
if [ "$behind" -eq 0 ]; then
	echo "Already up to date with OBS."
	exit 0
fi

echo "==> Merging $behind new OBS commits"
if ! git merge --no-edit upstream/master; then
	# rerere may already have solved every conflict from last time
	if [ -z "$(git diff --name-only --diff-filter=U)" ]; then
		git commit --no-edit
	else
		echo
		echo "These files need a hand-made fix:"
		git diff --name-only --diff-filter=U | sed 's/^/  /'
		echo
		echo "Fix them, then run:"
		echo "  git add -A && git commit --no-edit && fork-tools/sync-upstream.sh --build"
		exit 2
	fi
fi

rebuild

if $push; then
	git push origin master
fi

echo
echo "Done. Open the app, try Audio mode (record, edit, export), then push if you didn't use --push."
