package aimee

import (
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
)

// PrincipalRef is this module's principal in the kind formula
//
//	kind = 4096 + principal_ref*256 + stage
//
// so every family's event kind and stage in this module derive from it. It is
// stated once here and CHECKED against each family below, because a family
// whose kind and stage disagree with the formula binds to a kind the bus routes
// somewhere else -- and nothing about the family itself would look wrong.
const PrincipalRef = 30

// ModuleID is this module's id in src/modules/process-contracts.json and in
// src/modules/<id>/module.yaml, and the name a deployment installs it under:
// the executable is aimee-module-<ModuleID> and the grant pins that path.
//
// It sits beside PrincipalRef because the two are one identity split across two
// files, and a rename that moves only one of them produces a module the bus
// routes to and the registry has never heard of. Stating it here gives anything
// that needs to find this module's own entry a name to look up rather than a
// literal to transcribe.
const ModuleID = "aimee"

// kindFor is the kind a stage must be served on.
func kindFor(stage uint32) uint32 { return 4096 + PrincipalRef*256 + stage }

// Mux routes one invocation to the family that owns its stage.
//
// The bus hands a module one handler and a list of stages; each family here
// answers exactly one stage, so this is what turns nineteen families into the
// single handler the host asks for. A stage nobody claims is refused rather
// than silently answered by whichever family happened to be first.
type Mux struct {
	byStage map[uint32]bus.ModuleHandler
	stages  []bus.ModuleStage
}

// Bind is a stage served by something that is not a fields-v2 Family.
//
// Two operations in this module speak the keyed-blob wire rather than
// fields-v2, so they cannot go through Family's dispatch -- they carry their
// own handler. This is the seam for those, and it is deliberately narrow: it
// takes a kind and a stage and checks them against the same formula, so a raw
// handler cannot bind anywhere a Family could not.
type Bind struct {
	Name    string
	Event   uint32
	Stage   uint32
	Handler bus.ModuleHandler
}

// NewMux binds each family to a database and checks it against the formula.
//
// It returns an error rather than panicking: a mis-declared family is a
// programming mistake, but it is one the host can report cleanly at startup
// instead of dying with a stack trace.
func NewMux(db DB, families ...Family) (*Mux, error) {
	if db == nil {
		return nil, fmt.Errorf("aimee: no database")
	}
	m := &Mux{byStage: map[uint32]bus.ModuleHandler{}}
	for _, f := range families {
		if want := kindFor(f.Stage); f.Event != want {
			return nil, fmt.Errorf(
				"aimee: family %q serves kind %d on stage %d, but the kind formula "+
					"says stage %d is kind %d",
				f.Name, f.Event, f.Stage, f.Stage, want)
		}
		if existing, taken := m.byStage[f.Stage]; taken && existing != nil {
			return nil, fmt.Errorf("aimee: two families claim stage %d", f.Stage)
		}
		if len(f.Ops) == 0 {
			return nil, fmt.Errorf("aimee: family %q serves no operations", f.Name)
		}
		m.byStage[f.Stage] = f.Handler(db)
		m.stages = append(m.stages, bus.ModuleStage{EventKind: f.Event, StageID: f.Stage})
	}
	return m, nil
}

// Add binds a stage served by a handler of its own. See Bind.
func (m *Mux) Add(b Bind) error {
	if b.Handler == nil {
		return fmt.Errorf("aimee: %q has no handler", b.Name)
	}
	if want := kindFor(b.Stage); b.Event != want {
		return fmt.Errorf(
			"aimee: %q serves kind %d on stage %d, but the kind formula says "+
				"stage %d is kind %d", b.Name, b.Event, b.Stage, b.Stage, want)
	}
	if _, taken := m.byStage[b.Stage]; taken {
		return fmt.Errorf("aimee: stage %d is already claimed", b.Stage)
	}
	m.byStage[b.Stage] = b.Handler
	m.stages = append(m.stages, bus.ModuleStage{EventKind: b.Event, StageID: b.Stage})
	return nil
}

// Stages is the list the host registers with the bus.
func (m *Mux) Stages() []bus.ModuleStage { return m.stages }

// Handle routes one invocation.
func (m *Mux) Handle(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
	handler, ok := m.byStage[invocation.StageID]
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return handler(invocation, frame)
}
