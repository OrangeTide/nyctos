#!/bin/sh
# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Builds docs/site/ (see docs/Makefile) and publishes it as the gh-pages
# branch. A throwaway git worktree does the publishing, so this never
# touches the branch or working tree you're currently on:
#
#   scripts/deploy-gh-pages.sh
#
# REMOTE and BRANCH may be overridden in the environment; both default to
# the usual GitHub Pages convention.

set -eu

REMOTE=${REMOTE:-origin}
BRANCH=${BRANCH:-gh-pages}

TOPDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKTREE=$(mktemp -d "${TMPDIR:-/tmp}/gh-pages.XXXXXX")

# Branch refs, unlike HEAD, are shared by every worktree of a repository.
# Publishing under $BRANCH's own name here would clobber (or leave behind) a
# same-named local branch in the main checkout, so the worktree instead
# builds the commit on a scratch branch private to this run and only the
# resulting commit -- not the branch name -- is pushed to $REMOTE/$BRANCH.
TMP_BRANCH="gh-pages-deploy-$$"

cleanup() {
	git -C "$TOPDIR" worktree remove --force "$WORKTREE" >/dev/null 2>&1 || true
	git -C "$TOPDIR" branch -D "$TMP_BRANCH" >/dev/null 2>&1 || true
	rm -rf "$WORKTREE"
}
trap cleanup EXIT

echo "==> building docs/site"
make -C "$TOPDIR/docs" site

echo "==> preparing '$BRANCH' worktree"
git -C "$TOPDIR" fetch "$REMOTE" "$BRANCH" 2>/dev/null || true
if git -C "$TOPDIR" show-ref --verify --quiet "refs/remotes/$REMOTE/$BRANCH"; then
	git -C "$TOPDIR" worktree add --detach "$WORKTREE" "$REMOTE/$BRANCH" >/dev/null
	git -C "$WORKTREE" checkout -b "$TMP_BRANCH" >/dev/null
else
	# First deploy: no gh-pages branch upstream yet, so start an orphan
	# commit with no history shared with main.
	git -C "$TOPDIR" worktree add --detach "$WORKTREE" >/dev/null
	git -C "$WORKTREE" checkout --orphan "$TMP_BRANCH" >/dev/null
	git -C "$WORKTREE" rm -rf --quiet . >/dev/null 2>&1 || true
fi

echo "==> copying site contents"
find "$WORKTREE" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -R "$TOPDIR/docs/site/." "$WORKTREE/"
# Tells GitHub Pages not to run the Jekyll build step, which would otherwise
# ignore any file or directory starting with an underscore.
touch "$WORKTREE/.nojekyll"

cd "$WORKTREE"
git add -A
if git diff --cached --quiet; then
	echo "==> site unchanged, nothing to publish"
	exit 0
fi
git commit --quiet -m "Deploy site: $(git -C "$TOPDIR" rev-parse --short HEAD)"
git push "$REMOTE" "HEAD:$BRANCH"

echo "==> published to $REMOTE/$BRANCH"
