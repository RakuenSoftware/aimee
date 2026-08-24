package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Runtime state: key/value settings, project clones, the local operator,
// environment capabilities, maintenance bookkeeping, the model catalog and its
// prices, working-profile observations, and local tool availability.

// Reply widths, from the catalog.
const (
	projectCloneCells   = 6
	localOperatorCells  = 5
	envCapabilityCells  = 3
	modelCatalogCells   = 6
	workingProfileCells = 5
	toolAvailCells      = 4
)

// --- key/value state ------------------------------------------------------------

const (
	runtimeStateSetSQL = `INSERT INTO memory_runtime_state (state_key, state_value)
	                      VALUES ($1, $2)
	                      ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value`

	runtimeStateGetSQL = `SELECT state_value FROM memory_runtime_state WHERE state_key = $1`

	// Adding to a counter, as one statement.
	//
	// The C read the value, parsed it, added, and wrote it back -- three steps
	// with nothing holding the row, so two concurrent increments both read the
	// same number and one of them was lost. Doing the arithmetic in the
	// statement makes the read-modify-write atomic.
	//
	// A value that is not a number starts from zero rather than failing: this
	// is a counter, and the caller asking to add to it has said what it means.
	runtimeStateAddIntSQL = `INSERT INTO memory_runtime_state (state_key, state_value)
	                         VALUES ($1, $2::text)
	                         ON CONFLICT (state_key) DO UPDATE SET
	                             state_value = (
	                                 COALESCE(NULLIF(memory_runtime_state.state_value, '')::bigint,
	                                          0) + $2)::text
	                         RETURNING state_value::bigint`
)

func runtimeStateSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, runtimeStateSetSQL, f[0], f[1]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func runtimeStateGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var value string
	err := q.QueryRow(ctx, runtimeStateGetSQL, f[0]).Scan(&value)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{value, "0"}, nil
}

func runtimeStateAddInt(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	delta, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	var value int64
	if err := q.QueryRow(ctx, runtimeStateAddIntSQL, f[0], delta).Scan(&value); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(value)}, nil
}

// --- project clones ---------------------------------------------------------------

