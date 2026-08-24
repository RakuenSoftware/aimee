package families

import (
	"context"
	"fmt"
	"math"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Diagnoses: a symptom, the hypotheses raised about it, and the evidence for
// and against each.

// Evidence ranks, strongest first. The rank is how DIRECT the evidence is, so a
// lower number is stronger.
const (
	diagRankDirect      = 1
	diagRankLog         = 2
	diagRankCode        = 3
	diagRankSpeculation = 4
)

// Reply widths, from the catalog.
const (
	diagnosisCells      = 7
	diagnosisItemCells  = 8
	diagnosisRankCells  = diagnosisItemCells + 5
	diagnosisProbeCells = 3
)

// diagSuggestBalanceThreshold is how close two hypotheses must be in confidence
// before a probe is worth suggesting to tell them apart.
const diagSuggestBalanceThreshold = 0.15

const diagnosisColumns = `id, symptom, status, conclusion, confidence,
                          to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                          to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

// parent_id is stored NULL for an item that hangs off nothing and rendered as
// the wire's 0. See the note on the column in schema_telemetry.sql.
const diagnosisItemColumns = `id, diagnosis_id, kind, COALESCE(parent_id, 0), content, source,
                              evidence_rank,
                              to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	diagnoseStartSQL = `INSERT INTO diagnoses (symptom, status) VALUES ($1, 'active')
	                    RETURNING id`

	diagnoseAddItemSQL = `INSERT INTO diagnosis_items
	                          (diagnosis_id, kind, parent_id, content, source, evidence_rank)
	                      VALUES ($1, $2, NULLIF($3::bigint, 0), $4, $5, $6)
	                      RETURNING id`

	// Adding to a diagnosis touches it, so "most recently worked on" means what
	// it says. The C did this as a separate statement after each insert, which
	// could fail on its own and leave a diagnosis that had just gained an item
	// looking untouched.
	diagnoseTouchSQL = `UPDATE diagnoses SET updated_at = now() WHERE id = $1`

	diagnoseGetSQL = `SELECT ` + diagnosisColumns + ` FROM diagnoses WHERE id = $1`

	diagnoseListSQL = `SELECT ` + diagnosisColumns + `
	                     FROM diagnoses ORDER BY updated_at DESC, id DESC LIMIT $1`

	diagnoseListItemsSQL = `SELECT ` + diagnosisItemColumns + `
	                          FROM diagnosis_items WHERE diagnosis_id = $1
	                          ORDER BY id ASC LIMIT $2`

	diagnoseListHypothesesSQL = `SELECT ` + diagnosisItemColumns + `
	                               FROM diagnosis_items
	                              WHERE diagnosis_id = $1 AND kind = 'hypothesis'
	                              ORDER BY id ASC LIMIT $2`

	// A conclusion only lands on a diagnosis that is still open. The state is in
	// the WHERE clause, so two callers concluding at once cannot both win.
	diagnoseConcludeSQL = `UPDATE diagnoses
	                          SET status = 'concluded', conclusion = $2, confidence = $3,
	                              updated_at = now()
	                        WHERE id = $1 AND status = 'active'`

	diagnoseAbandonSQL = `UPDATE diagnoses
	                         SET status = 'abandoned', updated_at = now()
	                       WHERE id = $1 AND status = 'active'`

	// The ranking, done where the evidence is.
	//
	// The C read every hypothesis into a fixed array of 64, TRUNCATED it to the
	// caller's max, and only then counted evidence and sorted. So asking for the
	// top three hypotheses returned the three OLDEST, ranked among themselves --
	// the same "cut before you rank" mistake as the window search. Ranking is a
	// property of the whole set, so it happens before the limit here.
	//
	// The weights are the C's: direct 1.0, log 0.6, code 0.3, anything else 0.1,
	// evidence against subtracting what evidence for would have added.
	diagnoseRankSQL = `WITH weighted AS (
	        SELECT h.id, h.diagnosis_id, h.kind, h.parent_id, h.content, h.source,
	               h.evidence_rank, h.created_at,
	               COUNT(*) FILTER (WHERE e.kind = 'evidence_for')     AS for_count,
	               COUNT(*) FILTER (WHERE e.kind = 'evidence_against') AS against_count,
	               COALESCE(MIN(e.evidence_rank) FILTER (
	                   WHERE e.kind = 'evidence_for'), 0)              AS strongest_for,
	               COALESCE(MIN(e.evidence_rank) FILTER (
	                   WHERE e.kind = 'evidence_against'), 0)          AS strongest_against,
	               COALESCE(SUM(
	                   CASE WHEN e.kind = 'evidence_for' THEN 1 ELSE -1 END *
	                   CASE LEAST(GREATEST(e.evidence_rank, 1), 4)
	                        WHEN 1 THEN 1.0 WHEN 2 THEN 0.6 WHEN 3 THEN 0.3 ELSE 0.1 END
	               ), 0) AS score
	          FROM diagnosis_items h
	          LEFT JOIN diagnosis_items e
	                 ON e.parent_id = h.id
	                AND e.kind IN ('evidence_for', 'evidence_against')
	         WHERE h.diagnosis_id = $1 AND h.kind = 'hypothesis'
	         GROUP BY h.id
	    )
	    SELECT id, diagnosis_id, kind, COALESCE(parent_id, 0), content, source,
	           evidence_rank,
	           to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
	           for_count, against_count, strongest_for, strongest_against,
	           -- the logistic squash of a bounded score, so confidence is a
	           -- probability rather than an unbounded tally
	           1.0 / (1.0 + exp(-LEAST(GREATEST(score, -20.0), 20.0)))
	      FROM weighted
	     ORDER BY 13 DESC, id ASC
	     LIMIT $2`
)

