package db2

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// The maintenance markers live in kb_meta as text, keyed by name.
const (
	sqlKBMetaExists = `SELECT (to_regclass('kb_meta') IS NOT NULL)::int`

	sqlRecordedDimension = `SELECT value FROM kb_meta WHERE key = 'schema_embedding_dim'`

	sqlReembedMarker = `SELECT value FROM kb_meta WHERE key = 'reembed_in_progress'`

	sqlReembedClear = `DELETE FROM kb_meta WHERE key = 'reembed_in_progress'`

	// Vector tables are discovered from the catalog rather than assumed, so a
	// table the schema grew without this list knowing appears here and is
	// refused rather than silently left at the old width.
	sqlDiscoverVectorTables = `SELECT DISTINCT table_name FROM information_schema.columns` +
		` WHERE table_schema = 'public' AND udt_name = 'vector' ORDER BY table_name`

	sqlInboundForeignKeys = `SELECT count(*) FROM information_schema.constraint_column_usage ccu` +
		` JOIN information_schema.table_constraints tc ON tc.constraint_name = ccu.constraint_name` +
		` WHERE tc.constraint_type = 'FOREIGN KEY' AND ccu.table_name = $1`
)

// derivedVectorTables is the set of vector tables this reset knows how to
// rebuild.
//
// Everything here is derived: it can be dropped and regenerated from canonical
// rows. A vector table outside this set may hold the only copy of something, so
// discovering one is a refusal rather than a wider drop.
var derivedVectorTables = map[string]bool{
	"kb_embeddings":             true,
	"kb_pdf_embeddings":         true,
	"memory_embeddings":         true,
	"curator_entity_vectors":    true,
	"curator_narrative_vectors": true,
	"curator_claim_vectors":     true,
	"curator_code_unit_vectors": true,
	"exemplar_vectors":          true,
	"evidence_vectors":          true,
	"code_embeddings":           true,
}

// DimensionResetOutcome is what a reset decided to do.
type DimensionResetOutcome uint8

const (
	// DimensionResetApplied covers both a completed reset and a dry run: the
	// plan was valid and nothing refused it.
	DimensionResetApplied DimensionResetOutcome = iota
	// DimensionResetNoChange means the corpus already records the target
	// width.
	DimensionResetNoChange
	// DimensionResetNeedsForce means a table carries inbound foreign keys and
	// dropping it would cascade.
	DimensionResetNeedsForce
	// DimensionResetRefused means the live schema holds a vector table this
	// reset does not know how to rebuild.
	DimensionResetRefused
)

// result maps an outcome onto the wire's closed result codes.
func (o DimensionResetOutcome) result() uint32 {
	switch o {
	case DimensionResetNeedsForce:
		return db2contract.ResultConflict
	case DimensionResetRefused:
		return db2contract.ResultDenied
	default:
		// A no-change reset is a success carrying the discovered state, not a
		// failure: the caller asked for a width the corpus already has.
		return db2contract.ResultOK
	}
}

// DimensionResetExecutor performs the transactional DDL of a reset.
//
// The drop, the recreate at the new width, the re-recorded dimension and the
// maintenance marker are one PostgreSQL transaction, and the process's
// in-memory width may only move after it commits. That sequencing belongs to
// whoever owns the schema and the running dimension, so it is injected rather
// than reimplemented here. Everything the reset can decide without it — the
// target's validity, the recorded width, which tables exist, what they hold,
// and whether dropping them is safe — is decided above.
type DimensionResetExecutor func(ctx context.Context, plan DimensionResetPlan) error

// DimensionResetPlan is the decided shape of a reset, handed to the executor.
type DimensionResetPlan struct {
	TargetDimension   uint32
	RecordedDimension uint32
	Tables            []string
	RowsCleared       uint64
	Force             bool
}