const projectCloneColumns = `clone_path, project_uuid, canonical_url, origin_url, upstream_url,
                             to_char(last_seen_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	// last_seen_at moves on every upsert: seeing a clone again is the whole
	// point of recording it, so this timestamp is freshness rather than a
	// creation time to preserve.
	projectCloneUpsertSQL = `INSERT INTO project_clones
	                             (clone_path, project_uuid, canonical_url, origin_url,
	                              upstream_url, last_seen_at)
	                         VALUES ($1, $2, $3, $4, $5, now())
	                         ON CONFLICT (clone_path) DO UPDATE SET
	                             project_uuid  = EXCLUDED.project_uuid,
	                             canonical_url = EXCLUDED.canonical_url,
	                             origin_url    = EXCLUDED.origin_url,
	                             upstream_url  = EXCLUDED.upstream_url,
	                             last_seen_at  = now()`

	projectCloneGetSQL = `SELECT ` + projectCloneColumns + `
	                        FROM project_clones WHERE clone_path = $1`

	projectCloneDeleteSQL = `DELETE FROM project_clones WHERE clone_path = $1`

	projectCloneListSQL = `SELECT ` + projectCloneColumns + `
	                         FROM project_clones
	                         ORDER BY last_seen_at DESC, clone_path LIMIT $1`

	projectCloneListByProjectSQL = `SELECT ` + projectCloneColumns + `
	                                  FROM project_clones WHERE project_uuid = $1
	                                  ORDER BY last_seen_at DESC, clone_path LIMIT $2`
)

func projectCloneRow(scan func(...any) error) ([]string, error) {
	var clonePath, projectUUID, canonical, origin, upstream, lastSeen string
	if err := scan(&clonePath, &projectUUID, &canonical, &origin, &upstream, &lastSeen); err != nil {
		return nil, err
	}
	return []string{clonePath, projectUUID, canonical, origin, upstream, lastSeen}, nil
}

func projectCloneUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, projectCloneUpsertSQL, f[0], f[1], f[2], f[3], f[4]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func projectCloneGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := projectCloneRow(func(dest ...any) error {
		return q.QueryRow(ctx, projectCloneGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func projectCloneDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, projectCloneDeleteSQL, f[0])
}

func projectCloneList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, projectCloneListSQL, projectCloneCells, projectCloneRow, max)
}

func projectCloneListByProject(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, projectCloneListByProjectSQL, projectCloneCells,
		projectCloneRow, f[0], max)
}

// --- the local operator -------------------------------------------------------------

const localOperatorColumns = `secret_ref, operator_uuid, active, display_hint,
                              to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	// Deactivating everything else, then storing this one.
	//
	// The C did the same two things, but a failure between them left nobody
	// active. Here they are one transaction and the constraint in the schema is
	// what makes "at most one active" a rule rather than a convention -- it is
	// DEFERRABLE precisely so the moment in between, where two rows look
	// active, is allowed to exist and is checked at commit.
	//
	// This is deliberately NOT a data-modifying CTE. The sub-statements of one
	// of those all see the same snapshot and their effects are not ordered with
	// respect to each other, so the deactivation and the activation would race
	// against the uniqueness check within a single statement.
	localOperatorDeactivateOthersSQL = `UPDATE local_operator
	                                       SET active = false
	                                     WHERE active AND secret_ref <> $1`

	localOperatorUpsertSQL = `INSERT INTO local_operator
	                              (secret_ref, operator_uuid, active, display_hint)
	                          VALUES ($1, $2, $3, $4)
	                          ON CONFLICT (secret_ref) DO UPDATE SET
	                              operator_uuid = EXCLUDED.operator_uuid,
	                              active        = EXCLUDED.active,
	                              display_hint  = EXCLUDED.display_hint
	                          RETURNING (xmax = 0)`

	localOperatorGetSQL = `SELECT ` + localOperatorColumns + `
	                         FROM local_operator WHERE secret_ref = $1`

	localOperatorGetActiveSQL = `SELECT ` + localOperatorColumns + `
	                               FROM local_operator WHERE active LIMIT 1`

	localOperatorSetActiveSQL = `UPDATE local_operator SET active = true WHERE secret_ref = $1`

	localOperatorDeleteSQL = `DELETE FROM local_operator WHERE secret_ref = $1`

	localOperatorListSQL = `SELECT ` + localOperatorColumns + `
	                          FROM local_operator
	                          ORDER BY active DESC, created_at DESC, secret_ref LIMIT $1`
)

func localOperatorRow(scan func(...any) error) ([]string, error) {
	var secretRef, operatorUUID, displayHint, createdAt string
	var active bool
	if err := scan(&secretRef, &operatorUUID, &active, &displayHint, &createdAt); err != nil {
		return nil, err
	}
	return []string{
		secretRef, operatorUUID, store.Btoa(active), displayHint, createdAt,
	}, nil
}

func localOperatorUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}
	active, ok := store.Atob(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// Making this one active means the others are not. Both writes are in the
	// operation's transaction, and the constraint is checked when it commits.
	if active {
		if _, err := q.Exec(ctx, localOperatorDeactivateOthersSQL, f[0]); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	var inserted bool
	if err := q.QueryRow(ctx, localOperatorUpsertSQL,
		f[0], f[1], active, f[3]).Scan(&inserted); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Btoa(inserted)}, nil
}

func localOperatorGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := localOperatorRow(func(dest ...any) error {
		return q.QueryRow(ctx, localOperatorGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func localOperatorGetActive(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	reply, err := localOperatorRow(func(dest ...any) error {
		return q.QueryRow(ctx, localOperatorGetActiveSQL).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func localOperatorSetActive(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, localOperatorDeactivateOthersSQL, f[0]); err != nil {
		return store.StatusFailed, nil, err
	}
	tag, err := q.Exec(ctx, localOperatorSetActiveSQL, f[0])
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func localOperatorDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, localOperatorDeleteSQL, f[0])
}

func localOperatorList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, localOperatorListSQL, localOperatorCells, localOperatorRow, max)
}

// --- environment capabilities ---------------------------------------------------------

const (
	envCapabilitySetSQL = `INSERT INTO env_capabilities (key, value, detected_at)
	                       VALUES ($1, $2, now())
	                       ON CONFLICT (key) DO UPDATE SET
	                           value = EXCLUDED.value, detected_at = now()`

	envCapabilityGetSQL = `SELECT value,
	                              to_char(detected_at AT TIME ZONE 'utc',
	                                      'YYYY-MM-DD HH24:MI:SS')
	                         FROM env_capabilities WHERE key = $1`

	envCapabilityListSQL = `SELECT key, value,
	                               to_char(detected_at AT TIME ZONE 'utc',
	                                       'YYYY-MM-DD HH24:MI:SS')
	                          FROM env_capabilities ORDER BY key LIMIT $1`
)

func envCapabilitySet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, envCapabilitySetSQL, f[0], f[1]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func envCapabilityGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var value, detectedAt string
	err := q.QueryRow(ctx, envCapabilityGetSQL, f[0]).Scan(&value, &detectedAt)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{value, detectedAt}, nil
}

func envCapabilityList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, envCapabilityListSQL, envCapabilityCells,
		func(scan func(...any) error) ([]string, error) {
			var key, value, detectedAt string
			if err := scan(&key, &value, &detectedAt); err != nil {
				return nil, err
			}
			return []string{key, value, detectedAt}, nil
		}, max)
}

// --- maintenance bookkeeping ----------------------------------------------------------

const (
	maintenanceLoadSQL = `SELECT COALESCE(to_char(last_run_at AT TIME ZONE 'utc',
	                                      'YYYY-MM-DD HH24:MI:SS'), ''),
	                             last_memory_count, last_changes, last_elapsed_ms,
	                             last_summary_json
	                        FROM maintenance_state WHERE key = $1`

	maintenanceSaveSQL = `INSERT INTO maintenance_state
	                          (key, last_run_at, last_memory_count, last_changes,
	                           last_elapsed_ms, last_summary_json)
	                      VALUES ($1, NULLIF($2, '')::timestamptz, $3, $4, $5, $6)
	                      ON CONFLICT (key) DO UPDATE SET
	                          last_run_at       = EXCLUDED.last_run_at,
	                          last_memory_count = EXCLUDED.last_memory_count,
	                          last_changes      = EXCLUDED.last_changes,
	                          last_elapsed_ms   = EXCLUDED.last_elapsed_ms,
	                          last_summary_json = EXCLUDED.last_summary_json`
)

// maintenanceStateLoad answers with a "present" flag and the state.
//
// The flag is the first cell rather than the status carrying it, because a
// caller that has never run maintenance asks this and gets zeros -- which is a
// legitimate starting state, not a miss.
func maintenanceStateLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var (
		lastRunAt, summary   string
		memoryCount, changes int64
		elapsed              float64
	)
	err := q.QueryRow(ctx, maintenanceLoadSQL, f[0]).Scan(
		&lastRunAt, &memoryCount, &changes, &elapsed, &summary)
	if store.IsNoRows(err) {
		return store.StatusOK, []string{
			store.Btoa(false), "", "0", "0", "0", "",
		}, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.Btoa(true), lastRunAt,
		store.I64toa(memoryCount), store.I64toa(changes),
		store.Ftoa(elapsed), summary,
	}, nil
}

