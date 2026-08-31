# Releasing

A release happens when a human approves one. Merging to `main` proposes a version and stops.

That is a change. Until 2026-07-31 every push to `main` tagged, built thin clients for four
platforms, published `aimee-server` and `aimee-kb`, and moved `:latest`, with no one asked. Under the
testing to main promote flow a promote merge is a push to `main`, so merging was releasing.
`v0.2.196` went out that way, mid-cycle, while **v0.2.192** stayed the last release anyone meant to
make.

## The two channels are not the same

- **`:testing` is automatic.** `publish-testing.yml` builds and publishes on every push to
  `testing`. Nobody approves it. That is what the channel is for, and nothing here changes it.
- **`main` needs approval.** `auto-release.yml` computes a version and waits.

If you want a build without a decision, use `:testing`.

## What a release run does

| Job | Waits for approval | Effect |
|-----|--------------------|--------|
| `version` | no | Infers the version. Writes it, the commit and the actor to the run summary. Nothing else. |
| `tag` | **yes** | Re-checks the version is free, then creates and pushes the tag. |
| `thin-clients` | no, but needs `tag` | Builds the CLI for Linux, macOS and Windows onto the GitHub Release. |
| `images` | no, but needs `tag` | Publishes `aimee-server` and `aimee-kb`, tagged `:<version>` and `:latest`. |
| `rollback-tag` | no | Deletes the tag when a build failed, so the version is not claimed by a release that never shipped. |

Nothing before `tag` has an effect, so rejecting leaves no tag and no artifacts. `tag` re-checks
because another release can land while a run sits in the queue.

## Approving one

1. Merge to `main`.
2. Open the run. The summary names the version, the commit and who pushed it.
3. Approve the `release` environment, or reject it.

**The gate is only real while the `release` environment has required reviewers.** An environment with
none is approved automatically by GitHub, which is the old behaviour with extra steps. Check it in
repository settings before trusting this document.

## The version is inferred, except the part that is a decision

The tree declares `AIMEE_VERSION_SERIES` in `src/headers/aimee_version.h`, and that is the only
version component written down:

```c
#define AIMEE_VERSION_SERIES "0.4"
```

The patch comes from the highest `v<series>.*` tag at release time. So `0.4.1` after `0.4.0` needs no
commit, and the number cannot drift from what was tagged. The tag search is scoped to the series, so
a `0.5.0` tag does not make the next `0.4.x` jump the line.

**To ship a patch:** merge to `main`, approve. Nothing else.

**To move to a new series:** edit that line to the next series in a pull request, merge, approve. The first
release in a new series is `X.Y.0`. Nothing infers this, because deciding that a change set is a
minor rather than a patch is the one part of versioning a machine should not guess.

`AIMEE_VERSION` is the full string every binary reports. Release and image builds inject the resolved
version over it; the value in the header is a placeholder for local builds and is never read to
decide what to publish.

## When a release half ships

`rollback-tag` deletes the tag when `thin-clients` or `images` fails, so the next attempt reuses the
number instead of skipping it. It is best-effort and never masks the original failure.

This exists because it happened. On 2026-07-22 three runs failed after tagging: `v0.2.193`,
`v0.2.194` and `v0.2.195` all exist as tags with no images, and `:latest` stayed on a two week old
build while three version numbers claimed to be newer. The KB image of that vintage predates
PostgreSQL shipping inside it, so anything deploying `:latest` got a KB that could not bootstrap.

If a tag survives a failed run, delete it by hand before the next release or that version is skipped.

## A tag is not a release

The repository can carry tags that were never released. `v0.2.196` is marked prerelease for exactly
that reason: it exists, it has artifacts, and it is not a release anyone announced. The repository
now carries `v0.3.0`; with `AIMEE_VERSION_SERIES` set to `0.4`, the next approved release is
`v0.4.0`, and it is what `api/releases/latest` will report once it ships.

Do not read the tag list to decide what shipped. Read the releases, and check whether the one you are
looking at is marked prerelease.