// RuntimeState describes the running embedder process.
type RuntimeState struct {
	// Dimension is the width being served.
	Dimension func(ctx context.Context) (uint32, error)
	// Refusals counts vector upserts refused for width disagreement.
	Refusals func(ctx context.Context) (db2contract.EmbeddingRefusals, error)
	// ServingID names the embedder build.
	ServingID func(ctx context.Context) (string, error)
}

func (b *pgLifecycleBackend) EmbeddingDimension(ctx context.Context) (uint32, error) {
	if b == nil || b.runtime.Dimension == nil {
		return 0, ErrNoQuerier
	}
	return b.runtime.Dimension(ctx)
}

func (b *pgLifecycleBackend) EmbeddingRefusals(ctx context.Context) (db2contract.EmbeddingRefusals, error) {
	if b == nil || b.runtime.Refusals == nil {
		return db2contract.EmbeddingRefusals{}, ErrNoQuerier
	}
	refusals, err := b.runtime.Refusals(ctx)
	if err != nil {
		return db2contract.EmbeddingRefusals{}, err
	}
	// The contract ties the two fields together: a refusal count implies a
	// width was offered, and an offered width implies something refused it.
	// Reporting one without the other would describe a state that cannot
	// happen, and the encoder refuses it anyway.
	if (refusals.RefusedCount == 0) != (refusals.LastOffered == 0) {
		return db2contract.EmbeddingRefusals{}, fmt.Errorf(
			"db2: refusal count %d and last offered width %d disagree",
			refusals.RefusedCount, refusals.LastOffered)
	}
	return refusals, nil
}

func (b *pgLifecycleBackend) EmbedderServingID(ctx context.Context) (string, error) {
	if b == nil || b.runtime.ServingID == nil {
		return "", ErrNoQuerier
	}
	return b.runtime.ServingID(ctx)
}

// ReembedStatus reads the maintenance marker.
//
// The marker is "<target_dim>:<started_epoch>". A marker that is present but
// unparsable is reported as absent rather than as an error: the kb consults
// this to decide whether to serve vector search, and a garbled marker must not
// wedge search off permanently.
func (b *pgLifecycleBackend) ReembedStatus(ctx context.Context) (bool, db2contract.ReembedStatus, error) {
	if b == nil || b.queryRow == nil {
		return false, db2contract.ReembedStatus{}, ErrNoQuerier
	}
	row := b.queryRow(ctx, sqlReembedMarker)
	if row == nil {
		return false, db2contract.ReembedStatus{}, ErrNoQuerier
	}
	var marker string
	if err := row.Scan(&marker); err != nil {
		if isNoRows(err) {
			return false, db2contract.ReembedStatus{}, nil
		}
		return false, db2contract.ReembedStatus{}, err
	}

	target, started, ok := parseReembedMarker(marker)
	if !ok {
		return false, db2contract.ReembedStatus{}, nil
	}
	return true, db2contract.ReembedStatus{TargetDimension: target, StartedEpoch: started}, nil
}

// parseReembedMarker reads "<target_dim>:<started_epoch>".
func parseReembedMarker(marker string) (uint32, uint64, bool) {
	marker = strings.TrimSpace(marker)
	if marker == "" {
		return 0, 0, false
	}
	targetText, startedText, found := strings.Cut(marker, ":")
	if !found {
		return 0, 0, false
	}
	target, err := strconv.ParseUint(strings.TrimSpace(targetText), 10, 32)
	if err != nil || target < uint64(db2contract.ReembedDimensionMin) ||
		target > uint64(db2contract.ReembedDimensionMax) {
		return 0, 0, false
	}
	started, err := strconv.ParseUint(strings.TrimSpace(startedText), 10, 64)
	if err != nil || started == 0 {
		return 0, 0, false
	}
	return uint32(target), started, true
}

func (b *pgLifecycleBackend) ReembedClear(ctx context.Context) error {
	if b == nil || b.exec == nil {
		return ErrNoQuerier
	}
	_, err := b.exec(ctx, sqlReembedClear)
	return err
}

