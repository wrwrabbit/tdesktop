#!/bin/bash

set -e

# 1. Ensure two remotes: origin and upstream
if ! git remote | grep -q '^origin$'; then
  echo "Remote 'origin' not found."
  exit 1
fi
if ! git remote | grep -q '^upstream$'; then
  echo "Remote 'upstream' not found."
  exit 1
fi

# 2. Detect latest upstream tag (vX.Y.Z)
upstream_tag=$(git ls-remote --tags upstream | grep -o 'v[0-9]\+\.[0-9]\+\.[0-9]\+$' | sort -V | tail -n1)

# 3. Detect latest origin tag (v.X.Y.Z)
origin_tag=$(git ls-remote --tags origin | grep -o 'v\.[0-9]\+\.[0-9]\+\.[0-9]\+$' | sort -V | tail -n1)

# 4. Print both
echo "Latest upstream tag: $upstream_tag"
echo "Latest origin tag: $origin_tag"
read -p "Proceed? (y/n): " proceed
if [[ "$proceed" != "y" ]]; then
  exit 0
fi

# 5. Checkout master
git checkout master

# 6. Pull to latest
git pull

# 7. git clean
git clean -fdx

# 8. git submodule update
git submodule update --init --recursive

# 9. Generate branch name
merge_branch="merge/$(echo $upstream_tag | sed 's/^v//;s/\.//g')"

# 10. Create branch from master
git checkout -b "$merge_branch"

# 11. Merge upstream tag
git merge "$upstream_tag" || {
  echo "Merge conflicts detected!"
  read -p "Conflicts found. Resolve them and press y to continue: " resolved
  if [[ "$resolved" != "y" ]]; then
    exit 1
  fi
}

# 12. Check if conflicts remain
if git ls-files -u | grep .; then
  echo "Conflicts still present. Resolve them before proceeding."
  exit 1
fi

# 13. Generate new tag (next iteration)
latest_local_tag=$(git tag | grep '^v\.[0-9]\+\.[0-9]\+\.[0-9]\+$' | sort -V | tail -n1)
IFS='.' read -r _ major minor patch <<<"${latest_local_tag//v./}"
new_tag="v.$major.$minor.$((patch+1))"
new_version="$major.$minor.$((patch+1))"

# 14. Run set_ptg_version.py
echo "Setting PTG version to $new_version"
python3 ./set_ptg_version.py "$new_version" commit

# 15. git push with set-upstream
git push --set-upstream origin "$merge_branch"

echo "Done. Branch $merge_branch pushed with new tag $new_tag."
echo "Create PR: https://github.com/wrwrabbit/tdesktop/compare/master...wrwrabbit:tdesktop:$merge_branch?expand=1"
