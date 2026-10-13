#!/bin/bash
#
# Local CI/CD entry point. Use it in place of `git push`:
#   1. runs .ci/aurora-images.yml with act on this host (builds both images, then pushes them to Docker Hub);
#   2. only if that succeeds and the current branch is aurora-port, runs `git push origin aurora-port`.
#
# Usage: .ci/run.sh          build, publish, git push
#        .ci/run.sh --list   show the jobs only
#
# There is no dry run: act's -n still executes `run:` steps when jobs run on the host, so it would build and push.

set -euo pipefail

if [[ "${1:-}" == "--list" ]]; then
  exec act -l -W "$(git rev-parse --show-toplevel)/.ci/aurora-images.yml"
elif [[ $# -gt 0 ]]; then
  echo "usage: $0 [--list]" >&2
  exit 2
fi

readonly GIT_PUSH_BRANCH="aurora-port"

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

command -v act >/dev/null || { echo "error: act is not installed" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: cannot reach the Docker daemon" >&2; exit 1; }

BRANCH="$(git symbolic-ref --quiet --short HEAD)" || { echo "error: detached HEAD, check out a branch" >&2; exit 1; }

# act builds the working tree but tags the images with HEAD's SHA, so both must match.
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "error: uncommitted changes to tracked files; commit or stash them first" >&2
  exit 1
fi

echo ">>> Building and publishing images for ${BRANCH} @ $(git rev-parse --short=10 HEAD)"
act workflow_dispatch \
  -W .ci/aurora-images.yml \
  -P self-hosted=-self-hosted \
  --env REPO_ROOT="${REPO_ROOT}"

if [[ "${BRANCH}" != "${GIT_PUSH_BRANCH}" ]]; then
  echo ">>> Images published. Branch ${BRANCH} is not ${GIT_PUSH_BRANCH}: skipping git push"
  exit 0
fi

echo ">>> Images published. Pushing ${BRANCH} to every push URL of origin"
git push origin "${BRANCH}"
