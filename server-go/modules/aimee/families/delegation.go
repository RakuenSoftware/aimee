package families

import (
	"context"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The delegation family: the spawn tree, the agent-job queue, the replay
// reservation, the retry checkpoint and the learning store.
const (
	EventDelegation uint32 = 11781
	StageDelegation uint32 = 5

	opDelegationMessageRecord            uint32 = 1
	opDelegationSpawnRecord              uint32 = 2
	opDelegationSpawnComplete            uint32 = 3
	opDelegationSpawnPreempt             uint32 = 4
	opDelegationSpawnStatus              uint32 = 5
	opDelegationSpawnStopReason          uint32 = 6
	opDelegationSpawnIsStopped           uint32 = 7
	opDelegationSpawnIsCancelled         uint32 = 8
	opDelegationSpawnIsActive            uint32 = 9
	opDelegationSpawnCountTotal          uint32 = 10
	opDelegationSpawnFindRoot            uint32 = 11
	opDelegationSpawnCountDescendants    uint32 = 12
	opDelegationSpawnListActive          uint32 = 13
	opDelegationSpawnCancelByID          uint32 = 14
	opDelegationSpawnCancelRecursive     uint32 = 15
	opDelegationSpawnCancelStale         uint32 = 16
	opDelegateReservationGet             uint32 = 17
	opDelegateReservationAdopt           uint32 = 18
	opDelegateReservationSave            uint32 = 19
	opDelegateReservationForget          uint32 = 20
	opDelegateReservationForgetIfMatches uint32 = 21
	opDelegationCheckpointSave           uint32 = 22
	opDelegationCheckpointLoad           uint32 = 23
	opAgentJobCreate                     uint32 = 24
	opAgentJobUpdate                     uint32 = 25
	opAgentJobComplete                   uint32 = 26
	opAgentJobSetAgent                   uint32 = 27
	opAgentJobHeartbeat                  uint32 = 28
	opAgentJobHeartbeatExt               uint32 = 29
	opAgentJobIsCancelled                uint32 = 30
	opAgentJobClassifyStale              uint32 = 31
	opAgentJobGet                        uint32 = 32
	opAgentJobGetByParticipant           uint32 = 33
	opAgentJobHeartbeatIsStale           uint32 = 34
	opAgentJobTakeLease                  uint32 = 35
	opAgentJobListRecent                 uint32 = 36
	opAgentJobListRunningIDs             uint32 = 37
	opAgentJobCancelByID                 uint32 = 38
	opAgentJobCancelUnassigned           uint32 = 39
	opAgentJobCancelNonterminal          uint32 = 40
	opAgentJobCancelStale                uint32 = 41
	opAgentLogEntryList                  uint32 = 42
	opDelegateLearningRecord             uint32 = 43
	opDelegateLearningInjectPrompt       uint32 = 44
)

const (
	delegationListMax = 512
	// spawnStaleHours is how long a spawn may sit active before the sweep
	// cancels it.
	spawnStaleHours = 24
	// learningCap and learningInjectMax bound the learning store and the
	// prompt it produces.
	learningCap       = 200
	learningInjectMax = 10
	// treeDepthMax bounds both recursive walks. The C had no bound: a cycle in
	// parent_delegation_id -- which nothing prevents on write -- made the CTE
	// recurse until the process gave up. A delegation tree is never remotely
	// this deep, so the cap only ever fires on corruption.
	treeDepthMax = 64
)

// liveSpawnStates are the spawn statuses a cancel or completion may act on.
var liveSpawnStates = []string{"active", "running"}

// liveJobStates are the agent-job statuses a cancel may act on.
var liveJobStates = []string{"pending", "running"}

const (
	spawnRecordSQL = `INSERT INTO delegation_spawns
	                      (delegation_id, parent_delegation_id, session_id, depth, role, status)
	                  VALUES ($1, $2, $3, $4, $5, 'running')`

	messageRecordSQL = `INSERT INTO delegation_messages (delegation_id, direction, content)
	                    VALUES ($1, $2, $3)`

	spawnCompleteSQL = `UPDATE delegation_spawns
	                       SET status = 'done', completed_at = now(), updated_at = now()
	                     WHERE delegation_id = $1 AND status = ANY($2)`

	spawnPreemptSQL = `UPDATE delegation_spawns
	                      SET status = 'preempted', completed_at = now(), updated_at = now()
	                    WHERE delegation_id = $1 AND status = ANY($2)`

	spawnStatusSQL = `SELECT status FROM delegation_spawns
	                   WHERE delegation_id = $1 ORDER BY id DESC LIMIT 1`

	spawnCountTotalSQL = `SELECT count(*) FROM delegation_spawns WHERE session_id = $1`

	// This one genuinely needs the depth guard, unlike the walks that collect
	// only ids. It carries an incrementing depth for the ORDER BY, so its rows
	// never repeat and UNION cannot dedupe a cycle away -- the bound is the only
	// thing that stops it.
	spawnFindRootSQL = `WITH RECURSIVE ancestors(delegation_id, parent_delegation_id, depth) AS (
	        SELECT delegation_id, parent_delegation_id, 0
	          FROM delegation_spawns WHERE delegation_id = $1
	        UNION ALL
	        SELECT s.delegation_id, s.parent_delegation_id, a.depth + 1
	          FROM delegation_spawns s
	          JOIN ancestors a ON s.delegation_id = a.parent_delegation_id
	         WHERE a.parent_delegation_id <> '' AND a.depth < $2
	    )
	    SELECT delegation_id FROM ancestors
	     ORDER BY (parent_delegation_id = '') DESC, depth DESC
	     LIMIT 1`

	// UNION rather than UNION ALL: on a tree the two are identical, and on a
	// cycle only one of them stops. The depth guard bounds it as well.
	spawnCountDescendantsSQL = `WITH RECURSIVE descendants(delegation_id, depth) AS (
	        SELECT delegation_id, 0 FROM delegation_spawns WHERE parent_delegation_id = $1
	        UNION
	        SELECT s.delegation_id, d.depth + 1
	          FROM delegation_spawns s
	          JOIN descendants d ON s.parent_delegation_id = d.delegation_id
	         WHERE d.depth < $2
	    )
	    SELECT count(*) FROM descendants`

	spawnListActiveSQL = `SELECT id FROM delegation_spawns
	                       WHERE status = ANY($1) ORDER BY id LIMIT $2`

	spawnCancelByIDSQL = `UPDATE delegation_spawns
	                         SET status = 'cancelled', completed_at = now(), updated_at = now()
	                       WHERE id = $1 AND status = ANY($2)`

	spawnCancelRecursiveSQL = `WITH RECURSIVE descendants(id, delegation_id, depth) AS (
	        SELECT id, delegation_id, 0 FROM delegation_spawns WHERE id = $1
	        UNION
	        SELECT s.id, s.delegation_id, d.depth + 1
	          FROM delegation_spawns s
	          JOIN descendants d ON s.parent_delegation_id = d.delegation_id
	         WHERE d.depth < $2
	    )
	    UPDATE delegation_spawns
	       SET status = 'cancelled', completed_at = now(), updated_at = now()
	     WHERE id IN (SELECT id FROM descendants) AND status = ANY($3)`

	spawnCancelStaleSQL = `UPDATE delegation_spawns
	                          SET status = 'cancelled', completed_at = now(), updated_at = now()
	                        WHERE status = ANY($1)
	                          AND created_at < now() - make_interval(hours => $2)`
)

// spawnFlag answers a yes/no question about a spawn's status as a 0/1 cell.
func spawnFlag(ctx context.Context, q store.Queryer, delegationID string,
	match func(string) bool) (uint32, []string, error) {
	if delegationID == "" {
		return store.StatusInvalid, nil, nil
	}
	var status string
	switch err := q.QueryRow(ctx, spawnStatusSQL, delegationID).Scan(&status); {
	case store.IsNoRows(err):
		// No spawn is not a stopped, cancelled or active spawn.
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(match(status))}, nil
}

func isTerminalSpawn(status string) bool {
	switch status {
	case "done", "cancelled", "preempted", "failed":
		return true
	}
	return false
}

func delegationMessageRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, messageRecordSQL, f[0], f[1], f[2]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func delegationSpawnRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	depth, ok := store.Atoi64(f[3])
	if f[0] == "" || !ok || depth < 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, spawnRecordSQL, f[0], f[1], f[2], depth, f[4]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func delegationSpawnComplete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, spawnCompleteSQL, f[0], liveSpawnStates); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// delegationSpawnPreempt is op 4 and reports how many rows it moved.
