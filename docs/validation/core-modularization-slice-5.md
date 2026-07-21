# Core modularization slice 5: background skill-curator removal

This slice removes the background skill curator after the feature-liveness audit found no independent consumer of its plans or metrics. It does not remove skill review, generic memory maintenance, the DB1 maintenance-state facility, or the KB synthesis curator.

## Compatibility

Existing `skills.curator.enabled` and `skills.curator_interval_hours` values are accepted as unknown nested configuration in non-strict mode. The next save omits both retired keys. An existing `maintenance_state` row named `skill_curator` is left inert; this slice performs no schema or data migration.

The audit-of-record is [`background-skill-curator.yaml`](../audit/dispositions/background-skill-curator.yaml). Its approval is roundtable run `oprun_g6a5f2d161a3aecef_1784623013_6`.

## Acceptance gates

- `python3 -I -S scripts/check_background_skill_curator_absence.py`
- `python3 -I scripts/tests/test_check_background_skill_curator_absence.py -v`
- `make -C src build/obj/tests/unit-test-config`
- `./src/build/obj/tests/unit-test-config`
- `make -C src lint`
- full pull-request CI on `feature/core-modularization`

The absence checker pins the deleted sources, symbols, configuration readers and writers, and build objects. It also pins the generic memory-maintenance and KB-curator boundaries that must survive the deletion.
