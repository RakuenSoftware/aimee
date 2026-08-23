package db2

import (
	"context"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageCssMigrationEnumerate,
		db2contract.OperationCssMigrationEnumerate, cssMigrationEnumerate)
	Register(db2contract.StageCssMigrationRulesDoc,
		db2contract.OperationCssMigrationRulesDoc, cssMigrationRulesDoc)
	Register(db2contract.StageCssMigrationAssertConventions,
		db2contract.OperationCssMigrationAssertConventions,
		cssMigrationAssertConventions)
	Register(db2contract.StageCssRenderSnapshotStore,
		db2contract.OperationCssRenderSnapshotStore, cssRenderSnapshotStore)
}

// cssMigrationUnitCap is the C's ROW_CAP: the most units one enumerate will
// record. It is also the reply field's ceiling, which is why it is enforced
// here rather than discovered when the count fails to encode.
const cssMigrationUnitCap = 4096

// One statement where the C runs a scan and then an upsert per row it read.
//
// The C had a reason to split them -- it could not run nested writes while
// stepping the scan's statement -- and paid for it with a heap array, a cap,
// and a window in which the coverage written is the coverage of a scan that has
// already gone stale. None of that survives the collapse.
//
// The conflict clause looks like it drops three of the C's assignments. It does
// not: the C assigns state, oracle_equivalent and note to themselves, which is
// how it refreshes coverage without resetting a unit somebody is midway through
// converting. Not writing them at all is the same thing said once.
//
// ORDER BY is not decoration. The C's GROUP BY has no ordering, so which units
// a project of more than ROW_CAP files enumerates was whatever the plan
// happened to emit -- a different set each run, none of them wrong and none of
// them stable. Ordered by path, a re-enumerate refreshes the same units.
const cssMigrationEnumerateQuery = `INSERT INTO css_migration_units
 (project, generation, unit_path, state, total_tokens, resolved_tokens)
 SELECT $1, p.current_generation, f.path, 'pending', COUNT(*),
        SUM(CASE WHEN cs.resolved = 1 THEN 1 ELSE 0 END)
   FROM css_component_styles cs
   JOIN files f ON f.id = cs.component_file_id
   JOIN projects p ON p.id = f.project_id
  WHERE p.name = $1 AND p.lifecycle_state = 'current'
    AND f.generation = p.current_generation
  GROUP BY f.path, p.current_generation
  ORDER BY f.path
  LIMIT $2
 ON CONFLICT (project, generation, unit_path) DO UPDATE
    SET total_tokens = EXCLUDED.total_tokens,
        resolved_tokens = EXCLUDED.resolved_tokens`

// cssMigrationEnumerate records one migration unit per component file that has
// indexed styles, and answers how many.
func cssMigrationEnumerate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeCssMigrationEnumerateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	enumerated, execErr := store.Exec(ctx, cssMigrationEnumerateQuery, project,
		cssMigrationUnitCap)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeCssMigrationEnumerateReply(
		uint32(enumerated))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The three counts the C reads one statement at a time, read together.
//
// Separately they can disagree: an index pass landing between the first and the
// third produces a document claiming a rule count from before it and a naming
// convention from after. The document says it was derived from one exemplar
// scan, so it is derived from one.
//
// The join to declarations is a LEFT one and the rule counts are DISTINCT
// because of it: a rule with forty declarations is still one rule.
const cssExemplarSignalsQuery = `SELECT
   COUNT(DISTINCT c.id) AS rules,
   COUNT(*) FILTER (WHERE d.property LIKE '--%') AS token_declarations,
   COUNT(DISTINCT c.id) FILTER (
     WHERE c.selector LIKE '%\_\_%' ESCAPE '\'
        OR c.selector LIKE '%--%') AS bem_rules
 FROM css_rules c
 JOIN files f ON f.id = c.file_id
 JOIN projects p ON p.id = f.project_id
 LEFT JOIN css_declarations d ON d.rule_id = c.id
 WHERE p.name = $1 AND p.lifecycle_state = 'current'
   AND f.generation = p.current_generation`

// cssExemplarSignals is what both the rules document and the convention
// assertion derive from: how much is indexed, how much of it is tokenised, and
// whether the selectors carry BEM's delimiters.
type cssExemplarSignals struct {
	Rules             int64
	TokenDeclarations int64
	BEMRules          int64
}

func readCSSExemplarSignals(ctx context.Context, store Store, project string) (
	cssExemplarSignals, error,
) {
	var signals cssExemplarSignals
	err := store.QueryRow(ctx, cssExemplarSignalsQuery, project).Scan(
		&signals.Rules, &signals.TokenDeclarations, &signals.BEMRules)
	return signals, err
}

