package families

import store "github.com/JBailes/aimee/server-go/modules/aimee"

// All is every family this module serves.
//
// The store is ONE PostgreSQL module. These are its families -- the event kinds
// it answers on -- not separate stores, and there is no second database behind
// any of them. A different backing store would be a different module, declaring
// its own kinds.
//
// Adding a family here is what makes it reachable; writing one is not enough.
// The mux checks each against the kind formula when it binds them, so a family
// that lands here with the wrong kind fails at startup rather than binding to
// something else's traffic.
//
// Two things this module serves are NOT in here. economizer_state speaks the
// keyed-blob wire rather than fields-v2, so it carries its own handler and
// binds through Mux.Add -- see Binds below. The health probe is a different
// principal entirely and keeps its own pool on purpose, so that it can still
// answer when the shared one is broken.
func All() []store.Family {
	return []store.Family{
		GitOwnership,
		Conversation,
		Ensemble,
		Delegation,
		Sessions,
		Telemetry,
		Workflow,
		GuardrailState,
		AgentWork,
		Runtime,
		Roundtable,
		Identity,
		Checkpoints,
		JTIReplay,
		Lifecycle,
		MgmtJWKS,
		MgmtNonce,
		PKI,
	}
}

// Binds are the stages this module serves with handlers of their own rather
// than through Family dispatch.
func Binds(db store.DB) []store.Bind {
	return []store.Bind{
		{
			Name:  "economizer_state",
			Event: EventState,
			Stage: StageState,
			// The keyed-blob wire: two operations, their own codec. Given the
			// store rather than fetching one, so this module holds no database
			// of its own anywhere.
			Handler: newHandler(pgStore{db}),
		},
	}
}
