package families

import (
	"context"
	"errors"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Writing a family is not the same as serving one.
//
// Every family in this package is reachable only if it is in All(); a family
// that exists, passes its own tests, and is not in that list answers nothing at
// all, and no test of the family itself would notice. This is the check that
// the two agree.
func TestEveryFamilyInThisPackageIsServed(t *testing.T) {
	// Every Family value the package defines. Adding one here is the same edit
	// as adding it to All(), which is the point: the two lists are compared,
	// and a family that appears in neither is one nobody wrote.
	defined := map[string]store.Family{
		"git_ownership":   GitOwnership,
		"conversation":    Conversation,
		"ensemble":        Ensemble,
		"delegation":      Delegation,
		"sessions":        Sessions,
		"telemetry":       Telemetry,
		"workflow":        Workflow,
		"guardrail_state": GuardrailState,
		"agent_work":      AgentWork,
		"runtime":         Runtime,
		"roundtable":      Roundtable,
		"identity":        Identity,
		"checkpoints":     Checkpoints,
		"jti_replay":      JTIReplay,
		"lifecycle":       Lifecycle,
		"mgmt_jwks":       MgmtJWKS,
		"mgmt_nonce":      MgmtNonce,
		"pki":             PKI,
	}

	served := map[string]bool{}
	for _, f := range All() {
		served[f.Name] = true
	}

	for name := range defined {
		if !served[name] {
			t.Errorf("family %q is defined but not in All(), so it answers nothing", name)
		}
	}
	for name := range served {
		if _, ok := defined[name]; !ok {
			t.Errorf("All() serves %q, which this test does not know about", name)
		}
	}
}

// The whole module must bind. This is the check that catches a family whose
// kind and stage disagree with the formula, or two families claiming one stage
// -- both of which route traffic somewhere nobody intended and neither of which
// is visible in the family's own file.
func TestTheWholeModuleBinds(t *testing.T) {
	mux, err := store.NewMux(bindOnlyDB{}, All()...)
	if err != nil {
		t.Fatalf("the families do not bind: %v", err)
	}
	for _, b := range Binds(bindOnlyDB{}) {
		if err := mux.Add(b); err != nil {
			t.Fatalf("%s does not bind: %v", b.Name, err)
		}
	}

	stages := mux.Stages()
	if want := len(All()) + len(Binds(bindOnlyDB{})); len(stages) != want {
		t.Fatalf("the module registered %d stages, want %d", len(stages), want)
	}

	// Every registered kind must match the formula, which is what the bus uses
	// to route. A kind that is off by one lands on another module's traffic.
	seen := map[uint32]bool{}
	for _, s := range stages {
		want := uint32(4096 + store.PrincipalRef*256 + int(s.StageID))
		if s.EventKind != want {
			t.Errorf("stage %d registered kind %d, the formula says %d",
				s.StageID, s.EventKind, want)
		}
		if seen[s.EventKind] {
			t.Errorf("kind %d is registered twice", s.EventKind)
		}
		seen[s.EventKind] = true
	}
}

// bindOnlyDB satisfies the DB interface so the module can be bound. Nothing in
// these tests executes a query: what is under test is the wiring.
type bindOnlyDB struct{}

func (bindOnlyDB) Exec(context.Context, string, ...any) (store.Tag, error) {
	return store.RowsAffected(0), errNotWired
}
func (bindOnlyDB) Query(context.Context, string, ...any) (store.Rows, error) {
	return nil, errNotWired
}
func (bindOnlyDB) QueryRow(context.Context, string, ...any) store.Row { return nil }
func (bindOnlyDB) Begin(context.Context) (store.Tx, error)            { return nil, errNotWired }

var errNotWired = errors.New("bindOnlyDB does not run queries")