// ReembedClearMaintenance clears a stuck marker, refusing an unsafe clear.
//
// The dangerous case is a recorded width that disagrees with the running one:
// clearing there resumes vector search against a store still mid-transition.
// The common stuck case — the re-embed finished and the marker outlived a
// crash — has matching widths and clears without force, so requiring force
// everywhere would train operators to pass it reflexively.
func (b *pgLifecycleBackend) ReembedClearMaintenance(ctx context.Context, force bool) (bool, db2contract.ReembedClearMaintenance, error) {
	if b == nil || b.queryRow == nil || b.exec == nil {
		return false, db2contract.ReembedClearMaintenance{}, ErrNoQuerier
	}

	running, _, err := b.ReembedStatus(ctx)
	if err != nil {
		return false, db2contract.ReembedClearMaintenance{}, err
	}
	recorded, err := b.recordedDimension(ctx)
	if err != nil {
		return false, db2contract.ReembedClearMaintenance{}, err
	}
	runningDimension, err := b.EmbeddingDimension(ctx)
	if err != nil {
		return false, db2contract.ReembedClearMaintenance{}, err
	}

	status := db2contract.ReembedClearMaintenance{
		WasInProgress:     boolReply(running),
		RecordedDimension: recorded,
		RunningDimension:  runningDimension,
	}
	if recorded > 0 && recorded != runningDimension && !force {
		return false, status, nil
	}
	if err := b.ReembedClear(ctx); err != nil {
		return false, db2contract.ReembedClearMaintenance{}, err
	}
	return true, status, nil
}

// recordedDimension reads the width the corpus was built at.
//
// A fresh database has no kb_meta at all, so the catalog is probed first. On
// real PostgreSQL a SELECT against a missing table errors at execution, and
// reading that as a fault would fail the operation on exactly the databases
// where the answer is simply "nothing recorded yet".
func (b *pgLifecycleBackend) recordedDimension(ctx context.Context) (uint32, error) {
	exists, ok := b.metaTableExists(ctx)
	if !ok {
		return 0, fmt.Errorf("db2: could not determine whether kb_meta exists")
	}
	if !exists {
		return 0, nil
	}

	row := b.queryRow(ctx, sqlRecordedDimension)
	if row == nil {
		return 0, ErrNoQuerier
	}
	var value string
	if err := row.Scan(&value); err != nil {
		if isNoRows(err) {
			return 0, nil
		}
		return 0, err
	}
	// A garbage or out-of-range row reads as nothing recorded, matching the C
	// implementation: an unusable value is not evidence of a width.
	parsed, err := strconv.ParseUint(strings.TrimSpace(value), 10, 32)
	if err != nil || parsed < uint64(db2contract.ReembedDimensionMin) ||
		parsed > uint64(db2contract.ReembedDimensionMax) {
		return 0, nil
	}
	return uint32(parsed), nil
}

func (b *pgLifecycleBackend) metaTableExists(ctx context.Context) (bool, bool) {
	row := b.queryRow(ctx, sqlKBMetaExists)
	if row == nil {
		return false, false
	}
	var exists int64
	if err := row.Scan(&exists); err != nil {
		return false, false
	}
	return exists == 1, true
}

