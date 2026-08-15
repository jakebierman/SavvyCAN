#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./scripts/release.sh <vVERSION> [--push]

Without --push, validate the release and print the Git commands only.
With --push, push master, create an annotated tag, and push the tag to start
the GitHub release workflow.

Examples:
  ./scripts/release.sh v222
  ./scripts/release.sh v222 --push
EOF
}

tag="${1:-}"
mode="${2:-}"
if [[ -z "$tag" || ( -n "$mode" && "$mode" != "--push" ) ]]; then
    usage
    exit 2
fi
if [[ ! "$tag" =~ ^v[0-9]+([.][0-9]+){0,2}([-][0-9A-Za-z.-]+)?$ ]]; then
    echo "Release tag must look like v222, v222.1, or v222.1.0-rc1." >&2
    exit 2
fi

root="$(git rev-parse --show-toplevel)"
cd "$root"

branch="$(git branch --show-current)"
if [[ "$branch" != "master" ]]; then
    echo "Releases must be created from master; current branch is $branch." >&2
    exit 1
fi
if [[ -n "$(git status --porcelain)" ]]; then
    echo "The worktree is not clean. Commit or stash changes before releasing." >&2
    exit 1
fi
if git show-ref --verify --quiet "refs/tags/$tag"; then
    echo "Tag $tag already exists locally." >&2
    exit 1
fi

version="$(awk '/^#define[[:space:]]+VERSION[[:space:]]+/ { print $3; exit }' config.h)"
tag_major="${tag#v}"
tag_major="${tag_major%%.*}"
tag_major="${tag_major%%-*}"
if [[ -n "$version" && "$tag_major" != "$version" ]]; then
    echo "Tag $tag does not match VERSION $version in config.h." >&2
    exit 1
fi

if git show-ref --verify --quiet refs/remotes/origin/master; then
    read -r behind ahead < <(git rev-list --left-right --count origin/master...master)
    if (( behind > 0 )); then
        echo "Local master is behind origin/master. Pull and review before releasing." >&2
        exit 1
    fi
    echo "Local master is $ahead commit(s) ahead of origin/master."
fi

echo "Release candidate: $tag at $(git rev-parse --short HEAD)"
echo "GitHub Actions will build Linux, Windows, macOS x64, and macOS ARM64."
if [[ "$mode" != "--push" ]]; then
    echo
    echo "Dry run only. To publish:"
    echo "  ./scripts/release.sh $tag --push"
    exit 0
fi

read -r -p "Push master and publish $tag? Type the tag to confirm: " confirmation
if [[ "$confirmation" != "$tag" ]]; then
    echo "Release cancelled."
    exit 1
fi

git push origin master
git tag -a "$tag" -m "SavvyCAN $tag"
git push origin "$tag"

echo "Release workflow started for $tag."
echo "Track it at: https://github.com/$(git remote get-url origin | sed -E 's#(git@github.com:|https://github.com/)##; s#\.git$##')/actions"
