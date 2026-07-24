# Refactor surface baselines and cleanup ledger

These gates make public-surface drift and cleanup accounting explicit while Aimee's source is moved
into module owners. They are review aids, not a promise that every frozen name must remain forever.
An intentional change updates the baseline and explains the disposition in the cleanup ledger or an
approved compatibility record.

Here, `freeze` means “write a new reviewed baseline snapshot,” not “make this surface immutable.”

## Check the committed state

Run both commands from any directory:

```sh
python3 -I -S /path/to/aimee/scripts/refactor_baselines.py
python3 -I -S /path/to/aimee/scripts/check_cleanup_ledger.py
```

The surface checker compares the repository with
`tests/baselines/refactor/index.json`. It fails on changed content, a new matching file, or a removed
matching file. The ledger checker rejects unknown fields, duplicate or unordered slice entries,
unresolved evidence paths, and `present_and_unverified` entries.

## Refresh an intentional surface change

1. Run `python3 -I -S scripts/refactor_baselines.py` before changing the surface and keep the result
   as the review reference.
2. Make the scoped implementation change.
3. Check `git status`, make sure every tracked surface change belongs to this change set, then run
   `python3 -I -S scripts/refactor_baselines.py freeze --accept-dirty`. The explicit flag confirms
   that you reviewed the dirty tracked inputs; without it, `freeze` refuses to write.
   Partial regeneration and hand-editing individual digests are unsupported.
4. Review the complete index diff. A broad or unrelated digest change is a reason to narrow the code change,
   not to accept the regenerated file mechanically.
5. Add or update the slice's cleanup-ledger entry with production additions, deletions,
   consolidations, remaining fallbacks, consumers, blast radius, review, and repository evidence.
6. Run both checkers and their unit tests before committing.

CI never runs `freeze`; it only verifies committed intent.

## Frozen surfaces

- CLI help: generated client command reference plus Runtime, Control Plane, and gateway help owners.
- Routes: the generated `/v1` route descriptor.
- Configuration: the generated configuration catalog.
- Public headers and symbols: tracked global headers plus a separate aggregate digest of their
  complete normalized, declaration-bearing contents. This avoids a heuristic C parser silently
  omitting complex declarations.
- Database schemas: DB1/DB2 schema SQL and repository migration SQL.
- Packages and install surface: image definitions, install scripts, frontend/editor manifests, and
  the normalized `src/Makefile` `install` recipe.

File membership is sorted. Text bytes are normalized from CRLF or CR to LF before SHA-256 hashing.
The index contains no timestamp, absolute path, Git revision, compiler identity, or live-service
state. Only Git-tracked files participate; unrelated untracked files do not cause drift. This keeps
the result independent of the current directory, locale, and developer working-tree debris.

Database coverage hashes schema and migration inputs and never connects to or mutates a
database. Recovery, compatibility aliases, and descriptor-v2 attestations remain separate proposal
slices.

## Ledger states

- `present_and_verified`: accounting and evidence are complete; accepted by CI.
- `present_and_unverified`: an explicit draft state; rejected by CI unless a local diagnostic run
  uses `--allow-unverified`.
- `absent_with_reason`: the slice has no applicable ledger artifact and records why. Consumers and
  evidence may be empty; the disposition and blast-radius/review accounting remain required.

Historical entries are backfilled only where committed validation evidence exists. This foundation
records Slices 5 and 8–11; it does not invent accounting for earlier slices.

## Failure guide

| Failure | Meaning | Operator action |
| --- | --- | --- |
| File appeared or disappeared | A tracked member of a covered surface changed | Confirm the ownership/package change, regenerate the complete snapshot, and review it |
| Digest or symbol list changed | A covered contract changed | Confirm compatibility and consumers, then regenerate with the causing change |
| Evidence does not exist | A ledger claim is not anchored in the repository | Correct the path or add the missing reviewed evidence |
| Entry is unverified | Cleanup accounting is still a draft | Complete independent review and change the state only when its evidence is committed |

The baseline proves that a surface changed; it does not prove that the change is correct. Runtime
tests, compatibility review, and the slice's acceptance gates remain responsible for correctness.
