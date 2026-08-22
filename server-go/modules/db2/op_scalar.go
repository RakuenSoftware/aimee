package db2

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

func init() {
	Register(db2contract.StageMemoryLastRetroScan,
		db2contract.OperationMemoryLastRetroScan, memoryLastRetroScan)
	Register(db2contract.StageProjectLastScan,
		db2contract.OperationProjectLastScan, projectLastScan)
	Register(db2contract.StageProjectCurrentGeneration,
		db2contract.OperationProjectCurrentGeneration, projectCurrentGeneration)
	Register(db2contract.StageProjectFingerprint,
		db2contract.OperationProjectFingerprint, projectFingerprint)
	Register(db2contract.StageBanditDecisionPoints,
		db2contract.OperationBanditDecisionPoints, banditDecisionPoints)
	Register(db2contract.StageActiveEmbedderVersion,
		db2contract.OperationActiveEmbedderVersion, activeEmbedderVersion)
}

// An aggregate over no rows is NULL, which is the nullable-column trap by
// another route: the column MAX() reads may be NOT NULL and the aggregate still
// answers NULL when nothing matched. schema_nullability_test cannot see this --
// it checks columns -- so every MAX() here scans through a pointer.

const memoryLastRetroScanQuery = `SELECT MAX(detected_at) FROM contradiction_log
 WHERE details = 'retroactive_scan'`

// memoryLastRetroScan reports when the retroactive scan last ran.
//
// Empty means it never has, which is a real answer and the one a fresh install
// gives. It is not distinguishable from a read that could not run, which is the
// shape the boundary already records for every operation of this kind.
func memoryLastRetroScan(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeMemoryLastRetroScanRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var when *string
	if err := store.QueryRow(ctx, memoryLastRetroScanQuery).Scan(&when); err != nil &&
		!errors.Is(err, pgx.ErrNoRows) {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeMemoryLastRetroScanReply(text(when))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const projectLastScanQuery = `SELECT MAX(scanned_at) FROM projects
 WHERE lifecycle_state = 'current'`

// projectLastScan reports when any current project was last scanned.
//
// Across all of them, not per project: the newest scan in the index, which is
// what a staleness check asks about.
func projectLastScan(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeProjectLastScanRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var when *string
	if err := store.QueryRow(ctx, projectLastScanQuery).Scan(&when); err != nil &&
		!errors.Is(err, pgx.ErrNoRows) {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeProjectLastScanReply(text(when))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const projectCurrentGenerationQuery = `SELECT current_generation FROM projects
 WHERE name = $1 AND lifecycle_state = 'current'`

// projectCurrentGeneration reads the generation a project's current rows carry.
//
// Zero for a project that is not current, or not there at all. Both are absence
// and neither is an error: a caller asking which generation to read is entitled
// to be told there is none.
func projectCurrentGeneration(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectCurrentGenerationRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var generation int64
	if scanErr := store.QueryRow(ctx, projectCurrentGenerationQuery, project).
		Scan(&generation); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		generation = 0
	}
	reply, err := db2contract.EncodeProjectCurrentGenerationReply(clampToU64(generation))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The aggregate is coalesced in the statement, so a project with no files
// fingerprints as the md5 of the empty string rather than as NULL. That is the
// C statement's own COALESCE and it is kept: changing where the empty case is
// handled would change the fingerprint a fileless project reports.
const projectFingerprintQuery = `SELECT md5(coalesce(string_agg(f.path || ':' || f.hash,
 E'\n' ORDER BY f.path), ''))
 FROM files f JOIN projects p ON f.project_id=p.id WHERE p.name=$1`

// projectFingerprint reports a hash over a project's file paths and hashes.
//
// Ordered by path inside the aggregate, so the fingerprint depends on the set
// of files and not on the order the database happened to return them.
func projectFingerprint(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	project, err := db2contract.DecodeProjectFingerprintRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var fingerprint *string
	if scanErr := store.QueryRow(ctx, projectFingerprintQuery, project).
		Scan(&fingerprint); scanErr != nil && !errors.Is(scanErr, pgx.ErrNoRows) {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeProjectFingerprintReply(text(fingerprint))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

const banditDecisionPointsQuery = `SELECT decision_point
 FROM bandit_decisions
 GROUP BY decision_point
 ORDER BY MAX(decided_at) DESC
 LIMIT 64`

// banditDecisionPoints lists the decision points, most recently used first, as
// a JSON array.
//
// The array is built by encoding/json rather than by hand. The C implementation
// formats each element as "%s" with no escaping, so a decision point containing
// a quote or a backslash produces a document its own caller cannot parse. That
// is a defect rather than a behaviour, and reproducing it would mean writing
// broken JSON on purpose.
func banditDecisionPoints(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeBanditDecisionPointsRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	points, status := readStringArray(ctx, store, banditDecisionPointsQuery)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	reply, err := db2contract.EncodeBanditDecisionPointsReply(points)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// readStringArray reads one text column into a JSON array.
//
// Empty values are skipped, as the C builder does: an empty decision point is
// not one. The result is always a valid document -- "[]" when nothing matched,
// never the empty string, so a caller can parse it unconditionally.
func readStringArray(ctx context.Context, store Store, query string, args ...any) (
	string, bus.ModuleStatus,
) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return "", bus.ModuleStatusInternal
	}
	defer rows.Close()

	values := []string{}
	for rows.Next() {
		var value string
		if err := rows.Scan(&value); err != nil {
			return "", bus.ModuleStatusInternal
		}
		if value == "" {
			continue
		}
		values = append(values, value)
	}
	if rows.Err() != nil {
		return "", bus.ModuleStatusInternal
	}
	encoded, err := json.Marshal(values)
	if err != nil {
		return "", bus.ModuleStatusInternal
	}
	return string(encoded), bus.ModuleStatusOK
}

const activeEmbedderVersionQuery = `SELECT version FROM memory_active_embedder WHERE id = 1`

// activeEmbedderVersion reads the embedder version the corpus was built with.
//
// Empty when nothing has recorded one, which is a fresh install rather than a
// fault.
func activeEmbedderVersion(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeActiveEmbedderVersionRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var version string
	if scanErr := store.QueryRow(ctx, activeEmbedderVersionQuery).Scan(&version); scanErr != nil {
		if !errors.Is(scanErr, pgx.ErrNoRows) {
			return nil, bus.ModuleStatusInternal
		}
		version = ""
	}
	reply, err := db2contract.EncodeActiveEmbedderVersionReply(version)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
