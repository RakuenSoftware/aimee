package families

import (
	"context"
	"encoding/json"
	"errors"
	"os"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Ensembles: templated, turn-based multi-agent runs bound to a channel.
//
// The state machine is in ensemble_state.go. What is here is the storage and
// the one thing the C got structurally wrong: advance is a read-modify-write,
// and nothing serialised it.

// EventEnsemble and StageEnsemble are from the catalog: ref 30's kind block.
const (
	EventEnsemble uint32 = 11786
	StageEnsemble uint32 = 10
)

const (
	opEnsembleCreate               = 1
	opEnsembleView                 = 2
	opEnsembleAdvance              = 3
	opEnsemblePause                = 4
	opEnsembleList                 = 5
	opEnsembleFindCurrentByChannel = 6
)

// The verdict codes an ensemble operation can carry back alongside a
// successful reply. These are the ENSEMBLE's answers -- "expected alice, got
// bob", "already complete" -- not the store's. A broken store is still a
// failed reply; a run refusing a turn is a successful one that says no.
const (
	ensembleOK      = 0
	ensembleRefused = -1
)

// A run as the wire describes it: fourteen cells, three of which are not
// stored anywhere. phase_count, turns_in_phase and phase_name are properties of
// the TEMPLATE at the run's current position, so the template travels with the
// row and the three are computed from it on the way out.
//
// Storing them instead would mean three columns that silently disagree with the
// template the moment a run moves, which is the same trap the separate
// full-text index was in the conversation family.
const ensembleColumns = `id, template_name, channel, status, current_phase, current_turn,
                         expected_agent, expected_role, paused_reason,
                         to_char(created_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                         to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
                         template_json`

// ensembleCells is how many cells one run occupies on the wire.
const ensembleCells = 14

const (
	// ensembleLoadForUpdateSQL takes the row lock.
	//
	// advance reads a run, decides the next turn from it, and writes both the
	// new position and the appended transcript. The C did that as a plain
	// SELECT and a plain UPDATE with nothing between them -- the functions were
	// named _locked, but no lock of any kind existed anywhere in the module.
	// Two turns arriving together both read position N, both appended to the
	// transcript they had read, and the second write erased the first message
	// and re-took the same turn.
	//
	// FOR UPDATE makes the read and the write one decision.
	ensembleLoadForUpdateSQL = `SELECT status, current_phase, current_turn,
	                                   expected_agent, expected_role, paused_reason,
	                                   template_json, context_json
	                              FROM ensembles WHERE id = $1 FOR UPDATE`

	ensembleViewLoadSQL = `SELECT status, current_phase, current_turn,
	                              expected_agent, expected_role, paused_reason,
	                              template_json, context_json
	                         FROM ensembles WHERE id = $1`

	ensembleRowSQL = `SELECT ` + ensembleColumns + ` FROM ensembles WHERE id = $1`

	ensembleStoreSQL = `UPDATE ensembles
	                       SET status = $2, current_phase = $3, current_turn = $4,
	                           expected_agent = $5, expected_role = $6, paused_reason = $7,
	                           context_json = $8, updated_at = now()
	                     WHERE id = $1`

	ensembleInsertSQL = `INSERT INTO ensembles
	                         (template_name, channel, status, current_phase, current_turn,
	                          expected_agent, expected_role, paused_reason,
	                          template_json, assignments_json, context_json)
	                     VALUES ($1, $2, 'active', 0, 0, $3, $4, '', $5, $6, '[]')
	                     RETURNING id`

	ensemblePauseSQL = `UPDATE ensembles
	                       SET status = 'paused', paused_reason = $2, updated_at = now()
	                     WHERE id = $1`

	ensembleListSQL = `SELECT ` + ensembleColumns + ` FROM ensembles ORDER BY id`

	// The most usable run for a channel: an active one before a paused one,
	// a paused one before a completed one, most recently touched first.
	ensembleFindCurrentSQL = `SELECT id FROM ensembles
	                           WHERE channel = $1
	                           ORDER BY CASE status
	                                      WHEN 'active'   THEN 0
	                                      WHEN 'paused'   THEN 1
	                                      WHEN 'complete' THEN 2
	                                      ELSE 3
	                                    END,
	                                    updated_at DESC, id DESC
	                           LIMIT 1`
)

// loadedEnsemble is a run as the state machine needs it.
type loadedEnsemble struct {
	state    ensembleState
	template template
	ctx      []contextMessage
}

func loadEnsemble(ctx context.Context, q store.Queryer, sql string, id int64) (loadedEnsemble, bool, error) {
	var (
		l                     loadedEnsemble
		templateJSON, ctxJSON string
	)
	err := q.QueryRow(ctx, sql, id).Scan(
		&l.state.Status, &l.state.CurrentPhase, &l.state.CurrentTurn,
		&l.state.ExpectedAgent, &l.state.ExpectedRole, &l.state.PausedReason,
		&templateJSON, &ctxJSON)
	if store.IsNoRows(err) {
		return loadedEnsemble{}, false, nil
	}
	if err != nil {
		return loadedEnsemble{}, false, err
	}
	if l.template, err = parseTemplate(templateJSON); err != nil {
		return loadedEnsemble{}, false, err
	}
	if l.ctx, err = parseContext(ctxJSON); err != nil {
		return loadedEnsemble{}, false, err
	}
	return l, true, nil
}

// ensembleRow reads the fourteen wire cells describing a run.
//
// A run whose template will not parse still answers: the three derived cells
// come back empty rather than the whole row failing. A caller listing runs
// wants to SEE the broken one, which is the only way anyone finds out it is
// broken.
func ensembleRow(scan func(...any) error) ([]string, error) {
	var (
		id, currentPhase, currentTurn             int64
		templateName, channel, status             string
		expectedAgent, expectedRole, pausedReason string
		createdAt, updatedAt, templateJSON        string
	)
	if err := scan(&id, &templateName, &channel, &status, &currentPhase, &currentTurn,
		&expectedAgent, &expectedRole, &pausedReason, &createdAt, &updatedAt,
		&templateJSON); err != nil {
		return nil, err
	}

	phaseCount, turnsInPhase, phaseName := 0, 0, ""
	if tmpl, err := parseTemplate(templateJSON); err == nil {
		phaseCount = len(tmpl.Phases)
		// A complete run has no current phase to describe: it is finished, and
		// the C reported these as empty for exactly that reason.
		if status != "complete" {
			turnsInPhase = tmpl.turnsInPhase(int(currentPhase))
			phaseName = tmpl.phaseName(int(currentPhase))
		}
	}

	return []string{
		store.I64toa(id), templateName, channel, status,
		store.I64toa(currentPhase), store.I64toa(currentTurn),
		store.Itoa(phaseCount), store.Itoa(turnsInPhase), phaseName,
		expectedAgent, expectedRole, pausedReason, createdAt, updatedAt,
	}, nil
}

// viewReply is the shape both the read and the advance answer with: the
// verdict, the message, the row, the turn prompt and the transcript.
//
// These travel together because they are one observation. Asked separately --
// the row from one call, the prompt from the next -- a turn taken in between
// would pair turn N's row with turn N+1's prompt, and the result reads as a
// consistent run that never existed.
func viewReply(ctx context.Context, q store.Queryer, id int64, rc int, message string,
	prompt string, transcript []contextMessage) (uint32, []string, error) {

	row, err := ensembleRow(func(dest ...any) error {
		return q.QueryRow(ctx, ensembleRowSQL, id).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	encoded, err := encodeContext(transcript)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	reply := append([]string{store.Itoa(rc), message}, row...)
	return store.StatusOK, append(reply, prompt, encoded), nil
}

// loadTemplate resolves a template by name: the project root first, then the
// config directory, then the built-ins.
//
// The name is validated BEFORE it is joined to anything. The C checked only
// that it was non-empty and interpolated it into a path, so a name containing
// ../ read any .json file the daemon could reach and stored the contents where
// the next view would hand them back.
func loadTemplate(projectRoot, configDir, name string) (template, string, error) {
	if !validTemplateName(name) {
		return template{}, "", errors.New("invalid ensemble template name")
	}
	for _, path := range templateCandidates(projectRoot, configDir, name) {
		raw, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		t, err := parseTemplate(string(raw))
		if err != nil {
			return template{}, "", err
		}
		return t, string(raw), nil
	}
	builtin, ok := builtinTemplates[name]
	if !ok {
		return template{}, "", errors.New("ensemble template '" + name + "' not found")
	}
	t, err := parseTemplate(builtin)
	if err != nil {
		return template{}, "", err
	}
	return t, builtin, nil
}

// ensembleCreate starts a run: resolve the template, bind every turn to an
// agent, and record where it begins.
func ensembleCreate(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	projectRoot, configDir, name, channel, assignmentsJSON := f[0], f[1], f[2], f[3], f[4]
	if name == "" {
		return store.StatusInvalid, nil, nil
	}
	if channel == "" {
		channel = "general"
	}

	refuse := func(message string) (uint32, []string, error) {
		return store.StatusOK, []string{store.Itoa(ensembleRefused), message, "0"}, nil
	}

	t, _, err := loadTemplate(projectRoot, configDir, name)
	if err != nil {
		return refuse(err.Error())
	}
	assignments, err := parseAssignments(assignmentsJSON)
	if err != nil {
		return refuse(err.Error())
	}
	if err := expandAssignments(&t, assignments); err != nil {
		return refuse(err.Error())
	}
	if len(t.Phases) == 0 {
		return refuse("ensemble template has no phases")
	}

	// The template is stored WITH the assignments bound into it, so the run's
	// cast is fixed at creation. Re-resolving it per turn would let a template
	// edited mid-run change who is expected to speak next.
	bound, err := json.Marshal(t)
	if err != nil {
		return store.StatusFailed, nil, err
	}

	first := t.participantAt(0, 0)
	var expectedAgent, expectedRole string
	if first != nil {
		expectedAgent, expectedRole = first.Agent, first.Role
	}

	var id int64
	if err := q.QueryRow(ctx, ensembleInsertSQL,
		name, channel, expectedAgent, expectedRole,
		string(bound), assignmentsJSON).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.Itoa(ensembleOK), "", store.I64toa(id),
	}, nil
}

func ensembleView(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	l, found, err := loadEnsemble(ctx, q, ensembleViewLoadSQL, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if !found {
		return store.StatusMissing, nil, nil
	}
	prompt := ""
	if l.state.Status != "complete" {
		prompt = buildPrompt(l.template, l.ctx, l.state.CurrentPhase, l.state.CurrentTurn)
	}
	return viewReply(ctx, q, id, ensembleOK, "", prompt, l.ctx)
}

// ensembleAdvance applies one message to a run.
//
// The load takes the row lock, so the read that decides the next turn and the
// write that records it cannot be interleaved with another turn on the same
// run. This whole operation is one transaction for that reason.
func ensembleAdvance(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	sender, text := f[1], f[2]

	l, found, err := loadEnsemble(ctx, q, ensembleLoadForUpdateSQL, id)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if !found {
		return store.StatusMissing, nil, nil
	}

	// A refusal answers with the run as it stands, unchanged. It is a verdict,
	// not a store failure, so it rides a successful reply.
	refuse := func(message string) (uint32, []string, error) {
		prompt := ""
		if l.state.Status != "complete" {
			prompt = buildPrompt(l.template, l.ctx, l.state.CurrentPhase, l.state.CurrentTurn)
		}
		return viewReply(ctx, q, id, ensembleRefused, message, prompt, l.ctx)
	}

	if l.state.Status == "complete" {
		return refuse("ensemble is already complete")
	}
	if sender == "" {
		return refuse("--speaker is required")
	}

	state, transcript, outcome := advance(l.template, l.state, l.ctx, sender, text)
	if outcome == advanceRefused {
		return refuse("expected '" + l.state.ExpectedAgent + "', got '" + sender + "'")
	}

	encoded, err := encodeContext(transcript)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if _, err := q.Exec(ctx, ensembleStoreSQL, id,
		state.Status, state.CurrentPhase, state.CurrentTurn,
		state.ExpectedAgent, state.ExpectedRole, state.PausedReason, encoded); err != nil {
		return store.StatusFailed, nil, err
	}

	prompt := ""
	if state.Status != "complete" {
		prompt = buildPrompt(l.template, transcript, state.CurrentPhase, state.CurrentTurn)
	}
	return viewReply(ctx, q, id, ensembleOK, "", prompt, transcript)
}

func ensemblePause(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok || id <= 0 {
		return store.StatusInvalid, nil, nil
	}
	reason := f[1]
	if reason == "" {
		reason = "manual"
	}
	tag, err := q.Exec(ctx, ensemblePauseSQL, id, reason)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusOK, []string{"ensemble " + store.I64toa(id) + " not found", "-1"}, nil
	}
	return store.StatusOK, []string{"", "0"}, nil
}

// ensembleList answers with every run. Unlike the reads above there is no
// verdict to relay: every way this fails is the store failing.
func ensembleList(ctx context.Context, q store.Queryer, _ []string) (uint32, []string, error) {
	return collect(ctx, q, ensembleListSQL, ensembleCells, ensembleRow)
}

func ensembleFindCurrentByChannel(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	var id int64
	err := q.QueryRow(ctx, ensembleFindCurrentSQL, f[0]).Scan(&id)
	if store.IsNoRows(err) {
		// The verdict is worth reading: "no ensemble for this channel" is a
		// different thing from a store that could not answer.
		return store.StatusOK, []string{
			store.Itoa(ensembleRefused),
			"no ensemble for channel '" + f[0] + "'",
			"0",
		}, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{
		store.Itoa(ensembleOK), "", store.I64toa(id),
	}, nil
}

// Ensemble is the family, ready to be bound to kind 11780.
var Ensemble = store.Family{
	Name:  "ensemble",
	Event: EventEnsemble,
	Stage: StageEnsemble,
	Ops: map[uint32]store.Op{
		opEnsembleCreate:               {Name: "ensemble_create", Cells: 3, Args: 5, Tx: true, Run: ensembleCreate},
		opEnsembleView:                 {Name: "ensemble_view", Cells: 18, Args: 1, Run: ensembleView},
		opEnsembleAdvance:              {Name: "ensemble_advance", Cells: 18, Args: 3, Tx: true, Run: ensembleAdvance},
		opEnsemblePause:                {Name: "ensemble_pause", Cells: 2, Args: 2, Tx: true, Run: ensemblePause},
		opEnsembleList:                 {Name: "ensemble_list", Cells: 14, Args: 0, Run: ensembleList},
		opEnsembleFindCurrentByChannel: {Name: "ensemble_find_current_by_channel", Cells: 3, Args: 1, Run: ensembleFindCurrentByChannel},
	},
}