func maintenanceStateSave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	memoryCount, ok := store.Atoi64(f[3])
	if !ok || memoryCount < 0 {
		return store.StatusInvalid, nil, nil
	}
	changes, ok := store.Atoi64(f[4])
	if !ok || changes < 0 {
		return store.StatusInvalid, nil, nil
	}
	elapsed, ok := store.Atof(f[5])
	if !ok || elapsed < 0 {
		return store.StatusInvalid, nil, nil
	}
	// f[1] is the caller's "present" flag, which the store does not need: a row
	// exists once it has been saved.
	if _, err := q.Exec(ctx, maintenanceSaveSQL,
		f[0], f[2], memoryCount, changes, elapsed, f[6]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// --- the model catalog and its prices ---------------------------------------------------

const (
	modelCatalogIsFreshSQL = `SELECT EXISTS (
	                              SELECT 1 FROM model_catalog
	                               WHERE provider = $1
	                                 AND fetched_at > now() - make_interval(secs => $2::bigint))`

	modelCatalogGetSQL = `SELECT model, display_name, context_window, max_output, caps, deprecated
	                        FROM model_catalog WHERE provider = $1 ORDER BY model`

	// Replacing a provider's catalog is a delete and a re-insert, in one
	// transaction. There is no separate insert operation -- this is the only
	// way rows get in -- so a clear on its own would lose the catalog. Scoped
	// to one provider, so replacing one does not empty the others.
	modelCatalogReplaceSQL = `DELETE FROM model_catalog WHERE provider = $1`

	// The columns the C entry point wrote. The rest keep their defaults, which
	// is what it left them at too.
	modelCatalogInsertSQL = `INSERT INTO model_catalog
	                             (provider, model, display_name, context_window,
	                              max_output, caps, deprecated, fetched_at)
	                         VALUES ($1, $2, $3, $4, $5, $6, $7, now())
	                         ON CONFLICT (provider, model) DO UPDATE SET
	                             display_name   = EXCLUDED.display_name,
	                             context_window = EXCLUDED.context_window,
	                             max_output     = EXCLUDED.max_output,
	                             caps           = EXCLUDED.caps,
	                             deprecated     = EXCLUDED.deprecated,
	                             fetched_at     = EXCLUDED.fetched_at`

	modelPriceGetSQL = `SELECT cost_in_per_mtok, cost_out_per_mtok
	                      FROM model_pricing WHERE model = $1`

	modelPriceSetSQL = `INSERT INTO model_pricing (model, cost_in_per_mtok, cost_out_per_mtok)
	                    VALUES ($1, $2, $3)
	                    ON CONFLICT (model) DO UPDATE SET
	                        cost_in_per_mtok  = EXCLUDED.cost_in_per_mtok,
	                        cost_out_per_mtok = EXCLUDED.cost_out_per_mtok,
	                        updated_at        = now()`

	modelPriceDeleteSQL = `DELETE FROM model_pricing WHERE model = $1`
)

func modelCatalogIsFresh(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	ttl, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok || ttl < 0 {
		return store.StatusInvalid, nil, nil
	}
	var fresh bool
	if err := q.QueryRow(ctx, modelCatalogIsFreshSQL, f[0], ttl).Scan(&fresh); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Btoa(fresh)}, nil
}

func modelCatalogGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, modelCatalogGetSQL, modelCatalogCells,
		func(scan func(...any) error) ([]string, error) {
			var model, displayName string
			var contextWindow, maxOutput, caps int64
			var deprecated bool
			if err := scan(&model, &displayName, &contextWindow, &maxOutput,
				&caps, &deprecated); err != nil {
				return nil, err
			}
			return []string{
				model, displayName, store.I64toa(contextWindow),
				store.I64toa(maxOutput), store.I64toa(caps),
				store.Btoa(deprecated),
			}, nil
		}, f[0])
}

// modelCatalogReplaceMembers is how many fields one model occupies on the wire:
// id, display_name, context_window, max_output, caps, deprecated.
const modelCatalogReplaceMembers = 6

// modelCatalogReplaceMax bounds the repeated block, matching the client's own
// cap of 512 models.
const modelCatalogReplaceMax = 512

