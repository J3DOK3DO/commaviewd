# commaviewd: notes for agents

## Branches

- Never commit to `main` or push it. Start every change on its own branch from an up-to-date
  `main` (`git fetch origin main && git switch -c <topic> origin/main`), one topic per
  branch, and land it through a pull request.
- Don't push tags: the release workflow makes them.

## Check locally

GitHub Actions minutes are limited, and each commaviewd check is a cross build against an upstream
tree. Test on this machine while you work.

- `scripts/ci-local.sh` runs the checks for the source releases build from
  (`sunnypilot-release-pin`: a sunnypilot staging commit pinned on purpose in
  `ci/upstream-refs.env`); name targets (`--list`), or pass `--all` before a pull request is
  ready. A canary's targets: `scripts/ci-local.sh --targets ci/canary-sunnypilot.json --all`.
- The verification build needs the arm64 cross toolchain: `scripts/install-commaviewd-toolchain.sh`
  (Ubuntu 24.04; it rewrites the apt sources, so only in a container or on a build host). Web
  sessions install it at start (`.claude/hooks/session-start.sh`).
- Targets are data: `ci/targets.json` (CI: release and staging branches) and `ci/canary-*.json`
  (the canaries: openpilot nightlies, sunnypilot dev), planned by
  `ci/plan-targets.py` for both the workflows and `scripts/ci-local.sh`. All three workflows run
  `.github/workflows/commaviewd-verify.yml`.

## Pull requests and CI

- Open the pull request as a **draft** as soon as the branch has something worth showing, and
  keep it a draft while you iterate: drafts run no CI.
- Mark it **ready for review** only when the full local check passes and the branch is up to date
  with its base. Say in the description what you ran locally. That's when CI runs: it confirms,
  it isn't how you find out whether something works.
- Each push to a ready pull request runs CI again. Batch fixes into one push. If the work turns
  out to need more than a fix or two, convert it back to a draft (`gh pr ready --undo`) until it's
  ready again.
- When CI fails, reproduce the failure locally, fix it, run the local check again, then push.
  Don't re-run jobs to see whether they pass, don't push empty commits, and don't close and
  reopen a pull request to restart CI. If a failure doesn't reproduce locally, say so in the pull
  request rather than retrying.
- Don't run workflows by hand ("Run workflow" / `gh workflow run`) to test work in progress.
  Release, deploy and other manual workflows are for when a person asks for them.
- Merging is for the people reviewing, unless you're asked to merge.

## Changing CI

- Run [actionlint](https://github.com/rhysd/actionlint) on a workflow you change, and the local
  check, before pushing.
- CI skips a check whose inputs already passed (`.github/scripts/pass-marker.sh`: a merged pull
  request isn't checked twice). If a check starts to depend on something outside the paths its
  key lists, add the path to the key; to invalidate every marker, bump the key's `-v1`.
