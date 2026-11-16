package families

import (
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"path/filepath"
	"regexp"
	"strings"
)

// The ensemble state machine: templates, phases, turns and prompts.
//
// None of this touches a database. It is the ensemble's behaviour rather than
// its storage, and it lives apart from the operations for the same reason the
// clarify scoring does -- so the SQL beside it stays about rows, and so this
// can be tested without one.

// --- the template ----------------------------------------------------------

type participant struct {
	Agent string `json:"agent,omitempty"`
	Role  string `json:"role"`
}

type phase struct {
	Name         string        `json:"name"`
	Participants []participant `json:"participants"`
}

type template struct {
	Name        string  `json:"name"`
	Description string  `json:"description,omitempty"`
	Phases      []phase `json:"phases"`
}

// participantAt is the participant whose turn it is, or nil past the end. Every
// walk goes through this rather than indexing, because a phase or turn read
// from a stored row can point past a template that has since been edited.
func (t template) participantAt(phaseIdx, turnIdx int) *participant {
	if phaseIdx < 0 || phaseIdx >= len(t.Phases) {
		return nil
	}
	p := t.Phases[phaseIdx].Participants
	if turnIdx < 0 || turnIdx >= len(p) {
		return nil
	}
	return &p[turnIdx]
}

func (t template) turnsInPhase(phaseIdx int) int {
	if phaseIdx < 0 || phaseIdx >= len(t.Phases) {
		return 0
	}
	return len(t.Phases[phaseIdx].Participants)
}

func (t template) phaseName(phaseIdx int) string {
	if phaseIdx < 0 || phaseIdx >= len(t.Phases) {
		return ""
	}
	return t.Phases[phaseIdx].Name
}

// isAssignedAgent reports whether this name holds any turn in the template. It
// is what separates "the wrong agent spoke out of order" from "a person said
// something in the channel".
func (t template) isAssignedAgent(name string) bool {
	if name == "" {
		return false
	}
	for _, ph := range t.Phases {
		for _, p := range ph.Participants {
			if p.Agent == name {
				return true
			}
		}
	}
	return false
}

// --- template resolution ---------------------------------------------------