// modelCatalogReplace takes a provider and a repeated block of models, and is
// the ONLY way rows enter this table. Its arity is variable, so the dispatch
// check cannot do the validating and it is done here: the block must divide
// evenly, or the caller has sent a truncated model and the rest would be read
// off by one.
func modelCatalogReplace(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) < 1 || f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	rest := len(f) - 1
	if rest%modelCatalogReplaceMembers != 0 {
		return store.StatusInvalid, nil, nil
	}
	count := rest / modelCatalogReplaceMembers
	if count > modelCatalogReplaceMax {
		return store.StatusInvalid, nil, nil
	}

	// Every model is parsed BEFORE anything is deleted. A model that will not
	// parse half way through would otherwise leave the provider's catalog
	// emptied and partly rewritten, which is worse than refusing outright.
	type model struct {
		id, displayName          string
		contextWindow, maxOutput int64
		caps                     int64
		deprecated               bool
	}
	models := make([]model, 0, count)
	for i := 0; i < count; i++ {
		at := 1 + i*modelCatalogReplaceMembers
		contextWindow, okCW := store.Atoi64(f[at+2])
		maxOutput, okMO := store.Atoi64(f[at+3])
		caps, okCaps := store.Atoi64(f[at+4])
		deprecated, okDep := store.Atoi64(f[at+5])
		if f[at] == "" || !okCW || !okMO || !okCaps || !okDep ||
			contextWindow < 0 || maxOutput < 0 {
			return store.StatusInvalid, nil, nil
		}
		models = append(models, model{
			id: f[at], displayName: f[at+1],
			contextWindow: contextWindow, maxOutput: maxOutput,
			caps: caps, deprecated: deprecated != 0,
		})
	}

	if _, err := q.Exec(ctx, modelCatalogReplaceSQL, f[0]); err != nil {
		return store.StatusFailed, nil, err
	}
	for _, m := range models {
		if _, err := q.Exec(ctx, modelCatalogInsertSQL, f[0], m.id, m.displayName,
			m.contextWindow, m.maxOutput, m.caps, m.deprecated); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	return store.StatusOK, nil, nil
}

func modelPriceGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var in, out float64
	err := q.QueryRow(ctx, modelPriceGetSQL, f[0]).Scan(&in, &out)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Ftoa(in), store.Ftoa(out)}, nil
}

func modelPriceSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	in, ok := store.Atof(f[1])
	if f[0] == "" || !ok || in < 0 {
		return store.StatusInvalid, nil, nil
	}
	out, ok := store.Atof(f[2])
	if !ok || out < 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, modelPriceSetSQL, f[0], in, out); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func modelPriceDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, modelPriceDeleteSQL, f[0])
}

// --- working profile observations ----------------------------------------------------

const (
	// One observation, and the running state it updates, in one statement each
	// inside one transaction.
	workingProfileObserveSQL = `INSERT INTO working_profile_observations_local
	                                (working_profile_key, session_id, signal, payload_json)
	                            VALUES ($1, $2, $3, $4)`

	// The score is a running average weighted by how many observations there
	// are, so one confident observation does not overturn a settled profile.
	workingProfileStateSQL = `INSERT INTO working_profile_state_local
	                              (working_profile_key, score, observation_count,
	                               last_observation_at, updated_at)
	                          VALUES ($1, $2, 1, now(), now())
	                          ON CONFLICT (working_profile_key) DO UPDATE SET
	                              score = (working_profile_state_local.score
	                                       * working_profile_state_local.observation_count
	                                       + EXCLUDED.score)
	                                      / (working_profile_state_local.observation_count + 1),
	                              observation_count =
	                                  working_profile_state_local.observation_count + 1,
	                              last_observation_at = now(),
	                              updated_at = now()
	                          RETURNING score`

	workingProfileListSQL = `SELECT s.working_profile_key,
	                                COALESCE(MAX(o.signal), ''),
	                                s.observation_count, s.score,
	                                to_char(s.updated_at AT TIME ZONE 'utc',
	                                        'YYYY-MM-DD HH24:MI:SS')
	                           FROM working_profile_state_local s
	                           LEFT JOIN working_profile_observations_local o
	                                  ON o.working_profile_key = s.working_profile_key
	                          GROUP BY s.working_profile_key
	                          ORDER BY s.score DESC, s.working_profile_key
	                          LIMIT $1`

	workingProfileGetSQL = `SELECT s.working_profile_key,
	                               COALESCE(MAX(o.signal), ''),
	                               s.observation_count, s.score,
	                               to_char(s.updated_at AT TIME ZONE 'utc',
	                                       'YYYY-MM-DD HH24:MI:SS')
	                          FROM working_profile_state_local s
	                          LEFT JOIN working_profile_observations_local o
	                                 ON o.working_profile_key = s.working_profile_key
	                         WHERE s.working_profile_key = $1
	                         GROUP BY s.working_profile_key`

	// Resetting a field drops its state AND its observations: leaving the
	// observations would have the next one recompute an average against a
	// history the caller just asked to forget.
	workingProfileResetStateSQL = `DELETE FROM working_profile_state_local
	                                WHERE working_profile_key = $1`
	workingProfileResetObsSQL = `DELETE FROM working_profile_observations_local
	                              WHERE working_profile_key = $1`
)

