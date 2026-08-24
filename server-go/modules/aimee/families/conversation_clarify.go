package families

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Clarification sessions: a description, and up to eight question/answer pairs
// across five dimensions, scored until the description is considered specified
// enough to plan against.
//
// Four of these operations (score, weakest_dim, next_question, crystallize)
// touch no table at all. They are pure functions of a session that the CALLER
// sends in full -- all 48 wire fields of it -- and they ended up in the store
// module only because the wire routed them there. They are kept here so the
// wire contract is unchanged, but nothing about them belongs to a database, and
// they are the first thing to lift out once the callers are Go too.

const (
	clarifyMaxQA      = 8
	clarifyNumDims    = 5
	clarifyReadyScore = 0.75

	// A session is sent as 4 leading scalars, 8 repeats of a 5-member pair,
	// then 4 trailing scalars.
	clarifyQAOffset   = 4
	clarifyQAWidth    = 5
	clarifySessFields = clarifyQAOffset + clarifyMaxQA*clarifyQAWidth + 4 // 48
)

// clarifyDims is the dimension metadata, in the order that defines a
// dimension's index on the wire and in the scoring.
var clarifyDims = [clarifyNumDims]struct{ name, question string }{
	{"scope", "What is in scope for this task, and what should explicitly be left out?"},
	{"success_criteria", "How will you know this task is complete? What does a successful outcome look like?"},
	{"constraints", "Are there any hard constraints, requirements, or things that must not change?"},
	{"approach", "Do you have a preferred approach, technology, or pattern for this task?"},
	{"context", "What relevant context about the existing system should I know before starting?"},
}

func clarifyDimIndex(name string) int {
	for i, d := range clarifyDims {
		if d.name == name {
			return i
		}
	}
	return -1
}

type clarifyQA struct {
	dimension string
	question  string
	answer    string
	answered  int64
	seq       int64
}

type clarifySession struct {
	id          int64
	description string
	status      string
	score       float64
	qa          []clarifyQA
	spec        string
	createdAt   string
	updatedAt   string
}

// decodeClarifySession reads the 48-field session the caller sent.
//
// qaCount bounds how much of the fixed-width qa array is real; a count past the
// array is a malformed request rather than something to clamp, because clamping
// would silently score a session against fewer pairs than the caller believes
// it sent.
func decodeClarifySession(f []string) (clarifySession, bool) {
	if len(f) != clarifySessFields {
		return clarifySession{}, false
	}
	id, err := strconv.ParseInt(f[0], 10, 64)
	if err != nil {
		return clarifySession{}, false
	}
	statusCode, err := strconv.ParseInt(f[2], 10, 64)
	if err != nil {
		return clarifySession{}, false
	}
	score, err := strconv.ParseFloat(f[3], 64)
	if err != nil {
		return clarifySession{}, false
	}
	countAt := clarifyQAOffset + clarifyMaxQA*clarifyQAWidth
	qaCount, err := strconv.ParseInt(f[countAt], 10, 64)
	if err != nil || qaCount < 0 || qaCount > clarifyMaxQA {
		return clarifySession{}, false
	}
	s := clarifySession{
		id:          id,
		description: f[1],
		status:      clarifyStatusName(statusCode),
		score:       score,
		spec:        f[countAt+1],
		createdAt:   f[countAt+2],
		updatedAt:   f[countAt+3],
	}
	for i := int64(0); i < qaCount; i++ {
		at := clarifyQAOffset + int(i)*clarifyQAWidth
		answered, err := strconv.ParseInt(f[at+3], 10, 64)
		if err != nil {
			return clarifySession{}, false
		}
		seq, err := strconv.ParseInt(f[at+4], 10, 64)
		if err != nil {
			return clarifySession{}, false
		}
		s.qa = append(s.qa, clarifyQA{
			dimension: f[at], question: f[at+1], answer: f[at+2],
			answered: answered, seq: seq,
		})
	}
	return s, true
}

// The wire carries status as an int; the table stores it as a name.
func clarifyStatusName(code int64) string {
	switch code {
	case 1:
		return "ready"
	case 2:
		return "cancelled"
	default:
		return "open"
	}
}

func clarifyStatusCode(name string) int64 {
	switch name {
	case "ready":
		return 1
	case "cancelled":
		return 2
	default:
		return 0
	}
}

// --- scoring ---------------------------------------------------------------

// clarifyDimScore is how well one dimension is covered: nothing, one answer, or
// more than one.
func clarifyDimScore(s clarifySession, dim int) float64 {
	answered := 0
	for _, qa := range s.qa {
		if qa.answered != 0 && clarifyDimIndex(qa.dimension) == dim {
			answered++
		}
	}
	switch {
	case answered == 0:
		return 0
	case answered == 1:
		return 0.7
	default:
		return 1
	}
}

// clarifyScore is a description-length base plus the average dimension
// coverage, capped at 1.
func clarifyScore(s clarifySession) float64 {
	var base float64
	switch n := len(s.description); {
	case n >= 200:
		base = 0.30
	case n >= 80:
		base = 0.15
	default:
		base = 0.05
	}
	total := 0.0
	for i := 0; i < clarifyNumDims; i++ {
		total += clarifyDimScore(s, i)
	}
	score := base + (total/clarifyNumDims)*0.70
	if score > 1 {
		score = 1
	}
	return score
}

