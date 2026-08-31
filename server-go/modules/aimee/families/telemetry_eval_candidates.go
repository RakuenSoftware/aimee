package families

import (
	"context"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Eval candidates and approach memory: operations 49-58.
//
// Ported from src/modules/db1/eval.c and approach_failures.c, which upstream
// added after this branch forked. src/db1_client already issues all ten calls,
// so without these handlers the operations are declared and unserved.

// Cell widths, matching the reply structs the C client reads back.
const (
	evalCandidateCells   = 16
	ablationCellCells    = 4
	approachFailureCells = 12
)

// Row bounds from the operation contract. Each list op carries its own, so a
// single shared ceiling would let two of the three return more rows than the
// client declared room for.
const (
	evalCandidateListMax  = 128
	ablationGridMax       = 512
	approachCandidatesMax = 64
)

// cappedMax reads a caller's row bound and refuses one outside (0, limit].
func cappedMax(field string, limit int) (int, bool) {
	max, ok := store.Atoi(field)
	if !ok || max <= 0 || max > limit {
		return 0, false
	}
	return max, true
}

// rowID reads a row identity, which is positive by construction.
func rowID(field string) (int64, bool) {
	id, ok := store.Atoi64(field)
	if !ok || id <= 0 {
		return 0, false
	}
	return id, true
}

const evalCandidateColumns = `id, signature, state, suite, task_name, task_json,
                              origin, origin_ref, occurrences,
                              jsonb_array_length(sessions),
                              admitted_by, admitted_path, reject_reason, passing_windows,
                              to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                              to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

func evalCandidateRow(scan func(...any) error) ([]string, error) {
	var (
		id                                            int64
		occurrences, distinctSessions, passingWindows int64
		signature, state, suite, taskName, taskJSON   string
		origin, originRef                             string
		admittedBy, admittedPath, rejectReason        string
		createdAt, updatedAt                          string
	)
	if err := scan(&id, &signature, &state, &suite, &taskName, &taskJSON,
		&origin, &originRef, &occurrences, &distinctSessions,
		&admittedBy, &admittedPath, &rejectReason, &passingWindows,
		&createdAt, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), signature, state, suite, taskName, taskJSON,
		origin, originRef, store.I64toa(occurrences), store.I64toa(distinctSessions),
		admittedBy, admittedPath, rejectReason, store.I64toa(passingWindows),
		createdAt, updatedAt,
	}, nil
}

// Observation is idempotent per session: a session already on the row is the
// same observation reported twice -- a re-run scan over the same ledger -- and
// bumps nothing, so occurrences counts distinct reporters, which is what a
// reproduction bar is asking about. A caller that sends no session id has given
// no identity to deduplicate on, so that always counts.
//
// The C did this as SELECT-then-UPDATE, which two concurrent observers can
// interleave into a lost update. One statement closes that: the conflict target
// serialises them on the row.
//
// The repeated "$7 = ” OR NOT (sessions @> ...)" is that same "counts" test,
// needed in three assignments; ON CONFLICT DO UPDATE admits no FROM clause to
// compute it once.
const evalCandidateObserveSQL = `
INSERT INTO eval_candidates
       (signature, state, suite, task_name, task_json, origin, origin_ref,
        occurrences, sessions)
VALUES ($1, 'candidate',
        CASE WHEN $2 = '' THEN 'regressions' ELSE $2 END,
        $3,
        CASE WHEN $4 = '' THEN '{}' ELSE $4 END,
        $5, $6, 1,
        CASE WHEN $7 = '' THEN '[]'::jsonb ELSE jsonb_build_array($7::text) END)
ON CONFLICT (signature) DO UPDATE SET
  occurrences = eval_candidates.occurrences
    + CASE WHEN $7 = '' OR NOT (eval_candidates.sessions @> jsonb_build_array($7::text))
           THEN 1 ELSE 0 END,
  sessions = CASE
    WHEN $7 <> ''
     AND NOT (eval_candidates.sessions @> jsonb_build_array($7::text))
     -- Past the cap the set stops growing, but the session still counted
     -- above: by then the candidate is far past any admission bar, so the
     -- only cost is a counter that keeps rising.
     AND jsonb_array_length(eval_candidates.sessions) < 8
    THEN eval_candidates.sessions || jsonb_build_array($7::text)
    ELSE eval_candidates.sessions END,
  updated_at = CASE
    WHEN $7 = '' OR NOT (eval_candidates.sessions @> jsonb_build_array($7::text))
    THEN now() ELSE eval_candidates.updated_at END`

// state is deliberately absent from that SET list: observing a rejected
// signature bumps its counter and leaves it rejected.

func evalCandidateObserve(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, evalCandidateObserveSQL,
		f[0], f[1], f[2], f[3], f[4], f[5], f[6]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

const evalCandidateGetSQL = `SELECT ` + evalCandidateColumns +
	` FROM eval_candidates WHERE signature = $1`

func evalCandidateGetBySignature(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	code, cells, err := collect(ctx, q, evalCandidateGetSQL, evalCandidateCells,
		evalCandidateRow, f[0])
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if len(cells) == 0 {
		return store.StatusMissing, nil, nil
	}
	return code, cells, nil
}

const evalCandidateListSQL = `SELECT ` + evalCandidateColumns +
	` FROM eval_candidates WHERE ($1 = '' OR state = $1)
	  ORDER BY updated_at DESC, id DESC LIMIT $2`

func evalCandidateList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := cappedMax(f[1], evalCandidateListMax)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, evalCandidateListSQL, evalCandidateCells, evalCandidateRow, f[0], max)
}

// One transition statement serves all three state changes. A row already
// rejected never moves again, and a caller may additionally require the state
// it believes it is moving from -- so two operators racing to admit the same
// candidate cannot both succeed.
const evalCandidateTransitionSQL = `
UPDATE eval_candidates SET
  state = $2,
  admitted_by   = CASE WHEN $3 = '' THEN admitted_by   ELSE $3 END,
  admitted_path = CASE WHEN $4 = '' THEN admitted_path ELSE $4 END,
  reject_reason = CASE WHEN $5 = '' THEN reject_reason ELSE $5 END,
  updated_at = now()
WHERE id = $1 AND state <> 'rejected' AND ($6 = '' OR state = $6)`

// evalCandidateTransition reports StatusInvalid when the row moved nowhere: the
// id is unknown, or the state it is in does not permit this move. Either way
// the caller asked for something the ledger will not do, which is not the same
// as the store failing.
func evalCandidateTransition(ctx context.Context, q store.Queryer,
	id int64, next, admittedBy, path, reason, requireState string) (uint32, []string, error) {
	tag, err := q.Exec(ctx, evalCandidateTransitionSQL,
		id, next, admittedBy, path, reason, requireState)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, nil, nil
}

func evalCandidateMarkAdmitted(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := rowID(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return evalCandidateTransition(ctx, q, id, "admitted", f[1], f[2], "", "candidate")
}

func evalCandidateMarkRejected(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := rowID(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	reason := f[1]
	if reason == "" {
		// A rejection with no reason recorded reads later as an unexplained
		// one; name the actor rather than leaving the column empty.
		reason = "operator"
	}
	return evalCandidateTransition(ctx, q, id, "rejected", "", "", reason, "")
}

func evalCandidateMarkArchived(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := rowID(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return evalCandidateTransition(ctx, q, id, "archived", "", "", "", "admitted")
}

const evalCandidateWindowsSQL = `
UPDATE eval_candidates SET passing_windows = $2, updated_at = now() WHERE id = $1`

func evalCandidateSetPassingWindows(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := rowID(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	windows, ok := store.Atoi(f[1])
	if !ok || windows < 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, evalCandidateWindowsSQL, id, windows); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// The ablation grid reads eval_results, not the candidate ledger: it answers
// how each task fared under each ablation, which is what a capability is worth
// measured against having removed it.
const evalAblationGridSQL = `
SELECT task_name, ablation,
       COUNT(*) FILTER (WHERE success) AS passed,
       COUNT(*)                        AS total
  FROM eval_results
 WHERE ($1 = '' OR suite = $1) AND task_name <> ''
 GROUP BY task_name, ablation
 ORDER BY task_name, ablation
 LIMIT $2`

func ablationCellRow(scan func(...any) error) ([]string, error) {
	var (
		taskName, ablation string
		passed, total      int64
	)
	if err := scan(&taskName, &ablation, &passed, &total); err != nil {
		return nil, err
	}
	return []string{taskName, ablation, store.I64toa(passed), store.I64toa(total)}, nil
}

func evalAblationGrid(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := cappedMax(f[1], ablationGridMax)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, evalAblationGridSQL, ablationCellCells, ablationCellRow, f[0], max)
}

const approachFailureColumns = `id, goal_signature, goal_text, goal_tokens,
                                approach_signature, approach_text, failure_mode,
                                source, source_ref, occurrences,
                                to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                                to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

func approachFailureRow(scan func(...any) error) ([]string, error) {
	var (
		id, occurrences                         int64
		goalSig, goalText, goalTokens           string
		approachSig, approachText, failureMode  string
		source, sourceRef, createdAt, updatedAt string
	)
	if err := scan(&id, &goalSig, &goalText, &goalTokens, &approachSig, &approachText,
		&failureMode, &source, &sourceRef, &occurrences, &createdAt, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), goalSig, goalText, goalTokens, approachSig, approachText,
		failureMode, source, sourceRef, store.I64toa(occurrences), createdAt, updatedAt,
	}, nil
}

const approachFailureRecordSQL = `
INSERT INTO approach_failures
       (goal_signature, goal_text, goal_tokens, approach_signature,
        approach_text, failure_mode, source, source_ref, occurrences)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 1)
ON CONFLICT (goal_signature, approach_signature) DO UPDATE SET
  occurrences  = approach_failures.occurrences + 1,
  failure_mode = excluded.failure_mode,
  updated_at   = now()`

func approachFailureRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	// Both halves of the identity are required: a failure with no goal or no
	// approach cannot be matched against a future attempt, so it is not
	// negative knowledge, just a row.
	if f[0] == "" || f[3] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, approachFailureRecordSQL,
		f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

const approachFailureCandidatesSQL = `
SELECT ` + approachFailureColumns + `
  FROM approach_failures
 WHERE ($1 = '' OR goal_tokens LIKE $2 ESCAPE '\')
 ORDER BY occurrences DESC, updated_at DESC, id DESC
 LIMIT $3`

func approachFailureCandidates(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := cappedMax(f[1], approachCandidatesMax)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, approachFailureCandidatesSQL, approachFailureCells,
		approachFailureRow, f[0], likeContains(f[0]), max)
}