func delegationSpawnPreempt(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, spawnPreemptSQL, f[0], liveSpawnStates)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func delegationSpawnStatus(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var status string
	switch err := q.QueryRow(ctx, spawnStatusSQL, f[0]).Scan(&status); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{status, "0"}, nil
}

// delegationSpawnStopReason is op 6: the status, but only when it is a stop.
func delegationSpawnStopReason(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var status string
	switch err := q.QueryRow(ctx, spawnStatusSQL, f[0]).Scan(&status); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if !isTerminalSpawn(status) {
		// Still running: there is no stop to give a reason for.
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{status, "0"}, nil
}

func delegationSpawnCountTotal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return scalar(ctx, q, spawnCountTotalSQL, f[0])
}

// delegationSpawnFindRoot is op 11: climb to the top of the spawn tree.
func delegationSpawnFindRoot(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var root string
	switch err := q.QueryRow(ctx, spawnFindRootSQL, f[0], treeDepthMax).Scan(&root); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if root == "" {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{root, "0"}, nil
}

func delegationSpawnCountDescendants(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusOK, []string{"0"}, nil
	}
	return scalar(ctx, q, spawnCountDescendantsSQL, f[0], treeDepthMax)
}

func delegationSpawnListActive(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > delegationListMax {
		return store.StatusInvalid, nil, nil
	}
	return collectIDs(ctx, q, spawnListActiveSQL, liveSpawnStates, max)
}