// clarifyWeakestDim is the least-covered dimension that is not already waiting
// on an answer.
//
// The C skipped dimensions with a pending question -- but only inside a loop
// that started at index 1, seeding the answer with dimension 0 and never
// applying the check to it. A session with an unanswered "scope" question and
// nothing else outstanding was told to ask about scope AGAIN, because scope was
// the seed and no later dimension could beat a zero score. Here the check
// covers every dimension, including the first.
//
// If every dimension is pending there is no unblocked one to name; the C always
// returned something, so the first dimension is the fallback and the caller's
// own qa_count limit is what ends the loop.
func clarifyWeakestDim(s clarifySession) string {
	pending := func(dim int) bool {
		for _, qa := range s.qa {
			if qa.answered == 0 && clarifyDimIndex(qa.dimension) == dim {
				return true
			}
		}
		return false
	}
	weakest, weakestScore := -1, 0.0
	for i := 0; i < clarifyNumDims; i++ {
		if pending(i) {
			continue
		}
		if sc := clarifyDimScore(s, i); weakest < 0 || sc < weakestScore {
			weakest, weakestScore = i, sc
		}
	}
	if weakest < 0 {
		return clarifyDims[0].name
	}
	return clarifyDims[weakest].name
}

// clarifyNextQuestion picks the question to ask next. The second return is
// whether there is one at all: a session that is ready, or that has used up its
// pairs, has no next question and that is an answer, not a failure.
func clarifyNextQuestion(s clarifySession) (question, dimension string, ok bool) {
	if s.status == "ready" || clarifyScore(s) >= clarifyReadyScore {
		return "", "", false
	}
	if len(s.qa) >= clarifyMaxQA {
		return "", "", false
	}
	d := clarifyDimIndex(clarifyWeakestDim(s))
	if d < 0 {
		return "", "", false
	}
	return clarifyDims[d].question, clarifyDims[d].name, true
}

// clarifyCrystallize renders the session as a markdown specification.
func clarifyCrystallize(s clarifySession) string {
	var b strings.Builder
	fmt.Fprintf(&b, "# Task Specification\n\n## Task\n%s\n", s.description)
	if len(s.qa) > 0 {
		// The header rides on there being pairs at all, not on any being
		// answered, which is what the C settled on.
		b.WriteString("\n## Clarifications\n")
		for _, qa := range s.qa {
			if qa.answered == 0 {
				continue
			}
			fmt.Fprintf(&b, "\n**%s**: %s\n> %s\n", qa.dimension, qa.question, qa.answer)
		}
	}
	return b.String()
}

// --- the four pure operations ----------------------------------------------