// DimensionReset decides and, unless this is a dry run, performs a width
// change across every derived vector table.
func (b *pgLifecycleBackend) DimensionReset(ctx context.Context, target uint32, force, dryRun bool) (DimensionResetOutcome, db2contract.DimensionReset, error) {
	if b == nil || b.queryRow == nil || b.query == nil {
		return DimensionResetRefused, db2contract.DimensionReset{}, ErrNoQuerier
	}

	status := db2contract.DimensionReset{TargetDimension: target}
	recorded, err := b.recordedDimension(ctx)
	if err != nil {
		return DimensionResetRefused, db2contract.DimensionReset{}, err
	}
	status.RecordedDimension = recorded

	// Already at the target: nothing to drop, and dropping anyway would
	// destroy a corpus that is correct.
	if recorded == target {
		return DimensionResetNoChange, status, nil
	}

	tables, unknown, err := b.discoverVectorTables(ctx)
	if err != nil {
		return DimensionResetRefused, db2contract.DimensionReset{}, err
	}
	status.TablesDiscovered = uint32(len(tables) + len(unknown))

	// A vector table outside the derived set may hold the only copy of
	// something. Refusing is the safe answer, and naming it is what lets an
	// operator decide.
	if len(unknown) > 0 {
		return DimensionResetRefused, status, nil
	}

	for _, table := range tables {
		rows, err := b.tableRows(ctx, table)
		if err != nil {
			return DimensionResetRefused, db2contract.DimensionReset{}, err
		}
		status.RowsCleared += rows
	}

	// The foreign-key guard runs before anything is dropped, so a refusal
	// leaves the schema untouched rather than half-reset.
	for _, table := range tables {
		hasFK, err := b.hasInboundForeignKey(ctx, table)
		if err != nil {
			return DimensionResetRefused, db2contract.DimensionReset{}, err
		}
		if hasFK && !force {
			return DimensionResetNeedsForce, status, nil
		}
	}

	if dryRun {
		// A dry run reports what would go without dropping anything, so
		// TablesDropped stays zero while RowsCleared describes the cost.
		return DimensionResetApplied, status, nil
	}

	if b.resetExecutor == nil {
		return DimensionResetRefused, db2contract.DimensionReset{},
			fmt.Errorf("db2: no dimension-reset executor is installed")
	}
	plan := DimensionResetPlan{
		TargetDimension:   target,
		RecordedDimension: recorded,
		Tables:            tables,
		RowsCleared:       status.RowsCleared,
		Force:             force,
	}
	if err := b.resetExecutor(ctx, plan); err != nil {
		return DimensionResetRefused, db2contract.DimensionReset{}, err
	}
	status.TablesDropped = uint32(len(tables))
	return DimensionResetApplied, status, nil
}

// discoverVectorTables splits the live schema's vector tables into those this
// reset can rebuild and those it cannot.
func (b *pgLifecycleBackend) discoverVectorTables(ctx context.Context) ([]string, []string, error) {
	rows, err := b.query(ctx, sqlDiscoverVectorTables)
	if err != nil {
		return nil, nil, err
	}
	defer rows.Close()

	var known, unknown []string
	for rows.Next() {
		var table *string
		if err := rows.Scan(&table); err != nil {
			return nil, nil, err
		}
		if table == nil || *table == "" {
			continue
		}
		if derivedVectorTables[*table] {
			known = append(known, *table)
		} else {
			unknown = append(unknown, *table)
		}
	}
	return known, unknown, rows.Err()
}

// tableRows counts one table.
//
// The name is interpolated because a table name cannot be a bind parameter.
// That is only safe because every name reaching here came from the catalog and
// was matched against the derived set above — a name from a caller must never
// reach this function.
func (b *pgLifecycleBackend) tableRows(ctx context.Context, table string) (uint64, error) {
	if !derivedVectorTables[table] {
		return 0, fmt.Errorf("db2: refusing to count unknown vector table %q", table)
	}
	row := b.queryRow(ctx, `SELECT count(*) FROM `+table)
	if row == nil {
		return 0, ErrNoQuerier
	}
	var count int64
	if err := row.Scan(&count); err != nil {
		if isNoRows(err) {
			return 0, nil
		}
		return 0, err
	}
	if count < 0 {
		return 0, fmt.Errorf("db2: negative row count %d for %q", count, table)
	}
	return uint64(count), nil
}

func (b *pgLifecycleBackend) hasInboundForeignKey(ctx context.Context, table string) (bool, error) {
	row := b.queryRow(ctx, sqlInboundForeignKeys, table)
	if row == nil {
		return false, ErrNoQuerier
	}
	var count int64
	if err := row.Scan(&count); err != nil {
		if isNoRows(err) {
			return false, nil
		}
		return false, err
	}
	return count > 0, nil
}