func diagnosisRow(scan func(...any) error) ([]string, error) {
	var (
		id                          int64
		symptom, status, conclusion string
		createdAt, updatedAt        string
		confidence                  float64
	)
	if err := scan(&id, &symptom, &status, &conclusion, &confidence,
		&createdAt, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), symptom, status, conclusion,
		store.Ftoa(confidence), createdAt, updatedAt,
	}, nil
}

func diagnosisItemRow(scan func(...any) error) ([]string, error) {
	var (
		id, diagID, parentID, rank int64
		kind, content, source, at  string
	)
	if err := scan(&id, &diagID, &kind, &parentID, &content, &source, &rank, &at); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), store.I64toa(diagID), kind,
		store.I64toa(parentID), content, source,
		store.I64toa(rank), at,
	}, nil
}

func diagnoseStart(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, diagnoseStartSQL, f[0]).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

// addDiagnosisItem is what every add operation does: record the item and touch
// the diagnosis, in one transaction so the two cannot disagree.
func addDiagnosisItem(ctx context.Context, q store.Queryer,
	diagID int64, kind string, parentID int64, content, source string, rank int64) (
	uint32, []string, error) {

	if diagID <= 0 || content == "" {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	err := q.QueryRow(ctx, diagnoseAddItemSQL,
		diagID, kind, parentID, content, source, rank).Scan(&id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if _, err := q.Exec(ctx, diagnoseTouchSQL, diagID); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{store.I64toa(id)}, nil
}

func diagnoseAddObservation(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	diagID, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return addDiagnosisItem(ctx, q, diagID, "observation", 0, f[1], f[2], diagRankSpeculation)
}

func diagnoseAddHypothesis(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	diagID, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return addDiagnosisItem(ctx, q, diagID, "hypothesis", 0, f[1], "", diagRankSpeculation)
}

// diagnoseAddEvidence records evidence bearing on a hypothesis.
//
// The hypothesis is required: evidence with no parent is counted toward no
// hypothesis and is invisible to the ranking, which is the same as not having
// been recorded at all. The schema refuses it too.
func diagnoseAddEvidence(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	diagID, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	hypothesisID, ok := store.Atoi64(f[1])
	if !ok || hypothesisID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	kind := f[2]
	if kind != "evidence_for" && kind != "evidence_against" {
		return store.StatusInvalid, nil, nil
	}
	rank, ok := store.Atoi64(f[5])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// The rank is clamped rather than refused, matching the C: a caller that
	// says "5" means "weaker than anything I have a name for".
	if rank < diagRankDirect {
		rank = diagRankDirect
	}
	if rank > diagRankSpeculation {
		rank = diagRankSpeculation
	}
	return addDiagnosisItem(ctx, q, diagID, kind, hypothesisID, f[3], f[4], rank)
}

func diagnoseAddProbe(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	diagID, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	hypothesisID, ok := store.Atoi64(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return addDiagnosisItem(ctx, q, diagID, "probe", hypothesisID, f[2], "", diagRankSpeculation)
}

func diagnoseGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reply, err := diagnosisRow(func(dest ...any) error {
		return q.QueryRow(ctx, diagnoseGetSQL, id).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func diagnoseList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	max, ok := boundedMax(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, diagnoseListSQL, diagnosisCells, diagnosisRow, max)
}

// listDiagnosisItems is the shape both item listings share.
func listDiagnosisItems(sql string) store.OpFunc {
	return func(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
		id, ok := store.Atoi64(f[0])
		if !ok || id <= 0 {
			return store.StatusInvalid, nil, nil
		}
		max, ok := boundedMax(f[1])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		return collect(ctx, q, sql, diagnosisItemCells, diagnosisItemRow, id, max)
	}
}

// ranking is one hypothesis and what the evidence says about it.
type ranking struct {
	item             []string
	id               int64
	content          string
	forCount         int64
	againstCount     int64
	strongestFor     int64
	strongestAgainst int64
	confidence       float64
}

func scanRankings(ctx context.Context, q store.Queryer, diagID int64, max int) ([]ranking, error) {
	rows, err := q.Query(ctx, diagnoseRankSQL, diagID, max)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []ranking
	for rows.Next() {
		var (
			r                           ranking
			diagnosisID, parentID, rank int64
			kind, source, at            string
		)
		if err := rows.Scan(&r.id, &diagnosisID, &kind, &parentID, &r.content, &source,
			&rank, &at, &r.forCount, &r.againstCount,
			&r.strongestFor, &r.strongestAgainst, &r.confidence); err != nil {
			return nil, err
		}
		r.item = []string{
			store.I64toa(r.id), store.I64toa(diagnosisID), kind,
			store.I64toa(parentID), r.content, source,
			store.I64toa(rank), at,
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

func diagnoseRankHypotheses(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	diagID, ok := store.Atoi64(f[0])
	if !ok || diagID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	ranked, err := scanRankings(ctx, q, diagID, max)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	cells := make([]string, 0, len(ranked)*diagnosisRankCells)
	for _, r := range ranked {
		cells = append(cells, r.item...)
		cells = append(cells,
			store.I64toa(r.forCount), store.I64toa(r.againstCount),
			store.I64toa(r.strongestFor), store.I64toa(r.strongestAgainst),
			store.Ftoa(r.confidence))
	}
	return store.StatusOK, cells, nil
}

// weakestEvidenceLabel names the WEAKEST evidence a hypothesis rests on, which
// is what decides what kind of probe would actually help. A hypothesis with no
// evidence at all rests on speculation.
func weakestEvidenceLabel(r ranking) string {
	weakestFor, weakestAgainst := r.strongestFor, r.strongestAgainst
	if weakestFor <= 0 {
		weakestFor = diagRankSpeculation
	}
	if weakestAgainst <= 0 {
		weakestAgainst = diagRankSpeculation
	}
	weakest := weakestFor
	if weakestAgainst > weakest {
		weakest = weakestAgainst
	}
	switch {
	case weakest <= diagRankLog:
		return "log/metric"
	case weakest <= diagRankCode:
		return "code"
	default:
		return "speculation"
	}
}

// truncate cuts to n characters for display inside a suggestion.
func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

// suggestProbes is pure text: it reads a ranking and says what would move it.
//
// Two shapes of suggestion. One hypothesis is worth probing on its own when it
// is genuinely undecided and nothing argues against it. Two are worth telling
// apart when they sit at nearly the same confidence and neither is close to
// settled -- there is no point discriminating between hypotheses when one has
// already won.
func suggestProbes(ranked []ranking, max int) [][3]string {
	var out [][3]string

	for i, r := range ranked {
		if len(out) >= max {
			return out
		}
		if r.confidence >= 0.35 && r.confidence <= 0.65 && r.againstCount == 0 {
			out = append(out, [3]string{
				store.I64toa(r.id), "0",
				fmt.Sprintf("H%d (%q) is weakly evidenced (confidence %.2f, %s-level). "+
					"Collect a direct experiment or log correlation to strengthen or refute it.",
					i+1, truncate(r.content, 60), r.confidence, weakestEvidenceLabel(r)),
			})
		}
	}

	for i := range ranked {
		for j := i + 1; j < len(ranked); j++ {
			if len(out) >= max {
				return out
			}
			a, b := ranked[i], ranked[j]
			if math.Abs(a.confidence-b.confidence) > diagSuggestBalanceThreshold {
				continue
			}
			// One of them is already close to settled, so telling them apart is
			// no longer the useful question.
			if a.confidence > 0.80 || b.confidence > 0.80 {
				continue
			}
			var suggestion string
			switch weakestEvidenceLabel(a) {
			case "speculation":
				suggestion = fmt.Sprintf(
					"H%d and H%d are tied at %.2f vs %.2f (speculation only). "+
						"Run a direct experiment -- a targeted test or controlled "+
						"observation -- to discriminate %q from %q.",
					i+1, j+1, a.confidence, b.confidence,
					truncate(a.content, 50), truncate(b.content, 50))
			case "log/metric":
				suggestion = fmt.Sprintf(
					"H%d and H%d are tied at %.2f vs %.2f (log-level evidence). "+
						"Collect time-correlated metrics or traces at the event boundary "+
						"to distinguish %q from %q.",
					i+1, j+1, a.confidence, b.confidence,
					truncate(a.content, 50), truncate(b.content, 50))
			default:
				suggestion = fmt.Sprintf(
					"H%d and H%d are tied at %.2f vs %.2f (code-level evidence). "+
						"Trace the code path at the decision point to confirm %q over %q.",
					i+1, j+1, a.confidence, b.confidence,
					truncate(a.content, 50), truncate(b.content, 50))
			}
			out = append(out, [3]string{
				store.I64toa(a.id), store.I64toa(b.id), suggestion,
			})
		}
	}
	return out
}

func diagnoseSuggestProbes(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	diagID, ok := store.Atoi64(f[0])
	if !ok || diagID <= 0 {
		return store.StatusInvalid, nil, nil
	}
	max, ok := boundedMax(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	// The ranking is read in full rather than to `max`: the suggestions come
	// from PAIRS, so cutting the ranking first would hide the pair that most
	// needs telling apart.
	ranked, err := scanRankings(ctx, q, diagID, diagnoseRankScanMax)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	cells := make([]string, 0, max*diagnosisProbeCells)
	for _, s := range suggestProbes(ranked, max) {
		cells = append(cells, s[0], s[1], s[2])
	}
	return store.StatusOK, cells, nil
}

// diagnoseRankScanMax is how many hypotheses the probe suggester considers. The
// C used a fixed array of this size for the same reason.
const diagnoseRankScanMax = 64

// concludeOrAbandon is the shape both closing operations share: they only act
// on a diagnosis that is still open.
func concludeOrAbandon(ctx context.Context, q store.Queryer, sql string, args ...any) (uint32, []string, error) {
	tag, err := q.Exec(ctx, sql, args...)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		// Either it does not exist or it was already closed. Both mean this
		// call did not close it.
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}

func diagnoseConclude(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	// A conclusion with nothing in it is indistinguishable from never having
	// concluded, and the caller cannot tell which happened. The schema refuses
	// it too.
	if strings.TrimSpace(f[1]) == "" {
		return store.StatusInvalid, nil, nil
	}
	confidence, ok := store.Atof(f[2])
	if !ok || confidence < 0 || confidence > 1 {
		return store.StatusInvalid, nil, nil
	}
	return concludeOrAbandon(ctx, q, diagnoseConcludeSQL, id, f[1], confidence)
}

func diagnoseAbandon(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	return concludeOrAbandon(ctx, q, diagnoseAbandonSQL, id)
}