func clarifyScoreOp(_ context.Context, _ store.Queryer, f []string) (uint32, []string, error) {
	s, ok := decodeClarifySession(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{store.Ftoa(clarifyScore(s))}, nil
}

func clarifyWeakestDimOp(_ context.Context, _ store.Queryer, f []string) (uint32, []string, error) {
	s, ok := decodeClarifySession(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{clarifyWeakestDim(s)}, nil
}

// clarifyNextQuestionOp answers with the question to ask and its dimension.
//
// The C reported this operation's status INVERTED. Its helper returns 0 when it
// produced a question and 1 when none is needed, and the dispatch mapped a
// positive return to OK and zero to MISSING -- so a caller was told MISSING
// exactly when a question had been produced (the cells carried it regardless),
// and OK, with two empty cells, when the session was ready and there was none.
// A caller that trusted the status would never ask a clarifying question.
//
// Here OK means there is a question and MISSING means there is not.
func clarifyNextQuestionOp(_ context.Context, _ store.Queryer, f []string) (uint32, []string, error) {
	s, ok := decodeClarifySession(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	question, dimension, has := clarifyNextQuestion(s)
	if !has {
		return store.StatusMissing, []string{"", ""}, nil
	}
	return store.StatusOK, []string{question, dimension}, nil
}

func clarifyCrystallizeOp(_ context.Context, _ store.Queryer, f []string) (uint32, []string, error) {
	s, ok := decodeClarifySession(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{clarifyCrystallize(s)}, nil
}

// --- the three stored operations -------------------------------------------

const (
	clarifySelectSQL = `SELECT id, description, status, score, spec,
	                           to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
	                           to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')
	                      FROM clarify_sessions WHERE id = $1`

	clarifyQASQL = `SELECT dimension, question, answer, answered, seq
	                  FROM clarify_qa WHERE session_id = $1 ORDER BY seq ASC LIMIT $2`

	clarifyInsertSQL = `INSERT INTO clarify_sessions (description, status, score, spec)
	                    VALUES ($1, 'open', 0, '') RETURNING id`

	clarifyInsertQASQL = `INSERT INTO clarify_qa
	                          (session_id, dimension, question, answer, answered, seq)
	                      VALUES ($1, $2, $3, '', 0, $4)`
)

// loadClarifySession reads a session and its pairs.
func loadClarifySession(ctx context.Context, q store.Queryer, id int64) (clarifySession, bool, error) {
	var s clarifySession
	err := q.QueryRow(ctx, clarifySelectSQL, id).Scan(
		&s.id, &s.description, &s.status, &s.score, &s.spec, &s.createdAt, &s.updatedAt)
	if store.IsNoRows(err) {
		return clarifySession{}, false, nil
	}
	if err != nil {
		return clarifySession{}, false, err
	}
	rows, err := q.Query(ctx, clarifyQASQL, id, clarifyMaxQA)
	if err != nil {
		return clarifySession{}, false, err
	}
	defer rows.Close()
	for rows.Next() {
		var qa clarifyQA
		if err := rows.Scan(&qa.dimension, &qa.question, &qa.answer, &qa.answered, &qa.seq); err != nil {
			return clarifySession{}, false, err
		}
		s.qa = append(s.qa, qa)
	}
	return s, true, rows.Err()
}

// encodeClarifySession is the reply form: the same 48 fields the pure
// operations accept, so a caller can round-trip a session without reshaping it.
func encodeClarifySession(s clarifySession) []string {
	out := make([]string, clarifySessFields)
	out[0] = strconv.FormatInt(s.id, 10)
	out[1] = s.description
	out[2] = strconv.FormatInt(clarifyStatusCode(s.status), 10)
	out[3] = store.Ftoa(s.score)
	for i := 0; i < clarifyMaxQA; i++ {
		at := clarifyQAOffset + i*clarifyQAWidth
		if i < len(s.qa) {
			qa := s.qa[i]
			out[at], out[at+1], out[at+2] = qa.dimension, qa.question, qa.answer
			out[at+3] = strconv.FormatInt(qa.answered, 10)
			out[at+4] = strconv.FormatInt(qa.seq, 10)
			continue
		}
		out[at+3], out[at+4] = "0", "0"
	}
	countAt := clarifyQAOffset + clarifyMaxQA*clarifyQAWidth
	out[countAt] = strconv.Itoa(len(s.qa))
	out[countAt+1] = s.spec
	out[countAt+2] = s.createdAt
	out[countAt+3] = s.updatedAt
	return out
}

// clarifyStart opens a session and seeds it with its first question.
//
// The C did the insert, the question selection and the seeding question's
// insert as three unrelated statements, so a failure between them left a
// session with no opening question and no indication of it. One transaction
// makes the session and its first question arrive together or not at all.
func clarifyStart(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	description := f[0]
	if description == "" {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	if err := q.QueryRow(ctx, clarifyInsertSQL, description).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	seed := clarifySession{id: id, description: description, status: "open"}
	if question, dimension, ok := clarifyNextQuestion(seed); ok {
		if _, err := q.Exec(ctx, clarifyInsertQASQL, id, dimension, question, 0); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	s, found, err := loadClarifySession(ctx, q, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if !found {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, encodeClarifySession(s), nil
}

func clarifyGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	s, found, err := loadClarifySession(ctx, q, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if !found {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, encodeClarifySession(s), nil
}

const (
	// The answer lands on the oldest unanswered pair, which is the one the
	// caller was asked. Ordering by seq makes "oldest" the sequence the caller
	// sees rather than whatever order the rows happen to sit in.
	clarifyAnswerSQL = `UPDATE clarify_qa SET answer = $2, answered = 1
	                     WHERE id = (SELECT id FROM clarify_qa
	                                  WHERE session_id = $1 AND answered = 0
	                                  ORDER BY seq ASC, id ASC LIMIT 1)`

	clarifySaveSQL = `UPDATE clarify_sessions
	                     SET status = $2, score = $3, spec = $4, updated_at = now()
	                   WHERE id = $1`
)

// clarifyAnswer records an answer, then re-scores the session and either
// crystallizes it or asks the next question.
//
// This is one transaction because the three writes are one decision: an answer
// that landed without the re-score would leave a session whose stored score
// disagrees with its own pairs, and the next call would read that stale score
// and could declare the session ready on the strength of it.
func clarifyAnswer(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, clarifyAnswerSQL, id, f[1])
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		// Either the session does not exist or it has nothing outstanding.
		// Both mean there was no question this answered.
		return store.StatusMissing, nil, nil
	}
	s, found, err := loadClarifySession(ctx, q, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if !found {
		return store.StatusFailed, nil, nil
	}

	s.score = clarifyScore(s)
	if s.score >= clarifyReadyScore {
		s.status = "ready"
		s.spec = clarifyCrystallize(s)
	} else if question, dimension, has := clarifyNextQuestion(s); has {
		if _, err := q.Exec(ctx, clarifyInsertQASQL, id, dimension, question, len(s.qa)); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	if _, err := q.Exec(ctx, clarifySaveSQL, id, s.status, s.score, s.spec); err != nil {
		return store.StatusFailed, nil, err
	}
	// Re-read so the reply carries the question just seeded rather than the
	// session as it stood before it.
	s, found, err = loadClarifySession(ctx, q, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if !found {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, encodeClarifySession(s), nil
}