// cssRulesDocTemplate is the C's snprintf format, verbatim -- the em dash in
// the heading included, which is the one non-ASCII byte in this port and is
// here because the document is the C's, not because it was written again. A
// caller confirming these conventions is reading a document it has read before,
// so it is the same document.
const cssRulesDocTemplate = "# CSS Migration — Convention Rules (derived from exemplar `%s`)\n\n" +
	"> Degraded #2 convention model: a written rules document derived from the\n" +
	"> exemplar's indexed style graph. A human (or delegate) CONFIRMS these\n" +
	"> before they gate any conversion. No typed-fact dependency.\n\n" +
	"## Derived signals\n" +
	"- Indexed rules in exemplar: **%d**\n" +
	"- Custom-property (design token) declarations: **%d**\n" +
	"- Detected naming convention: **%s**\n\n" +
	"## Conventions each conversion must satisfy (confirm/edit)\n" +
	"1. **File layout:** a component owns its styles (co-located stylesheet or\n" +
	"   module), mirroring the exemplar's structure.\n" +
	"2. **Naming:** follow the detected scheme above for new class names.\n" +
	"3. **Tokens:** reuse the exemplar's custom properties; do not hard-code\n" +
	"   values that a token already covers.\n" +
	"4. **No Tailwind utilities in output:** utilities are expanded to plain,\n" +
	"   structured CSS modelled on the exemplar.\n" +
	"5. **Equivalence:** the WP-E static oracle must report the converted\n" +
	"   unit's resolved declaration set as equivalent (or the diff is\n" +
	"   explained and approved).\n\n" +
	"_Verify these against the exemplar before fan-out; pilot one component\n" +
	"end-to-end first._\n"