func delegationSpawnCancelByID(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, spawnCancelByIDSQL, id, liveSpawnStates)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// delegationSpawnCancelRecursive is op 15: cancel a spawn and everything under
// it, in one statement.
//
// The CTE collects the subtree by walking parent_delegation_id down from the
// requested row, and the outer UPDATE flips every collected row that is still
// live. One round trip and no application-side recursion.
func delegationSpawnCancelRecursive(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, spawnCancelRecursiveSQL, id, treeDepthMax, liveSpawnStates)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func delegationSpawnCancelStale(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	tag, err := q.Exec(ctx, spawnCancelStaleSQL, liveSpawnStates, spawnStaleHours)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// collectIDs runs a query returning one bigint column and flattens it.
func collectIDs(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	rows, err := q.Query(ctx, sql, args...)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	var cells []string
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return 0, nil, err
		}
		cells = append(cells, store.I64toa(id))
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// --- the replay reservation -------------------------------------------------------

const (
	// The reservation table is created by the Go control plane's migrations. A
	// server running without that plane still launches delegates; it just
	// cannot replay them. Reporting a miss keeps that degradation quiet and
	// safe rather than failing every launch, which is why the presence check
	// survives the port -- as a catalog lookup rather than a sqlite_master one.
	reservationPresentSQL = `SELECT to_regclass('lifecycle_delegate_job') IS NOT NULL`

	// job_id is stored NULL for an empty seat and rendered as the wire's 0.
	reservationGetSQL = `SELECT COALESCE(job_id, 0), participant_token
	                       FROM lifecycle_delegate_job
	                      WHERE execution_key = $1`

	// One UPDATE owns both the cardinality check and the key move, so there is
	// no read-then-write window in which a second seat can appear.
	reservationAdoptSQL = `UPDATE lifecycle_delegate_job
	                          SET execution_key = $1, updated_at = now()
	                        WHERE execution_key = (
	                                SELECT execution_key FROM lifecycle_delegate_job
	                                 WHERE work_item_id = $2 AND left(execution_key, $3) = $4
	                                   AND job_id IS NOT NULL
	                                 LIMIT 1)
	                          AND 1 = (
	                                SELECT count(*) FROM lifecycle_delegate_job
	                                 WHERE work_item_id = $2 AND left(execution_key, $3) = $4
	                                   AND job_id IS NOT NULL)
	                          AND NOT EXISTS (
	                                SELECT 1 FROM lifecycle_delegate_job WHERE execution_key = $1)
	                    RETURNING COALESCE(job_id, 0), participant_token`

	reservationSaveSQL = `INSERT INTO lifecycle_delegate_job
	                          (execution_key, job_id, work_item_id, participant_token)
	                      VALUES ($1, NULLIF($2::bigint, 0), $3, $4)
	                      ON CONFLICT (execution_key) DO UPDATE SET
	                          job_id = EXCLUDED.job_id,
	                          work_item_id = EXCLUDED.work_item_id,
	                          participant_token = EXCLUDED.participant_token,
	                          updated_at = now()`

	reservationForgetSQL = `DELETE FROM lifecycle_delegate_job WHERE execution_key = $1`

	// IS NOT DISTINCT FROM rather than =, because the caller may be forgetting
	// an EMPTY seat: it sends 0, the column holds NULL, and = is never true of a
	// null. Equality here would silently decline to forget exactly the seats
	// that most need clearing.
	reservationForgetIfMatchesSQL = `DELETE FROM lifecycle_delegate_job
	                                  WHERE execution_key = $1
	                                    AND job_id IS NOT DISTINCT FROM NULLIF($2::bigint, 0)`
)

func reservationPresent(ctx context.Context, q store.Queryer) (bool, error) {
	var present bool
	if err := q.QueryRow(ctx, reservationPresentSQL).Scan(&present); err != nil {
		return false, err
	}
	return present, nil
}

func delegateReservationGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	present, err := reservationPresent(ctx, q)
	if err != nil {
		return 0, nil, err
	}
	if !present {
		return store.StatusMissing, nil, nil
	}
	var jobID int64
	var token string
	switch err := q.QueryRow(ctx, reservationGetSQL, f[0]).Scan(&jobID, &token); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if jobID <= 0 {
		// A row whose job id is unusable is worse than no row: it would replay
		// a launch that can never be polled. Report a miss so the caller
		// launches and overwrites it.
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{store.I64toa(jobID), token}, nil
}

// delegateReservationAdopt is op 18: take over the one legacy seat for a work
// item, if there is exactly one.
//
// The prefix is the execution key up to and including its last ':' -- the key
// without its content hash. Adoption only happens when that prefix matches
// EXACTLY ONE usable seat, so a grouped seat can never be guessed at.
func delegateReservationAdopt(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	executionKey, workItemID := f[0], f[1]
	if executionKey == "" || workItemID == "" {
		return store.StatusInvalid, nil, nil
	}
	sep := strings.LastIndex(executionKey, ":")
	if sep <= 0 || sep == len(executionKey)-1 {
		return store.StatusInvalid, nil, nil
	}
	prefix := executionKey[:sep+1]

	present, err := reservationPresent(ctx, q)
	if err != nil {
		return 0, nil, err
	}
	if !present {
		return store.StatusMissing, nil, nil
	}

	var jobID int64
	var token string
	switch err := q.QueryRow(ctx, reservationAdoptSQL,
		executionKey, workItemID, len(prefix), prefix).Scan(&jobID, &token); {
	case store.IsNoRows(err):
		// Either there was no seat, or there was more than one. Both mean
		// "do not adopt".
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if jobID <= 0 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, []string{store.I64toa(jobID), token}, nil
}

func delegateReservationSave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, reservationSaveSQL, f[0], jobID, f[2], f[3]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func delegateReservationForget(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, reservationForgetSQL, f[0]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// delegateReservationForgetIfMatches is op 21: drop the reservation only if it
// still points at the job the caller thinks it does.
func delegateReservationForgetIfMatches(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, reservationForgetIfMatchesSQL, f[0], jobID)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// --- the retry checkpoint ----------------------------------------------------------

const (
	checkpointSaveSQL = `INSERT INTO delegation_checkpoint
	        (delegation_id, job_id, steps_completed, last_output, error, attempt)
	    VALUES ($1, $2, $3, $4, $5, $6)
	    ON CONFLICT (delegation_id) DO UPDATE SET
	        job_id = EXCLUDED.job_id,
	        steps_completed = EXCLUDED.steps_completed,
	        last_output = EXCLUDED.last_output,
	        error = EXCLUDED.error,
	        attempt = EXCLUDED.attempt`

	checkpointLoadSQL = `SELECT steps_completed, error, last_output
	                       FROM delegation_checkpoint WHERE delegation_id = $1`
)

func delegationCheckpointSave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	attempt, ok := store.Atoi64(f[5])
	if f[0] == "" || !ok || attempt < 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, checkpointSaveSQL, f[0], f[1], f[2], f[3], f[4], attempt); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func delegationCheckpointLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var steps, failure, lastOutput string
	switch err := q.QueryRow(ctx, checkpointLoadSQL, f[0]).Scan(&steps, &failure, &lastOutput); {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{steps, failure, lastOutput}, nil
}

// --- agent jobs --------------------------------------------------------------------

// agentJobColumns is the sixteen-cell row a job read answers with.
const agentJobColumns = `id, role, prompt, agent_name, participant_token, status, result,
	    cursor, lease_owner,
	    coalesce(to_char(heartbeat_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), ''),
	    current_tool, api_call_count, cost_usd, cost_known,
	    to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	    to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')`

// agentJobLightColumns is agentJobColumns with prompt and result blanked. Same
// arity, so the reply decodes identically -- the caller that passed
// include_heavy = 0 simply reads empty strings there, which is what the C did.
const agentJobLightColumns = `id, role, '' AS prompt, agent_name, participant_token, status,
	    '' AS result, cursor, lease_owner,
	    coalesce(to_char(heartbeat_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), ''),
	    current_tool, api_call_count, cost_usd, cost_known,
	    to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'),
	    to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')`

var agentJobCols = []col{
	num("id", false), text("role", false), text("prompt", false), text("agent_name", false),
	text("participant_token", false), text("status", false), text("result", false),
	text("cursor", false), text("lease_owner", false),
	{name: "heartbeat_at", kind: kNullStamp}, text("current_tool", false),
	num("api_call_count", false), real_("cost_usd", false), num("cost_known", false),
	{name: "created_at", kind: kStamp}, {name: "updated_at", kind: kStamp},
}

const (
	// The participant token is minted here, not supplied: it is a bearer
	// capability -- db1_agent_job_get_by_participant() returns the job to
	// whoever presents it -- so it has to be unguessable, and it carries a
	// unique index that a caller-supplied string would collide on.
	agentJobCreateSQL = `INSERT INTO agent_jobs (role, prompt, agent_name, participant_token, status)
	                     VALUES ($1, $2, $3, replace(gen_random_uuid()::text || gen_random_uuid()::text, '-', ''), 'pending')
	                     RETURNING id`

	agentJobUpdateSQL = `UPDATE agent_jobs
	                        SET status = $2, cursor = $3, result = $4, updated_at = now()
	                      WHERE id = $1`

	agentJobCompleteSQL = `UPDATE agent_jobs
	                          SET status = $2, cursor = $3, result = $4,
	                              cost_known = $5, cost_usd = $6, updated_at = now()
	                        WHERE id = $1`

	agentJobSetAgentSQL = `UPDATE agent_jobs SET agent_name = $2, updated_at = now() WHERE id = $1`

	agentJobHeartbeatSQL = `UPDATE agent_jobs
	                           SET heartbeat_at = now(), updated_at = now() WHERE id = $1`

	agentJobHeartbeatExtSQL = `UPDATE agent_jobs
	                              SET heartbeat_at = now(), current_tool = $2,
	                                  api_call_count = $3, updated_at = now()
	                            WHERE id = $1`

	agentJobStatusSQL = `SELECT status FROM agent_jobs WHERE id = $1`

	agentJobGetSQL = `SELECT ` + agentJobColumns + ` FROM agent_jobs WHERE id = $1`

	agentJobByParticipantSQL = `SELECT ` + agentJobColumns + `
	                              FROM agent_jobs WHERE participant_token = $1`

	agentJobListRecentSQL = `SELECT ` + agentJobColumns + `
	                           FROM agent_jobs ORDER BY created_at DESC, id DESC LIMIT $1`

	agentJobListRecentLightSQL = `SELECT ` + agentJobLightColumns + `
	                           FROM agent_jobs ORDER BY created_at DESC, id DESC LIMIT $1`

	agentJobListRunningSQL = `SELECT id FROM agent_jobs
	                           WHERE status = 'running' ORDER BY id LIMIT $1`

	// RETURNING rather than a row count. On the C's process-wide connection
	// sqlite3_changes() was connection-global, so another worker could replace
	// the count between the step and the read -- making a successful claim
	// report failure. Reading the returned row keeps the decision tied to this
	// statement, and that reasoning holds here too.
	agentJobTakeLeaseSQL = `UPDATE agent_jobs
	                           SET status = 'running', lease_owner = $2,
	                               heartbeat_at = now(), updated_at = now()
	                         WHERE id = $1 AND status = 'pending'
	                     RETURNING id`

	agentJobCancelByIDSQL = `UPDATE agent_jobs
	                            SET status = 'cancelled', cancelled_at = now(),
	                                cancel_reason = $2,
	                                result = CASE WHEN result = '' THEN $3 ELSE result END,
	                                updated_at = now()
	                          WHERE id = $1 AND status = ANY($4)`

	// The other half of the lease race, and RETURNING for the same reason.
	agentJobCancelUnassignedSQL = `UPDATE agent_jobs
	                                  SET status = 'cancelled', cancelled_at = now(),
	                                      cancel_reason = $2, updated_at = now()
	                                WHERE id = $1
	                                  AND (status = 'pending'
	                                       OR (status = 'running' AND btrim(agent_name) = ''))
	                                  AND coalesce(heartbeat_at, created_at)
	                                      <= now() - make_interval(secs => $3)
	                            RETURNING id`

	agentJobCancelNonterminalSQL = `UPDATE agent_jobs
	                                   SET status = 'cancelled', cancelled_at = now(),
	                                       cancel_reason = $1,
	                                       result = CASE WHEN result = ''
	                                                     THEN 'cancelled: ' || $1
	                                                     ELSE result END,
	                                       updated_at = now()
	                                 WHERE status = ANY($2)`

	agentJobCancelStaleSQL = `UPDATE agent_jobs
	                             SET status = 'cancelled', cancelled_at = now(),
	                                 cancel_reason = $1,
	                                 result = CASE WHEN result = '' THEN $2 ELSE result END,
	                                 updated_at = now()
	                           WHERE status = 'running'
	                             AND created_at < now() - make_interval(secs => $3)`

	// The caller supplies the heartbeat as text and an age in minutes, so the
	// comparison is done in the database against its own clock. The C built
	// this statement with snprintf because a TEXT column gave it no interval.
	heartbeatIsStaleSQL = `SELECT $1::timestamptz + make_interval(mins => $2) < now()`
)

func agentJobCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	// f[3] is the caller's lease owner. The C ignored it here and so does this:
	// a lease is taken through agent_job_take_lease, which is what writes the
	// lease_owner column.
	var id int64
	if err := q.QueryRow(ctx, agentJobCreateSQL, f[0], f[1], f[2]).Scan(&id); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func agentJobUpdate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, agentJobUpdateSQL, id, f[1], f[2], f[3]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func agentJobComplete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, okID := store.Atoi64(f[0])
	// Match the native db1-fields-v2 contract: id, status, cursor, result,
	// has_cost, cost_usd. Result is arbitrary text, never an API-call count.
	cursor, okCursor := store.Atoi64(f[2])
	costKnown, okKnown := store.Atoi64(f[4])
	cost, okCost := store.Atof(f[5])
	if !okID || id <= 0 || !okCursor || cursor < 0 || !okCost || cost < 0 || !okKnown || (costKnown != 0 && costKnown != 1) {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, agentJobCompleteSQL, id, f[1], f[2], f[3], costKnown, cost); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func agentJobSetAgent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, agentJobSetAgentSQL, id, f[1]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func agentJobHeartbeat(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, agentJobHeartbeatSQL, id); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func agentJobHeartbeatExt(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, okID := store.Atoi64(f[0])
	apiCalls, okCalls := store.Atoi64(f[2])
	if !okID || id <= 0 || !okCalls {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, agentJobHeartbeatExtSQL, id, f[1], apiCalls); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

func agentJobIsCancelled(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var status string
	switch err := q.QueryRow(ctx, agentJobStatusSQL, id).Scan(&status); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(status == "cancelled")}, nil
}

// agentJobClassifyStale is op 31: is this job's heartbeat older than the age.
// Final-response turns are model waits, not external tool calls: keep them
// long enough for a slow completion even under an aggressive idle threshold.
// A review role gets a shorter in-tool threshold than the caller's default.
const (
	finalResponseMinStaleSecs = 300
	reviewInToolStaleSecs     = 240
)

const agentJobClassifyStaleSQL = `SELECT role, current_tool,
                                         COALESCE(EXTRACT(EPOCH FROM (now() - heartbeat_at)), 0)
                                  FROM agent_jobs WHERE id = $1`

// staleThresholdSecs mirrors the C: which threshold applies depends on what the
// job is waiting for, not only on how long it has waited.
func staleThresholdSecs(role, tool string, idleSecs, inToolSecs int) int {
	switch {
	case tool == "":
		return idleSecs
	case tool == "final_response":
		return max(idleSecs, finalResponseMinStaleSecs)
	case tool == "model":
		return idleSecs
	case role == "review" && inToolSecs > reviewInToolStaleSecs:
		return reviewInToolStaleSecs
	}
	return inToolSecs
}

// agentJobClassifyStale replies with the state NAME and, as the rc cell, 1 when
// that state is stale. A job that is absent, or has never beaten, reads as
// fresh with an empty name -- as it did in C, where a NULL heartbeat produced
// an age of zero.
func agentJobClassifyStale(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	jobID, ok := store.Atoi64(f[0])
	if !ok || jobID <= 0 {
		return store.StatusOK, []string{"", "0"}, nil
	}
	idleSecs, idleOK := store.Atoi(f[1])
	inToolSecs, toolOK := store.Atoi(f[2])
	if !idleOK || !toolOK {
		return store.StatusOK, []string{"", "0"}, nil
	}

	var role, tool string
	var ageSecs float64
	switch err := q.QueryRow(ctx, agentJobClassifyStaleSQL, jobID).Scan(&role, &tool, &ageSecs); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"", "0"}, nil
	case err != nil:
		return store.StatusOK, []string{"", "0"}, nil
	}

	if ageSecs <= float64(staleThresholdSecs(role, tool, idleSecs, inToolSecs)) {
		return store.StatusOK, []string{"fresh", "0"}, nil
	}
	switch {
	case tool == "final_response":
		return store.StatusOK, []string{"final_response", "1"}, nil
	case tool == "model":
		return store.StatusOK, []string{"model", "1"}, nil
	case tool != "":
		return store.StatusOK, []string{"in_tool", "1"}, nil
	}
	return store.StatusOK, []string{"idle", "1"}, nil
}

