# E5c resumable experiment validation

- **Date:** 2026-07-30 UTC
- **Base:** `testing` at `456444cad5a1f4b8e8595ae91e5c3923f4cef468`
- **Slice:** E5c benchmark/QA checkpoint and artifact integrity

The runner creates an immutable manifest before execution and an exclusive artifact
directory per attempt. A result is durably written before the named checkpoint advances.
Command launch failures, timeouts, and nonzero exits are `infrastructure-invalid` and
`score_eligible:false`; their raw output is retained. Resume requires the manifest,
checkpoint name, run ID, and plan digest to agree and skips only cells already completed
within that exact run.

```text
python3 -m unittest benchmarks/tests/test_code_intelligence_checkpoint_runner.py
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py --verify-sources
git diff --check
```

The regressions cover infrastructure-invalid preservation, idempotent named resume,
cross-run checkpoint-splice rejection, and refusal to overwrite an existing run.

## Frozen-diff review

- Run `oprun_g6a6b1ed93980fd5d_1785409585_8` converged with all three participants
  and requested changes. It found timeout output loss, unchecked checkpoint path
  names, and completed cells that were not cryptographically/provenance-bound back
  to their preserved result artifacts; it also suggested eager command validation
  and removal of a redundant resume load. All five findings are incorporated with
  regressions before reconvening.
- Run `oprun_g6a6b1ed93980fd5d_1785410134_9` converged with all three participants
  and found two incomplete closures: skipped results were field-bound but not bound
  to immutable result/stdout/stderr digests, and new runs still reloaded their just-
  written state through the resume path. Checkpoint entries now hash all three
  artifacts and resume verifies each byte stream; new runs directly retain the
  state returned by exclusive creation.
- Run `oprun_g6a6b1ed93980fd5d_1785410424_10` converged with all three
  participants and found that raw output bytes were digest-bound but lacked their
  own cell provenance, allowing a checkpoint edit to substitute another cell's
  output and matching digest. Stdout and stderr envelopes now carry and verify the
  same run/plan/checkpoint/cell tuple as the result; a cross-cell splice regression
  proves rejection.
- Run `oprun_g6a6b1ed93980fd5d_1785411063_11` converged with all three
  participants and found that invalid attempts lacked a digest ledger entry, text
  mode was not byte-lossless, and the embedded plan was not re-hashed on resume.
  Every attempt is now digest-bound in the checkpoint while only valid attempts
  enter `completed`; output envelopes are base64 over captured bytes; and resume
  verifies the canonical plan digest before reading evidence or executing work.
- Run `oprun_g6a6b1ed93980fd5d_1785411393_12` converged with all three
  participants and found that timeout diagnostics were appended to captured stderr,
  violating byte-lossless preservation. Captured stderr is now stored unchanged;
  the runner-authored timeout message lives separately in the result envelope's
  `infrastructure_error` field.
- Run `oprun_g6a6b1ed93980fd5d_1785411634_13` converged with all three
  participants and required exact-byte timeout assertions plus explicit rejection
  of evidence for cell IDs absent from the immutable plan. The regression now
  compares complete stdout/stderr byte strings and verifies diagnostic separation;
  resume derives the allowed cell-ID set from the digest-verified plan.
- Run `oprun_g6a6b1ed93980fd5d_1785411892_14` converged with all three
  participants and required a direct regression for the already implemented
  absent-from-plan rejection. The test injects a foreign checkpoint attempt ID
  and proves resume rejects it before work executes.
- Run `oprun_g6a6b1ed93980fd5d_1785412321_15` converged with two of three
  participants and found that exclusive manifest creation did not reserve the run
  directory itself, allowing a pre-existing symlinked artifact tree. New runs now
  exclusively create the entire run directory and artifact root, and every attempt
  resolves beneath that root before evidence writes. A symlink-tree regression
  proves no outside path is created.
- Run `oprun_g6a6b1ed93980fd5d_1785412589_16` converged with all three
  participants and found that attempt containment was checked after directory
  creation and that file fsync did not durably order directory entries before
  checkpoint replacement. Artifact/cell parents are now validated before mutation;
  directories and new evidence entries are fsynced; checkpoint temp contents are
  fsynced before atomic replace; and the checkpoint directory is fsynced afterward.
- Run `oprun_g6a6b1ed93980fd5d_1785412931_17` reviewed that closure artifact but
  failed at the panel's 630-second infrastructure deadline without a verdict. It is
  retained as failed-run evidence and replaced by an identical-diff closure run.
- Replacement run `oprun_g6a6b1ed93980fd5d_1785413622_18` also failed from
  provider deadlines before aggregation. Run `oprun_g6a6b1ed93980fd5d_1785414237_19`
  completed and found unchecked checkpoint temporaries, completed-resume artifact
  root replacement, and unchecked/undurable auto-created run parents. Checkpoint
  temporaries are now unpredictable and exclusively created in a verified directory;
  resume validates the complete plain artifact tree; and new runs require an existing,
  fully resolved non-symlink parent, so no ancestor creation is hidden from fsync.
- Run `oprun_g6a6b1ed93980fd5d_1785414625_20` converged with all three
  participants and found that top-level validation still allowed in-tree symlinked
  cell/attempt/evidence components. Resume now walks every lexical evidence path
  with `lstat`, requiring plain intermediate directories and a regular final file
  before resolving, loading, or hashing it. An in-tree evidence-symlink regression
  proves rejection.
- Run `oprun_g6a6b1ed93980fd5d_1785414928_21` approved and converged with all
  three participants, no degradation, and no findings. Frozen artifact SHA-256:
  `50dd19a6189e0cb0173910380ac9c054d4733da567603dd39db3277a18d1dfce`.
