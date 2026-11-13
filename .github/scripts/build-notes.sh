#!/bin/bash
# Prints this build's patch notes: .github/build-notes.md when this push changed it since the
# previous build of the branch, otherwise the branch's own commits since that build.
# Usage: build-notes.sh <branch> <this build's tag>
set -euo pipefail
branch=$1 tag=$2
notes=.github/build-notes.md

# The newest earlier build. A round pushed over the branch is not its descendant, so the commits
# listed start from where the two meet.
prev=$(git tag --list "${branch}-build-*" --sort=-v:refname | grep -vxF "$tag" | head -1 || true)
base=""
[ -n "$prev" ] && base=$(git merge-base "$prev" HEAD || true)

if [ -f "$notes" ] && { [ -z "$prev" ] || ! git diff --quiet "$prev" HEAD -- "$notes"; }; then
  cat "$notes"
  exit 0
fi

if [ -z "$base" ]; then
  echo "First build of \`${branch}\`."
  exit 0
fi

# --first-parent keeps an upstream merge as one line instead of every upstream commit.
count=$(git rev-list --count --first-parent "${base}..HEAD")
if [ "$count" -eq 0 ]; then
  echo "Rebuild of ${prev}."
  exit 0
fi
echo "**Changes since ${prev}**"
git log --first-parent --format='- %s (%h)' -n 20 "${base}..HEAD"
if [ "$count" -gt 20 ]; then echo "- and $((count - 20)) more"; fi