func workingProfileRow(scan func(...any) error) ([]string, error) {
	var field, signal, updatedAt string
	var count int64
	var score float64
	if err := scan(&field, &signal, &count, &score, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		field, signal, store.I64toa(count), store.Ftoa(score), updatedAt,
	}, nil
}

// workingProfileLocalObserve records an observation and says whether the field
// has now crossed the caller's promotion threshold.
func workingProfileLocalObserve(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	confidence, ok := store.Atof(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	threshold, ok := store.Atof(f[4])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, workingProfileObserveSQL, f[0], f[3], f[1], "{}"); err != nil {
		return store.StatusFailed, nil, err
	}
	var score float64
	if err := q.QueryRow(ctx, workingProfileStateSQL, f[0], confidence).Scan(&score); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.Btoa(score >= threshold)}, nil
}

func workingProfileLocalList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, workingProfileListSQL, workingProfileCells, workingProfileRow, max)
}

func workingProfileLocalGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := workingProfileRow(func(dest ...any) error {
		return q.QueryRow(ctx, workingProfileGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func workingProfileLocalResetField(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, workingProfileResetObsSQL, f[0]); err != nil {
		return store.StatusFailed, nil, err
	}
	if _, err := q.Exec(ctx, workingProfileResetStateSQL, f[0]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// --- local tool availability -------------------------------------------------------

const toolAvailColumns = `tool_uuid, usable, binary_path,
                          to_char(checked_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	toolAvailSetSQL = `INSERT INTO tool_local_availability
	                       (tool_uuid, usable, binary_path, checked_at)
	                   VALUES ($1, $2, $3, now())
	                   ON CONFLICT (tool_uuid) DO UPDATE SET
	                       usable      = EXCLUDED.usable,
	                       binary_path = EXCLUDED.binary_path,
	                       checked_at  = now()`

	toolAvailGetSQL = `SELECT ` + toolAvailColumns + `
	                     FROM tool_local_availability WHERE tool_uuid = $1`

	toolAvailDeleteSQL = `DELETE FROM tool_local_availability WHERE tool_uuid = $1`

	toolAvailListSQL = `SELECT ` + toolAvailColumns + `
	                      FROM tool_local_availability ORDER BY tool_uuid LIMIT $1`
)

func toolAvailRow(scan func(...any) error) ([]string, error) {
	var toolUUID, binaryPath, checkedAt string
	var usable bool
	if err := scan(&toolUUID, &usable, &binaryPath, &checkedAt); err != nil {
		return nil, err
	}
	return []string{toolUUID, store.Btoa(usable), binaryPath, checkedAt}, nil
}

func toolAvailabilitySet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	usable, ok := store.Atob(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, toolAvailSetSQL, f[0], usable, f[2]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func toolAvailabilityGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := toolAvailRow(func(dest ...any) error {
		return q.QueryRow(ctx, toolAvailGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func toolAvailabilityDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, toolAvailDeleteSQL, f[0])
}

func toolAvailabilityList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, toolAvailListSQL, toolAvailCells, toolAvailRow, max)
}