// cssMigrationRulesDoc writes the conventions a conversion must satisfy, with
// the exemplar's own numbers in them.
//
// This is the degraded path: it states what the scan found and asks a human to
// confirm it, where assert_conventions commits the same findings as facts. It
// is deliberately ungated -- a document nobody has confirmed yet gates nothing,
// so there is nothing for the typed-fact flag to protect.
func cssMigrationRulesDoc(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeCssMigrationRulesDocRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	signals, readErr := readCSSExemplarSignals(ctx, store, project)
	if readErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	naming := "flat / utility (no BEM delimiters detected)"
	if signals.BEMRules > 0 {
		naming = "BEM-like (block__element--modifier delimiters present)"
	}
	document := fmt.Sprintf(cssRulesDocTemplate, project, signals.Rules,
		signals.TokenDeclarations, naming)
	reply, encodeErr := db2contract.EncodeCssMigrationRulesDocReply(document)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// cssMigrationAssertConventions commits what the exemplar scan found as typed
// facts, and answers how many it committed.
//
// Two gates and an emptiness check stand in front of it. The flags are the C's
// -- the typed-fact layer off means the rules document is the spec and this
// asserts nothing -- and a project with nothing indexed asserts nothing either,
// because "flat-utility" derived from zero rules is not a finding about the
// project, it is a finding about the absence of a scan.
func cssMigrationAssertConventions(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, nowISO, err :=
		db2contract.DecodeCssMigrationAssertConventionsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if !cssStyleGraphEnabled() || !typedFactsEnabled() {
		return assertedCount(0)
	}
	signals, readErr := readCSSExemplarSignals(ctx, store, project)
	if readErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	if signals.Rules <= 0 {
		return assertedCount(0)
	}
	naming := "flat-utility"
	if signals.BEMRules > 0 {
		naming = "BEM"
	}
	strategy := "literal-values"
	if signals.TokenDeclarations > 0 {
		strategy = "css-custom-properties"
	}
	asserted := uint32(0)
	for _, fact := range []struct{ relation, object string }{
		{"naming_convention", naming},
		{"token_strategy", strategy},
	} {
		// The confidence is the C's 75 and it is heuristic on purpose: this is
		// derived from a scan, never from somebody saying so, and a fact a
		// person asserted must outrank one a delimiter search did.
		if assertTypedFact(ctx, store, project, "project", fact.relation,
			fact.object, "scalar", 75, "exemplar-scan", nowISO) == nil {
			asserted++
		}
	}
	return assertedCount(asserted)
}

func assertedCount(asserted uint32) ([]byte, bus.ModuleStatus) {
	reply, err := db2contract.EncodeCssMigrationAssertConventionsReply(asserted)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The lock the C's assert does without.
//
// Its sequence is a read of the active fact, an insert, and an update
// superseding what the read found, with nothing holding the three together but
// a transaction that cannot see a concurrent one's rows. Two asserters of the
// same subject and relation therefore both find no prior and both insert, and
// the table ends up with two active facts and no supersession between them --
// which the recall path hides rather than reports, because it takes the latest.
//
// Keyed on the pair rather than the table so asserting a fact about one project
// does not wait on another.
const typedFactLockQuery = `SELECT 1
 FROM (SELECT pg_advisory_xact_lock(
   hashtext('aimee_typed_fact:' || $1 || E'\x1f' || $2))) AS fact_lock`

// The C's read, insert and supersede, in one statement.
//
// The prior fact is taken FOR UPDATE so a concurrent asserter that already
// holds the lock cannot be superseded twice, and the insert is skipped when the
// object has not changed -- which is the C's UNCHANGED, and the reason a
// re-assert of a settled convention does not grow the table by a row a day.
//
// The prior row is retained and marked, never deleted: a superseded fact is how
// the layer answers what it used to believe.
const typedFactAssertQuery = `WITH prior AS (
   SELECT id, object FROM typed_facts
    WHERE subject = $1 AND relation = $3 AND active = 1
    ORDER BY id DESC LIMIT 1
    FOR UPDATE
 ), fresh AS (
   INSERT INTO typed_facts
    (subject, subject_kind, relation, object, object_kind, confidence,
     source, asserted_at)
   SELECT $1, $2, $3, $4, $5, $6, $7, $8
    WHERE NOT EXISTS (SELECT 1 FROM prior WHERE prior.object = $4)
   RETURNING id
 ), superseded AS (
   UPDATE typed_facts SET active = 0, superseded_by = (SELECT id FROM fresh)
    WHERE id IN (SELECT id FROM prior) AND EXISTS (SELECT 1 FROM fresh)
   RETURNING id
 )
 SELECT (SELECT COUNT(*) FROM fresh)`

// assertTypedFact records a (subject, relation, object) proposition, retiring
// whatever the layer believed before it.
//
// The C checks the relation and the kinds against a seed ontology first. That
// check is not reproduced because it cannot fail here: both call sites name a
// relation the ontology carries, with the subject kind it requires and a scalar
// object. Reproducing an ontology to reject arguments no caller can pass would
// be a table nothing consults.
//
// Confidence is clamped in the C to 0..100 and is a constant at both call
// sites, so that clamp is not reproduced either.
func assertTypedFact(ctx context.Context, store Store, subject, subjectKind,
	relation, object, objectKind string, confidence int64, source,
	nowISO string) error {
	return store.InTx(ctx, func(tx Store) error {
		var locked int64
		if err := tx.QueryRow(ctx, typedFactLockQuery, subject, relation).
			Scan(&locked); err != nil {
			return err
		}
		var inserted int64
		return tx.QueryRow(ctx, typedFactAssertQuery, subject, subjectKind,
			relation, object, objectKind, confidence, source, nowISO).
			Scan(&inserted)
	})
}

// The C deletes the prior snapshot and then inserts the new one, outside any
// transaction. Between the two the unit has no snapshot for that phase at all,
// and a failure between them leaves it that way: the retained origin artifact
// the diff path reads is gone and the one meant to replace it never arrived.
//
// One upsert has no such gap. The row is replaced or it is not.
const cssRenderSnapshotStoreQuery = `INSERT INTO css_render_snapshots
 (project, generation, unit_path, phase, snapshot, content_hash, captured_at)
 SELECT $1, p.current_generation, $2, $3, $4, $5,
        COALESCE(NULLIF($6, ''), pg_now_text())
   FROM projects p
  WHERE p.name = $1 AND p.lifecycle_state = 'current'
 ON CONFLICT (project, generation, unit_path, phase) DO UPDATE
    SET snapshot = EXCLUDED.snapshot,
        content_hash = EXCLUDED.content_hash,
        captured_at = EXCLUDED.captured_at`

// cssRenderSnapshotStore keeps a unit's computed-style snapshot for one render
// phase.
//
// The phase is checked against the two the schema means rather than trusted:
// the envelope bounds its length and cannot bound its spelling, and a snapshot
// filed under a third phase is one the before/after comparison will never find.
//
// captured_at falls back to the canonical stamp when the caller sends none.
// The C stores the empty string, which leaves the retained artifact undatable
// -- and undatable evidence cannot be ordered against the conversion it is
// evidence about.
func cssRenderSnapshotStore(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, unitPath, phase, snapshot, capturedAt, err :=
		db2contract.DecodeCssRenderSnapshotStoreRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if phase != "before" && phase != "after" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if !cssStyleGraphEnabled() {
		// Not an error: the style graph off means nothing captures snapshots,
		// so nothing is stored and the caller is told so.
		return acknowledgement(false,
			db2contract.EncodeCssRenderSnapshotStoreReply)
	}
	stored, execErr := store.Exec(ctx, cssRenderSnapshotStoreQuery, project,
		unitPath, phase, snapshot, fnv1a64Hex(snapshot), capturedAt)
	return acknowledgement(execErr == nil && stored == 1,
		db2contract.EncodeCssRenderSnapshotStoreReply)
}

// fnv1a64Hex is the C's cssr_hash: FNV-1a's mixing over the bytes, sixteen hex
// digits.
//
// The basis is the C's and it is one digit short of FNV-1a's published one,
// which is copied deliberately. It costs nothing -- any odd basis mixes as well
// for change detection, which is all this is: what it answers is whether this
// snapshot is the one already recorded, never whether somebody forged it. It
// would cost everything to fix, because every hash already stored would stop
// matching its own content, and because a C-written row and a Go-written row
// have to stay comparable, which is the whole point of storing one.
func fnv1a64Hex(value string) string {
	hash := uint64(1469598103934665603)
	for i := 0; i < len(value); i++ {
		hash ^= uint64(value[i])
		hash *= 1099511628211
	}
	return fmt.Sprintf("%016x", hash)
}
