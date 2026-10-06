# Releasing

The release being prepared is **1.0.0**. A release happens when a human approves one.
Merging to `main` proposes a version and stops.

The automatic 0.2.196 publication exposed why a promotion merge needs a separate release decision.
It remains marked prerelease. The later **0.3.0 was an intended public release**, published on
2026-08-04. The latest application release verified on 2026-10-04 is
[0.4.6](https://github.com/RakuenSoftware/aimee/releases/tag/v0.4.6).

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
| `images` | no, but needs `thin-clients` | Publishes the application/container artifacts, tagged `:<version>` and `:latest`. |
| `llm-images`, `embedder-images` | no, but need `thin-clients` | Promote the tested synthesis and embedding artifacts without rebuilding their selected manifests. |
| `rollback-tag` | no | Deletes the tag and partial release when a build failed, so the version is not claimed by a release that never shipped. |

Nothing before `tag` has an effect, so rejecting leaves no tag and no artifacts. `tag` re-checks
because another release can land while a run sits in the queue.

## Approving one

1. Review the `testing` → `main` promotion. Its `Main merge approval` workflow builds the proposed
   version's thin clients and application images, and validates synthesis/embedder promotion sources
   with publication disabled.
2. Approve the `main-merge-approval` environment after those jobs pass, then merge to `main`.
3. Open the release run. Its summary names the version, commit and actor. Approve the separate
   `release` environment to create the tag and publish the artifacts, or reject it.

**Both gates depend on their GitHub configuration.** The branch ruleset must require deployment to
`main-merge-approval`; both environments need required reviewers. An environment with no reviewers
approves automatically. Check repository settings before treating the gates as enforced.

The separate [native-memory 0.3.3 candidate](releases/native-memory-v0.3.3/README.md) has its own
reviewed seven-wheel payload and requires nine detached signatures before creating its draft prerelease. The application workflow does not build
or publish those plugins. Publish a compatible application first, then verify a clean plugin install
from the public release assets.

## The version is inferred, except the part that is a decision

The tree declares `AIMEE_VERSION_SERIES` in `src/headers/aimee_version.h`, and that is the only
version component written down:

```c
#define AIMEE_VERSION_SERIES "1.0"
```

The patch comes from the highest `v<series>.*` tag at release time. With series `1.0` and no
`v1.0.*` tag, the next proposal is `1.0.0`. Later patches follow the highest tag in that series;
a tag in another series cannot advance it.

**To ship a patch:** merge to `main`, approve. Nothing else.

**To move to a new series:** edit that line to the next series in a pull request, merge, approve. The first
release in a new series is `X.Y.0`. Nothing infers this, because deciding that a change set is a
minor rather than a patch is the one part of versioning a machine should not guess.

`AIMEE_VERSION` is the full string every binary reports. Release and image builds inject the resolved
version over it; the value in the header is a placeholder for local builds and is never read to
decide what to publish.

## When a release half ships

`rollback-tag` deletes the tag and partial release when `thin-clients`, `images`, `llm-images` or
`embedder-images` fails or is cancelled, so the next attempt reuses the
number instead of skipping it. It is best-effort and never masks the original failure.

This exists because it happened. On 2026-07-22 three runs failed after tagging: `v0.2.193`,
`v0.2.194` and `v0.2.195` all exist as tags with no images, and `:latest` stayed on a two week old
build while three version numbers claimed to be newer. The KB image of that vintage predates
PostgreSQL shipping inside it, so anything deploying `:latest` got a KB that could not bootstrap.

If a tag survives a failed run, delete it by hand before the next release or that version is skipped.

## A tag is not a release

A tag may exist without a successful release. The [0.2.196 entry](https://github.com/RakuenSoftware/aimee/releases/tag/v0.2.196)
is a prerelease; [0.3.0](https://github.com/RakuenSoftware/aimee/releases/tag/v0.3.0) is an intended
public release. Published 0.4 releases continue through 0.4.6 as of 2026-10-04.

Read the release entry, draft/prerelease flags and artifact status to establish what shipped.
Run `scripts/next-release-version.sh /tmp/aimee-next-version.txt` with freshly fetched tags to
inspect the next proposal. It writes an output file and creates no tag or release. In the current
1.0 series with no v1.0.* tag, it proposes 1.0.0; a later tag changes that answer.

Independent core/module repository tags and the separate native-memory plugin release do not set
the application version. See [repository extraction](core/repository-extraction.md).
