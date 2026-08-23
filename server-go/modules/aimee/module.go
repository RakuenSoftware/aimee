// Package aimee is the core aimee module: the home for functionality specific
// to aimee-server, served as a bus process rather than linked into a caller.
//
// The module is a HOST FOR CAPABILITIES, not a single feature. Session peer
// messaging is the first to land here; server-side symbols currently in db2
// (src/index.c, dashboard_kb.c) arrive during the redistribution, and they are
// index and dashboard reads with nothing to do with peers. A module whose
// constructor, stage table and dispatch were shaped around its first capability
// would need surgery to accept its second, so it is shaped around neither.
//
// See docs/modules/aimee.md and
// docs/proposals/pending/session-peer-messaging.md.
//
// # Why no stage blocks
//
// The bus caps a module at moduleMaxInFlight = 16 concurrent invocations and
// REFUSES the seventeenth outright rather than queueing it — with a generic
// Internal status that does not read as backpressure. A capability whose normal
// use is "wait for another agent to answer" must therefore never hold a handler
// slot while it waits: sixteen outstanding asks would wedge the whole module,
// including capabilities that have nothing to do with peers, and the next
// caller's cheap read would look like a crash.
//
// That constraint is a property of the MODULE, not of peer messaging, which is
// why it is stated here: anything landing in this module inherits it.
package aimee

import (
	"errors"
	"fmt"
	"sort"

	"github.com/JBailes/aimee/server-go/bus"
)

// Capability is one unit of aimee-server functionality this module serves.
//
// A capability owns a disjoint set of stages and is reached only through them.
// Capabilities do not call each other in-process: two that need to interact do
// so over the bus like any other pair of modules, so the governance tap sees
// the exchange.
type Capability interface {
	// Stages are the bus stages this capability serves.
	Stages() []bus.ModuleStage
	// Handle serves one invocation for a stage this capability advertised.
	Handle(bus.ModuleInvocation, []byte) ([]byte, bus.ModuleStatus)
}

// Module dispatches each bus invocation to the capability that advertised its
// stage.
type Module struct {
	byStage map[uint32]Capability
	stages  []bus.ModuleStage
}

// ErrStageConflict is two capabilities advertising one stage.
//
// It is a CONSTRUCTION error rather than a runtime one because the loser would
// be silently unreachable: its stage is advertised, so calls arrive and are
// served by the wrong capability. That is worse than CAPABILITY_ABSENT, which
// at least fails visibly — a stage answered by the wrong owner returns
// plausible nonsense.
var ErrStageConflict = errors.New("aimee: two capabilities advertise one stage")

// New builds a module over its capabilities. A nil capability is ignored so a
// caller can pass one conditionally without branching at the call site.
func New(capabilities ...Capability) (*Module, error) {
	m := &Module{byStage: make(map[uint32]Capability)}
	for _, capability := range capabilities {
		if capability == nil {
			continue
		}
		for _, stage := range capability.Stages() {
			if _, taken := m.byStage[stage.StageID]; taken {
				return nil, fmt.Errorf("%w: stage %d (event %d)",
					ErrStageConflict, stage.StageID, stage.EventKind)
			}
			if want := EventKind(stage.StageID); want != stage.EventKind {
				return nil, fmt.Errorf(
					"aimee: stage %d advertises event %d, but the bus formula gives %d",
					stage.StageID, stage.EventKind, want)
			}
			m.byStage[stage.StageID] = capability
			m.stages = append(m.stages, stage)
		}
	}
	sort.Slice(m.stages, func(i, j int) bool { return m.stages[i].StageID < m.stages[j].StageID })
	return m, nil
}

// Stages is the advertisement table for this module. It must match the stages
// declared for `aimee` in src/modules/process-contracts.json exactly: a stage
// declared there and not advertised here is never available, and every call to
// it returns CAPABILITY_ABSENT while the module runs normally.
func (m *Module) Stages() []bus.ModuleStage { return m.stages }

// Handle routes one invocation to the capability owning its stage.
func (m *Module) Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	// Observe cancellation before dispatch, so a cancelled request for an
	// unknown stage has the same outcome as a cancelled well-formed one.
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	capability, served := m.byStage[invocation.StageID]
	if !served {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return capability.Handle(invocation, request)
}