func agentJobHeartbeatIsStale(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return heartbeatStale(ctx, q, f[0], f[1])
}

// heartbeatStale answers whether a heartbeat plus an age has already passed.
//
// An absent heartbeat is NOT stale: a job that has never beaten has not gone
// quiet, it has not started, and cancelling it here would race the worker that
// is about to take its lease.
func heartbeatStale(ctx context.Context, q store.Queryer, heartbeat, minutes string) (uint32, []string, error) {
	staleMinutes, ok := store.Atoi(minutes)
	if heartbeat == "" || !ok || staleMinutes < 0 {
		return store.StatusOK, []string{"0"}, nil
	}
	var stale bool
	if err := q.QueryRow(ctx, heartbeatIsStaleSQL, heartbeat, staleMinutes).Scan(&stale); err != nil {
		// A heartbeat that will not parse is not a staleness verdict.
		return store.StatusOK, []string{"0"}, nil
	}
	return store.StatusOK, []string{store.Btoa(stale)}, nil
}

func agentJobGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q, agentJobGetSQL, agentJobCols, id)
}

func agentJobGetByParticipant(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return readOne(ctx, q, agentJobByParticipantSQL, agentJobCols, f[0])
}

// agentJobTakeLease is op 35: claim a pending job.
func agentJobTakeLease(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	var claimed int64
	switch err := q.QueryRow(ctx, agentJobTakeLeaseSQL, id, f[1]).Scan(&claimed); {
	case store.IsNoRows(err):
		// Someone else holds it, or it is not pending.
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}
	if claimed != id {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

func agentJobListRecent(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > delegationListMax {
		return store.StatusInvalid, nil, nil
	}
	// prompt and result are the heavy columns. A caller that does not need them
	// says so, and gets empty strings rather than a reply that can overrun the
	// operation's 1 MiB cap.
	heavy, ok := store.Atoi(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	sql := agentJobListRecentLightSQL
	if heavy != 0 {
		sql = agentJobListRecentSQL
	}
	return readMany(ctx, q, sql, agentJobCols, max)
}

func agentJobListRunningIDs(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[0])
	if !ok || max <= 0 || max > delegationListMax {
		return store.StatusInvalid, nil, nil
	}
	return collectIDs(ctx, q, agentJobListRunningSQL, max)
}

func agentJobCancelByID(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusOK, []string{"0"}, nil
	}
	reason := f[1]
	if reason == "" {
		reason = "cancelled"
	}
	tag, err := q.Exec(ctx, agentJobCancelByIDSQL, id, f[1], "cancelled: "+reason, liveJobStates)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// agentJobCancelUnassigned is op 39: cancel a job nobody has picked up.
//
// 1 means this call cancelled it, 0 means it did not. The distinction matters:
// the caller uses it to decide whether the job is still someone else's to run.
func agentJobCancelUnassigned(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, okID := store.Atoi64(f[0])
	minAge, okAge := store.Atoi(f[2])
	if !okID || id <= 0 || !okAge || minAge < 0 {
		return store.StatusInvalid, nil, nil
	}
	reason := f[1]
	if reason == "" {
		reason = "unassigned delegate lease expired"
	}
	var cancelled int64
	switch err := q.QueryRow(ctx, agentJobCancelUnassignedSQL, id, reason, minAge).Scan(&cancelled); {
	case store.IsNoRows(err):
		return store.StatusOK, []string{"0"}, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(cancelled == id)}, nil
}

func agentJobCancelNonterminal(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	reason := f[0]
	if reason == "" {
		reason = "orphaned by server restart"
	}
	tag, err := q.Exec(ctx, agentJobCancelNonterminalSQL, reason, liveJobStates)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

func agentJobCancelStale(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	threshold, ok := store.Atoi(f[0])
	if !ok || threshold < 0 {
		return store.StatusOK, []string{"0"}, nil
	}
	reason := f[1]
	if reason == "" {
		reason = "orphan cleanup"
	}
	tag, err := q.Exec(ctx, agentJobCancelStaleSQL, reason, "cancelled: "+reason, threshold)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.I64toa(tag.RowsAffected())}, nil
}

// --- the agent log -------------------------------------------------------------------

var agentLogCols = []col{
	text("agent_name", false), text("role", false), num("turns", false),
	num("tool_calls", false), boolint("success", false), num("confidence", false),
	num("prompt_tokens", false), num("completion_tokens", false), num("latency_ms", false),
	{name: "created_at", kind: kStamp},
}

func agentLogEntryList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := store.Atoi(f[1])
	if !ok || max <= 0 || max > delegationListMax {
		return store.StatusInvalid, nil, nil
	}
	list := selectList(agentLogCols)
	if f[0] == "" {
		return readMany(ctx, q,
			`SELECT `+list+` FROM agent_log ORDER BY id DESC LIMIT $1`, agentLogCols, max)
	}
	return readMany(ctx, q,
		`SELECT `+list+` FROM agent_log WHERE agent_name = $1 ORDER BY id DESC LIMIT $2`,
		agentLogCols, f[0], max)
}

// --- the learning store ----------------------------------------------------------------

const (
	learningCountSQL = `SELECT count(*) FROM delegate_learnings`

	// Evict the oldest of a review status. Two passes: reviewed and rejected
	// entries go first because they have already been acted on; only if the
	// store is still over cap does a pending one -- which nobody has looked at
	// yet -- get dropped.
	learningEvictSQL = `DELETE FROM delegate_learnings WHERE id IN (
	        SELECT id FROM delegate_learnings
	         WHERE review_status = ANY($1)
	         ORDER BY created_at ASC, id ASC
	         LIMIT $2)`

	learningRecordSQL = `INSERT INTO delegate_learnings
	        (session_id, role, failure_mode, lesson, evidence_json, confidence,
	         auto_applied, review_status)
	    VALUES ($1, $2, $3, $4, $5, $6, 1, 'pending')`

	learningInjectSQL = `SELECT lesson, confidence FROM delegate_learnings
	                      WHERE role = $1 AND auto_applied = 1
	                      ORDER BY confidence DESC, created_at DESC, id DESC
	                      LIMIT $2`
)

var (
	learningActedOn = []string{"reviewed", "rejected"}
	learningPending = []string{"pending"}
)

// evictLearnings keeps the store under its cap.
func evictLearnings(ctx context.Context, q store.Queryer) error {
	var count int64
	if err := q.QueryRow(ctx, learningCountSQL).Scan(&count); err != nil {
		return err
	}
	if count <= learningCap {
		return nil
	}
	if _, err := q.Exec(ctx, learningEvictSQL, learningActedOn, count-learningCap); err != nil {
		return err
	}
	if err := q.QueryRow(ctx, learningCountSQL).Scan(&count); err != nil {
		return err
	}
	if count <= learningCap {
		return nil
	}
	_, err := q.Exec(ctx, learningEvictSQL, learningPending, count-learningCap)
	return err
}

func delegateLearningRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	confidence, ok := store.Atof(f[5])
	if !ok || confidence < 0 || confidence > 1 {
		return store.StatusInvalid, nil, nil
	}
	evidence := f[4]
	if evidence == "" {
		evidence = "{}"
	}
	if err := evictLearnings(ctx, q); err != nil {
		return 0, nil, err
	}
	if _, err := q.Exec(ctx, learningRecordSQL, f[0], f[1], f[2], f[3], evidence, confidence); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// delegateLearningInjectPrompt is op 44: the top lessons for a role.
//
// The module returns the lessons; composing them into a prompt is the caller's
// job, and was already -- what the C did here was the query.
func delegateLearningInjectPrompt(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	topN, ok := store.Atoi(f[2])
	if !ok || topN <= 0 {
		topN = 3
	}
	if topN > learningInjectMax {
		topN = learningInjectMax
	}
	rows, err := q.Query(ctx, learningInjectSQL, f[0], topN)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	var cells []string
	for rows.Next() {
		var lesson string
		var confidence float64
		if err := rows.Scan(&lesson, &confidence); err != nil {
			return 0, nil, err
		}
		cells = append(cells, lesson)
	}
	if err := rows.Err(); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// Delegation is the family, ready to be bound to kind 11781.
var Delegation = store.Family{
	Name:  "delegation",
	Event: EventDelegation,
	Stage: StageDelegation,
	Ops: map[uint32]store.Op{
		opDelegationMessageRecord:   {Name: "delegation_message_record", Args: 3, Tx: true, Run: delegationMessageRecord},
		opDelegationSpawnRecord:     {Name: "delegation_spawn_record", Args: 5, Tx: true, Run: delegationSpawnRecord},
		opDelegationSpawnComplete:   {Name: "delegation_spawn_complete", Args: 1, Tx: true, Run: delegationSpawnComplete},
		opDelegationSpawnPreempt:    {Name: "delegation_spawn_preempt", Args: 1, Tx: true, Run: delegationSpawnPreempt},
		opDelegationSpawnStatus:     {Name: "delegation_spawn_status", Cells: 2, Args: 1, Run: delegationSpawnStatus},
		opDelegationSpawnStopReason: {Name: "delegation_spawn_stop_reason", Cells: 2, Args: 1, Run: delegationSpawnStopReason},
		opDelegationSpawnIsStopped: {Name: "delegation_spawn_is_stopped", Args: 1,
			Run: func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
				return spawnFlag(ctx, q, f[0], isTerminalSpawn)
			}},
		opDelegationSpawnIsCancelled: {Name: "delegation_spawn_is_cancelled", Args: 1,
			Run: func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
				return spawnFlag(ctx, q, f[0], func(s string) bool { return s == "cancelled" })
			}},
		opDelegationSpawnIsActive: {Name: "delegation_spawn_is_active", Args: 1,
			Run: func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
				return spawnFlag(ctx, q, f[0], func(s string) bool {
					return s == "active" || s == "running"
				})
			}},
		opDelegationSpawnCountTotal:          {Name: "delegation_spawn_count_total", Args: 1, Run: delegationSpawnCountTotal},
		opDelegationSpawnFindRoot:            {Name: "delegation_spawn_find_root", Cells: 2, Args: 1, Run: delegationSpawnFindRoot},
		opDelegationSpawnCountDescendants:    {Name: "delegation_spawn_count_descendants", Args: 1, Run: delegationSpawnCountDescendants},
		opDelegationSpawnListActive:          {Name: "delegation_spawn_list_active", Cells: 1, Args: 1, Run: delegationSpawnListActive},
		opDelegationSpawnCancelByID:          {Name: "delegation_spawn_cancel_by_id", Args: 1, Tx: true, Run: delegationSpawnCancelByID},
		opDelegationSpawnCancelRecursive:     {Name: "delegation_spawn_cancel_recursive", Args: 1, Tx: true, Run: delegationSpawnCancelRecursive},
		opDelegationSpawnCancelStale:         {Name: "delegation_spawn_cancel_stale", Args: 0, Tx: true, Run: delegationSpawnCancelStale},
		opDelegateReservationGet:             {Name: "delegate_reservation_get", Cells: 2, Args: 1, Run: delegateReservationGet},
		opDelegateReservationAdopt:           {Name: "delegate_reservation_adopt", Cells: 2, Args: 2, Tx: true, Run: delegateReservationAdopt},
		opDelegateReservationSave:            {Name: "delegate_reservation_save", Args: 4, Tx: true, Run: delegateReservationSave},
		opDelegateReservationForget:          {Name: "delegate_reservation_forget", Args: 1, Tx: true, Run: delegateReservationForget},
		opDelegateReservationForgetIfMatches: {Name: "delegate_reservation_forget_if_matches", Args: 2, Tx: true, Run: delegateReservationForgetIfMatches},
		opDelegationCheckpointSave:           {Name: "delegation_checkpoint_save", Args: 6, Tx: true, Run: delegationCheckpointSave},
		opDelegationCheckpointLoad:           {Name: "delegation_checkpoint_load", Cells: 3, Args: 1, Run: delegationCheckpointLoad},
		opAgentJobCreate:                     {Name: "agent_job_create", Args: 4, Tx: true, Run: agentJobCreate},
		opAgentJobUpdate:                     {Name: "agent_job_update", Args: 4, Tx: true, Run: agentJobUpdate},
		opAgentJobComplete:                   {Name: "agent_job_complete", Args: 6, Tx: true, Run: agentJobComplete},
		opAgentJobSetAgent:                   {Name: "agent_job_set_agent", Args: 2, Tx: true, Run: agentJobSetAgent},
		opAgentJobHeartbeat:                  {Name: "agent_job_heartbeat", Args: 1, Tx: true, Run: agentJobHeartbeat},
		opAgentJobHeartbeatExt:               {Name: "agent_job_heartbeat_ext", Args: 3, Tx: true, Run: agentJobHeartbeatExt},
		opAgentJobIsCancelled:                {Name: "agent_job_is_cancelled", Args: 1, Run: agentJobIsCancelled},
		opAgentJobClassifyStale:              {Name: "agent_job_classify_stale", Cells: 2, Args: 3, Run: agentJobClassifyStale},
		opAgentJobGet:                        {Name: "agent_job_get", Cells: 16, Args: 1, Run: agentJobGet},
		opAgentJobGetByParticipant:           {Name: "agent_job_get_by_participant", Cells: 16, Args: 1, Run: agentJobGetByParticipant},
		opAgentJobHeartbeatIsStale:           {Name: "agent_job_heartbeat_is_stale", Args: 2, Run: agentJobHeartbeatIsStale},
		opAgentJobTakeLease:                  {Name: "agent_job_take_lease", Args: 2, Tx: true, Run: agentJobTakeLease},
		opAgentJobListRecent:                 {Name: "agent_job_list_recent", Cells: len(agentJobCols), Args: 2, Run: agentJobListRecent},
		opAgentJobListRunningIDs:             {Name: "agent_job_list_running_ids", Cells: 1, Args: 1, Run: agentJobListRunningIDs},
		opAgentJobCancelByID:                 {Name: "agent_job_cancel_by_id", Args: 2, Tx: true, Run: agentJobCancelByID},
		opAgentJobCancelUnassigned:           {Name: "agent_job_cancel_unassigned", Args: 3, Tx: true, Run: agentJobCancelUnassigned},
		opAgentJobCancelNonterminal:          {Name: "agent_job_cancel_nonterminal_on_restart", Args: 1, Tx: true, Run: agentJobCancelNonterminal},
		opAgentJobCancelStale:                {Name: "agent_job_cancel_stale", Args: 2, Tx: true, Run: agentJobCancelStale},
		opAgentLogEntryList:                  {Name: "agent_log_entry_list", Cells: len(agentLogCols), Args: 2, Run: agentLogEntryList},
		opDelegateLearningRecord:             {Name: "delegate_learning_record", Args: 6, Tx: true, Run: delegateLearningRecord},
		opDelegateLearningInjectPrompt:       {Name: "delegate_learning_inject_prompt", Args: 3, Run: delegateLearningInjectPrompt},
	},
}
