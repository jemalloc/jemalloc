#!/usr/bin/env bash
set -u

# $1: Upstream branch
# $2: Feature branch
for commit_hash in $(git cherry "$1" "$2" | awk '$1 == "+" { print $2 }')
do
	if git grep -E '\s+$' "$commit_hash" -- ':!*.md' ':!build-aux/install-sh'
	then
		echo 'Error: found trailing whitespace' 1>&2
		exit 1
	fi
done
