package families

import (
	"context"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Cron jobs and their run history.

// A cron job travels as eight leading scalars, eight skill slots, a skill
// count, and six trailing scalars.
const (
	cronMaxSkills = 8
	// cronCells is the catalog's width for a job: eight leading scalars, eight
	// skill slots, the skill count, and five trailing scalars.
	cronCells = 8 + cronMaxSkills + 1 + 5
)

const cronJobColumns = `id, schedule, mode, script, prompt, workdir, context_from,
                        when_context_contains, skills_csv, deliver_target,
                        deliver_only_if_changed, deliver_first_run_silent,
                        pre_wake_gate, enabled`

const (
	// An upsert rather than the C's INSERT OR REPLACE: created_at is when the
	// job was first defined, and editing a schedule is not creating a new job.
	cronJobUpsertSQL = `INSERT INTO cron_jobs
	                        (id, schedule, mode, script, prompt, workdir, context_from,
	                         when_context_contains, skills_csv, deliver_target,
	                         deliver_only_if_changed, deliver_first_run_silent,
	                         pre_wake_gate, enabled)
	                    VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)
	                    ON CONFLICT (id) DO UPDATE SET
	                        schedule                 = EXCLUDED.schedule,
	                        mode                     = EXCLUDED.mode,
	                        script                   = EXCLUDED.script,
	                        prompt                   = EXCLUDED.prompt,
	                        workdir                  = EXCLUDED.workdir,
	                        context_from             = EXCLUDED.context_from,
	                        when_context_contains    = EXCLUDED.when_context_contains,
	                        skills_csv               = EXCLUDED.skills_csv,
	                        deliver_target           = EXCLUDED.deliver_target,
	                        deliver_only_if_changed  = EXCLUDED.deliver_only_if_changed,
	                        deliver_first_run_silent = EXCLUDED.deliver_first_run_silent,
	                        pre_wake_gate            = EXCLUDED.pre_wake_gate,
	                        enabled                  = EXCLUDED.enabled`

	cronJobGetSQL = `SELECT ` + cronJobColumns + ` FROM cron_jobs WHERE id = $1`

	cronJobLoadSQL = `SELECT ` + cronJobColumns + `
	                    FROM cron_jobs
	                   WHERE (NOT $2 OR enabled)
	                   ORDER BY id LIMIT $1`

	cronJobSetEnabledSQL    = `UPDATE cron_jobs SET enabled = $2 WHERE id = $1`
	cronJobSetEnabledAllSQL = `UPDATE cron_jobs SET enabled = $1`
	cronJobDeleteSQL        = `DELETE FROM cron_jobs WHERE id = $1`

	// Recording a run also updates the job's last-run summary, in one
	// transaction. The C wrote the run row and then updated the job from a
	// second statement, so a failure between them left a job whose last_run_*
	// disagreed with its own history.
	cronJobRecordRunSQL = `INSERT INTO cron_job_runs
	                           (job_id, completed_at, status, silent, delivered,
	                            output, error, output_hash, output_ir)
	                       VALUES ($1, EXTRACT(EPOCH FROM now())::bigint, $2, $3, $4, $5, $6, $7, $8)
	                       RETURNING id`

	cronJobTouchLastRunSQL = `UPDATE cron_jobs
	                             SET last_run_at = EXTRACT(EPOCH FROM now())::bigint,
	                                 last_run_status = $2,
	                                 last_run_output_hash = $3
	                           WHERE id = $1`

	// The list and the history are rendered as JSON documents, which is what
	// the wire's single cell carries in each case.
	cronJobListJSONSQL = `SELECT COALESCE(json_agg(row_to_json(j) ORDER BY j.id), '[]')::text
	                        FROM (SELECT id, schedule, mode, enabled, next_run_at,
	                                     last_run_at, last_run_status
	                                FROM cron_jobs) j`

	cronJobHistoryJSONSQL = `SELECT COALESCE(json_agg(row_to_json(r) ORDER BY r.id DESC),
	                                         '[]')::text
	                           FROM (SELECT id, started_at, completed_at, status, silent,
	                                        delivered, error, output_hash
	                                   FROM cron_job_runs
	                                  WHERE job_id = $1
	                                  ORDER BY id DESC
	                                  LIMIT $2) r`

	cronJobLatestOutputSQL = `SELECT output, COALESCE(output_ir, '') FROM cron_job_runs
	                           WHERE job_id = $1 ORDER BY id DESC LIMIT 1`

	cronJobLastOutputHashSQL = `SELECT last_run_output_hash FROM cron_jobs WHERE id = $1`
)

// cronSkills packs the eight skill slots and their count into the CSV the
// column stores, and unpacks them again on the way out.
//
// The wire carries a fixed eight slots plus a count; the column carries one
// comma-separated string. The count is what says how many slots are real --
// trailing empties are padding, not skills.
func cronSkillsToCSV(f []string, at int) (string, bool) {
	count, ok := store.Atoi(f[at+cronMaxSkills])
	if !ok || count < 0 || count > cronMaxSkills {
		return "", false
	}
	skills := make([]string, 0, count)
	for i := 0; i < count; i++ {
		if s := f[at+i]; s != "" {
			skills = append(skills, s)
		}
	}
	return strings.Join(skills, ","), true
}

func cronSkillsFromCSV(csv string) ([]string, int) {
	out := make([]string, cronMaxSkills)
	if strings.TrimSpace(csv) == "" {
		return out, 0
	}
	parts := strings.Split(csv, ",")
	count := 0
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if count >= cronMaxSkills {
			// More skills than the wire can carry. The COUNT still says how
			// many there are, so a caller can tell it is not seeing all of
			// them rather than believing the list is complete.
			count++
			continue
		}
		out[count] = part
		count++
	}
	return out, count
}