// templateNamePattern is what a template name may be.
//
// The C interpolated this name straight into "%s/%s/%s.json" with nothing but
// a non-empty check, so a name containing ../ escaped the templates directory
// entirely: the store would read any .json file the daemon could reach, store
// its contents in template_json, and hand them back on the next view. That is
// an arbitrary file read reachable by anyone who can ask to start an ensemble.
//
// A template name is a bare filename. Anything that is not one is refused
// rather than cleaned up, because a name that needed cleaning was not a name.
var templateNamePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`)

// validTemplateName also rejects any name containing "..", which the pattern
// admits as an ordinary run of dots. Both checks are here rather than folded
// into one unreadable expression.
func validTemplateName(name string) bool {
	return templateNamePattern.MatchString(name) && !strings.Contains(name, "..")
}

// templateDirs are searched in order. session_templates is the pre-rename name,
// kept so project-local templates authored before it keep resolving.
var templateDirs = []string{"ensemble_templates", "session_templates"}

// templateCandidates is every path a named template could live at, in search
// order. The name is validated before this is called, so the join cannot
// escape the roots.
func templateCandidates(projectRoot, configDir, name string) []string {
	var out []string
	for _, dir := range templateDirs {
		for _, root := range []string{projectRoot, configDir} {
			if root != "" {
				out = append(out, filepath.Join(root, dir, name+".json"))
			}
		}
	}
	return out
}

// builtinTemplates are the templates that need no file. They are the fallback
// when nothing resolves on disk.
var builtinTemplates = map[string]string{
	"code-review": `{"name":"code-review",
	  "description":"Structured code review with independent reviewers",
	  "phases":[
	    {"name":"initial-review","participants":[{"role":"reviewer"},{"role":"reviewer"}]},
	    {"name":"rebuttal","participants":[{"role":"author"}]},
	    {"name":"final-verdict","participants":[{"role":"reviewer"},{"role":"reviewer"}]}]}`,

	"debate": `{"name":"debate",
	  "description":"Adversarial analysis with alternating turns",
	  "phases":[
	    {"name":"opening","participants":[{"role":"for"},{"role":"against"}]},
	    {"name":"cross-examination","participants":[{"role":"for"},{"role":"against"}]},
	    {"name":"closing","participants":[{"role":"for"},{"role":"against"}]}]}`,

	"planning": `{"name":"planning",
	  "description":"Collaborative planning with critique and synthesis",
	  "phases":[
	    {"name":"brainstorm","participants":[{"role":"planner"}]},
	    {"name":"critique","participants":[{"role":"critic"}]},
	    {"name":"synthesis","participants":[{"role":"planner"}]}]}`,

	"design-critique": `{"name":"design-critique",
	  "description":"Design review with presentation, feedback, and revision",
	  "phases":[
	    {"name":"presentation","participants":[{"role":"author"}]},
	    {"name":"feedback","participants":[{"role":"critic"}]},
	    {"name":"revision","participants":[{"role":"author"}]}]}`,
}

// --- assignments -----------------------------------------------------------

// expandAssignments binds a concrete agent to every turn.
//
// Roles are filled round-robin from the caller's list for that role, so a role
// held twice in one phase goes to two different agents where the caller
// supplied two. A role with no assignment is an error naming the role, because
// "missing an assignment" is something the caller can act on and "could not
// start" is not.
func expandAssignments(t *template, assignments map[string][]string) error {
	slot := map[string]int{}
	for i := range t.Phases {
		for j := range t.Phases[i].Participants {
			p := &t.Phases[i].Participants[j]
			if p.Role == "" {
				return fmt.Errorf("template participant is missing role")
			}
			agents := assignments[p.Role]
			if len(agents) == 0 {
				return fmt.Errorf("missing --assign for role '%s'", p.Role)
			}
			agent := agents[slot[p.Role]%len(agents)]
			if agent == "" {
				return fmt.Errorf("invalid assignment for role '%s'", p.Role)
			}
			p.Agent = agent
			slot[p.Role]++
		}
	}
	return nil
}

// --- context ---------------------------------------------------------------

type contextMessage struct {
	Sender  string            `json:"sender"`
	Message canonical.Message `json:"message"`
	Phase   int               `json:"phase"`
	Turn    int               `json:"turn"`
}

// contextExcerptTurns is how much of the conversation a prompt carries.
const contextExcerptTurns = 4

// dissentRoles are the roles told to disagree. A reviewer that echoes the
// previous reviewer has not reviewed anything.
var dissentRoles = map[string]bool{
	"reviewer": true, "red_team": true, "critic": true,
	"challenger": true, "against": true,
}

const dissentInstruction = "\nProvide your own independent analysis. Do not repeat or defer to " +
	"previous reviewers. If you agree on a point, acknowledge it briefly and " +
	"focus on what others missed or where you disagree.\n"

// buildRequest retains each admitted contribution as canonical blocks. Only the
// explicit MCP response boundary renders this next-turn preview as text.
func buildRequest(t template, ctx []contextMessage, phaseIdx, turnIdx int) *canonical.Request {
	name := t.phaseName(phaseIdx)
	agent, role := "?", "?"
	if p := t.participantAt(phaseIdx, turnIdx); p != nil {
		agent, role = p.Agent, p.Role
	}
	instruction := fmt.Sprintf("Phase: %s\nAgent: %s\nRole: %s\n", name, agent, role)
	if dissentRoles[role] {
		instruction += dissentInstruction
	}
	request := canonical.TaskRequest(role, "Provide the next contribution for this phase.")
	request.System = append(request.System, canonical.Block{Type: "text", Text: &instruction,
		Context: &canonical.Context{Origin: 1, Authority: 3, Trust: 4, Sensitivity: 1, ModelVisible: true}})
	start := len(ctx) - contextExcerptTurns
	if start < 0 {
		start = 0
	}
	request.Messages = nil
	for _, contribution := range ctx[start:] {
		raw, _ := json.Marshal(contribution.Message)
		var message canonical.Message
		if !canonical.Decode(raw, &message) {
			continue
		}
		label := "Speaker: " + contribution.Sender
		message.ContentScalar = false
		message.Blocks = append([]canonical.Block{{Type: "text", Text: &label,
			Context: &canonical.Context{Origin: 3, Authority: 1, Trust: 1, Sensitivity: 1, ModelVisible: true}}}, message.Blocks...)
		request.Messages = append(request.Messages, message)
	}
	next := "Provide the next contribution for this phase."
	request.Messages = append(request.Messages, canonical.Message{Role: "user", Blocks: []canonical.Block{{Type: "text", Text: &next,
		Context: &canonical.Context{Origin: 1, Authority: 2, Trust: 4, Sensitivity: 1, ModelVisible: true}}}})
	return request
}

// --- the turn ---------------------------------------------------------------

// ensembleState is the part of a row the state machine reasons about.
type ensembleState struct {
	Status        string
	CurrentPhase  int
	CurrentTurn   int
	ExpectedAgent string
	ExpectedRole  string
	PausedReason  string
}

// resolveExpectation fills in what the template says about where the run
// currently stands. A complete run expects nobody.
func (s *ensembleState) resolveExpectation(t template) {
	if s.Status == "complete" {
		s.ExpectedAgent, s.ExpectedRole = "", ""
		return
	}
	if p := t.participantAt(s.CurrentPhase, s.CurrentTurn); p != nil {
		s.ExpectedAgent, s.ExpectedRole = p.Agent, p.Role
		return
	}
	s.ExpectedAgent, s.ExpectedRole = "", ""
}

// advanceOutcome is what one turn did.
type advanceOutcome int

const (
	// advanceTook means the expected agent spoke and the run moved on.
	advanceTook advanceOutcome = iota
	// advanceInterrupted means someone outside the cast spoke, which pauses
	// the run rather than rejecting the message: a person saying something in
	// the channel is not an error, it is a reason to stop and let them.
	advanceInterrupted
	// advanceRefused means an agent that IS in the cast spoke out of turn.
	// That is a caller mistake, and the run does not move.
	advanceRefused
)

// advance applies one message to a run.
//
// It returns the new state, the context to store, and what happened. It does
// not decide whether to write: that belongs to the operation, which holds the
// row lock that makes the read-modify-write safe.
func advance(t template, s ensembleState, ctx []contextMessage, sender string, message canonical.Message) (
	ensembleState, []contextMessage, advanceOutcome) {

	if sender != s.ExpectedAgent {
		if t.isAssignedAgent(sender) {
			return s, ctx, advanceRefused
		}
		ctx = append(ctx, contextMessage{
			Sender: sender, Message: message,
			Phase: s.CurrentPhase, Turn: s.CurrentTurn,
		})
		s.Status = "paused"
		s.PausedReason = "human_interruption"
		s.resolveExpectation(t)
		return s, ctx, advanceInterrupted
	}

	ctx = append(ctx, contextMessage{
		Sender: sender, Message: message,
		Phase: s.CurrentPhase, Turn: s.CurrentTurn,
	})

	turns := t.turnsInPhase(s.CurrentPhase)
	nextPhase, nextTurn := s.CurrentPhase, s.CurrentTurn+1
	if nextTurn >= turns {
		nextPhase, nextTurn = nextPhase+1, 0
	}

	if nextPhase >= len(t.Phases) {
		// The run is over. It stays parked on the last turn of the last phase
		// rather than on a phase that does not exist, so a reader sees where it
		// ended rather than one past it.
		s.Status = "complete"
		s.CurrentPhase = nextPhase - 1
		s.CurrentTurn = turns
		s.PausedReason = ""
	} else {
		s.Status = "active"
		s.CurrentPhase, s.CurrentTurn = nextPhase, nextTurn
		s.PausedReason = ""
	}
	s.resolveExpectation(t)
	return s, ctx, advanceTook
}

// --- json helpers -----------------------------------------------------------

// parseTemplate reads a stored or loaded template.
func parseTemplate(raw string) (template, error) {
	var t template
	if err := json.Unmarshal([]byte(raw), &t); err != nil {
		return template{}, fmt.Errorf("template is not valid JSON: %w", err)
	}
	return t, nil
}

const ensembleContextSchema = "aimee.ensemble-context.v1"

type ensembleContext struct {
	Schema   string           `json:"schema"`
	Messages []contextMessage `json:"messages"`
}

// The old SQL array schema is an explicit historical text import. New writes
// always use the marked envelope, even when empty; message content never selects
// a codec or gets reinterpreted as JSON.
func parseContext(raw string) ([]contextMessage, error) {
	if strings.TrimSpace(raw) == "" || strings.TrimSpace(raw) == "null" {
		return nil, nil
	}
	var result ensembleContext
	if strings.HasPrefix(strings.TrimSpace(raw), "[") {
		var legacy []struct {
			Sender string `json:"sender"`
			Text   string `json:"text"`
			Phase  int    `json:"phase"`
			Turn   int    `json:"turn"`
		}
		if !canonical.Decode([]byte(raw), &legacy) {
			return nil, fmt.Errorf("invalid legacy ensemble context")
		}
		result.Messages = make([]contextMessage, 0, len(legacy))
		for _, item := range legacy {
			text := item.Text
			message := canonical.Message{Role: "user", ContentScalar: true, Blocks: []canonical.Block{{Type: "text", Text: &text,
				Context: &canonical.Context{Origin: 3, Authority: 1, Trust: 1, Sensitivity: 1, ModelVisible: true}}}}
			result.Messages = append(result.Messages, contextMessage{Sender: item.Sender, Message: message, Phase: item.Phase, Turn: item.Turn})
		}
	} else if !canonical.Decode([]byte(raw), &result) || result.Schema != ensembleContextSchema || result.Messages == nil {
		return nil, fmt.Errorf("invalid canonical ensemble context")
	}
	for _, item := range result.Messages {
		encoded, _ := json.Marshal(item.Message)
		if !canonical.ValidMessage(encoded) {
			return nil, fmt.Errorf("invalid ensemble message")
		}
	}
	return result.Messages, nil
}

func encodeContext(ctx []contextMessage) (string, error) {
	if ctx == nil {
		ctx = []contextMessage{}
	}
	raw, err := json.Marshal(ensembleContext{Schema: ensembleContextSchema, Messages: ctx})
	if err != nil {
		return "", err
	}
	if _, err = parseContext(string(raw)); err != nil {
		return "", err
	}
	return string(raw), nil
}

// parseAssignments reads the caller's role-to-agents map.
func parseAssignments(raw string) (map[string][]string, error) {
	if strings.TrimSpace(raw) == "" {
		return map[string][]string{}, nil
	}
	var out map[string][]string
	if err := json.Unmarshal([]byte(raw), &out); err != nil {
		return nil, fmt.Errorf("assignments are not valid JSON: %w", err)
	}
	return out, nil
}