// cronJobRow reads a job as its wire cells.
func cronJobRow(scan func(...any) error) ([]string, error) {
	var (
		id, schedule, mode, script, prompt string
		workdir, contextFrom, whenContains string
		skillsCSV, deliverTarget           string
		onlyIfChanged, firstRunSilent      bool
		preWakeGate, enabled               bool
	)
	if err := scan(&id, &schedule, &mode, &script, &prompt, &workdir,
		&contextFrom, &whenContains, &skillsCSV, &deliverTarget,
		&onlyIfChanged, &firstRunSilent, &preWakeGate, &enabled); err != nil {
		return nil, err
	}
	skills, count := cronSkillsFromCSV(skillsCSV)

	out := []string{
		id, schedule, mode, script, prompt, workdir, contextFrom, whenContains,
	}
	out = append(out, skills...)
	out = append(out, store.Itoa(count), deliverTarget,
		store.Btoa(onlyIfChanged), store.Btoa(firstRunSilent),
		store.Btoa(preWakeGate), store.Btoa(enabled))
	return out, nil
}

func cronJobUpsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, schedule, mode := f[0], f[1], f[2]
	if id == "" || schedule == "" {
		return store.StatusInvalid, nil, nil
	}
	switch mode {
	case "script", "llm", "hybrid":
	default:
		return store.StatusInvalid, nil, nil
	}
	// A job with nothing to run fires forever and does nothing. The schema
	// refuses it too; refusing here names it rather than surfacing a
	// constraint violation.
	script, prompt := f[3], f[4]
	if (mode == "script" && script == "") ||
		(mode == "llm" && prompt == "") ||
		(mode == "hybrid" && (script == "" || prompt == "")) {
		return store.StatusInvalid, nil, nil
	}

	skillsCSV, ok := cronSkillsToCSV(f, 8)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	flags := make([]bool, 0, 4)
	for _, at := range []int{18, 19, 20, 21} {
		v, ok := store.Atob(f[at])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		flags = append(flags, v)
	}

	if _, err := q.Exec(ctx, cronJobUpsertSQL,
		id, schedule, mode, script, prompt, f[5], f[6], f[7],
		skillsCSV, f[17], flags[0], flags[1], flags[2], flags[3]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

func cronJobGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := cronJobRow(func(dest ...any) error {
		return q.QueryRow(ctx, cronJobGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func cronJobLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	enabledOnly, ok := store.Atob(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, cronJobLoadSQL, cronCells, cronJobRow, max, enabledOnly)
}

func cronJobSetEnabled(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	enabled, ok := store.Atob(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, cronJobSetEnabledSQL, f[0], enabled)
}

// cronJobSetEnabledAll turns every job on or off. Touching no jobs is success:
// there were none to touch, which is what the caller asked to be true.
func cronJobSetEnabledAll(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	enabled, ok := store.Atob(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, cronJobSetEnabledAllSQL, enabled); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// cronJobDelete removes a job and, through the cascade, its run history.
//
// The C's reference had no cascade, so deleting a job left its runs behind
// naming a job that no longer existed -- and the history read would keep
// returning them.
func cronJobDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	return touchedOrMissing(ctx, q, cronJobDeleteSQL, f[0])
}

// cronJobRecordRun records a run and updates the job's last-run summary, in one
// transaction so the two cannot disagree.
func cronJobRecordRun(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	silent, ok := store.Atob(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	delivered, ok := store.Atob(f[3])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	var snapshot any
	if f[7] != "" {
		if text, valid := cronOutputIRText(f[7]); !valid || text != f[4] {
			return store.StatusInvalid, nil, nil
		}
		snapshot = f[7]
	}
	var runID int64
	if err := q.QueryRow(ctx, cronJobRecordRunSQL,
		f[0], f[1], silent, delivered, f[4], f[5], f[6], snapshot).Scan(&runID); err != nil {
		return store.StatusFailed, nil, err
	}
	if _, err := q.Exec(ctx, cronJobTouchLastRunSQL, f[0], f[1], f[6]); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(runID)}, nil
}

func cronJobListJSON(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	var document string
	if err := q.QueryRow(ctx, cronJobListJSONSQL).Scan(&document); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{document}, nil
}

func cronJobHistoryJSON(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[1])
	if f[0] == "" || !ok {
		return store.StatusInvalid, nil, nil
	}
	var document string
	if err := q.QueryRow(ctx, cronJobHistoryJSONSQL, f[0], max).Scan(&document); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{document}, nil
}

func cronJobLatestOutput(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var output, snapshot string
	err := q.QueryRow(ctx, cronJobLatestOutputSQL, f[0]).Scan(&output, &snapshot)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if text, valid := cronOutputIRText(snapshot); snapshot != "" && (!valid || text != output) {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{output, snapshot}, nil
}

func cronJobLastOutputHash(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var hash string
	err := q.QueryRow(ctx, cronJobLastOutputHashSQL, f[0]).Scan(&hash)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{hash}, nil
}

// output_ir is an independent persisted discriminator. Legacy output is never
// parsed, regardless of marker-like or JSON-looking bytes in its display text.
func cronOutputIRText(raw string) (string, bool) {
	if len(raw) < 1 || len(raw) > 1048576 {
		return "", false
	}
	var envelope struct {
		Schema string          `json:"schema"`
		Kind   string          `json:"kind"`
		Value  json.RawMessage `json:"value"`
	}
	if !canonical.Decode([]byte(raw), &envelope) || envelope.Schema != "aimee.cron-output.v1" {
		return "", false
	}
	switch envelope.Kind {
	case "response":
		response, err := canonical.DecodeResponse(envelope.Value)
		if err != nil {
			return "", false
		}
		return response.DisplayText(), response.CompleteText()
	case "script":
		var message canonical.Message
		if !canonical.ValidMessage(envelope.Value) || !canonical.Decode(envelope.Value, &message) || message.Role != "user" || len(message.Blocks) != 1 || message.Blocks[0].Type != "text" || message.Blocks[0].Text == nil {
			return "", false
		}
		return *message.Blocks[0].Text, true
	default:
		return "", false
	}
}
