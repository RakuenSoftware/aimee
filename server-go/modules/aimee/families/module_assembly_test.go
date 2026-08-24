package families

import (
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// The module the daemon actually builds: the store's families AND peer
// messaging, under one principal, assembled the way cmd/aimee-module assembles
// them.
//
// NOTHING covered this before, and the gap had a shape. The aimee package can
// test New() only with the peer capability, because families imports it and the
// import cannot go back. This package can build the store but had no reason to
// reach for peer. So each half was tested against the contract and the ASSEMBLY
// of the two was tested by production -- where a construction error means the
// module declines to serve, every call answers CAPABILITY_ABSENT, and the rig
// that notices reports it as "the store is unreachable".
//
// Everything here is a pure construction check: NewMux and New validate stage
// numbering and ownership without touching a database, so the stub never has a
// query run against it.
func TestModuleAssemblesStoreAndPeerUnderOnePrincipal(t *testing.T) {
	mux, err := store.NewMux(bindOnlyDB{}, All()...)
	if err != nil {
		t.Fatalf("the store's families do not assemble: %v", err)
	}
	for _, bind := range Binds(bindOnlyDB{}) {
		if err := mux.Add(bind); err != nil {
			t.Fatalf("bind %q does not assemble: %v", bind.Name, err)
		}
	}
	peerCapability, err := store.NewPeer(peer.New(peer.Options{}), store.NoDirectory{})
	if err != nil {
		t.Fatalf("the peer capability does not build: %v", err)
	}

	// The call cmd/aimee-module makes. A stage claimed twice fails HERE, which
	// is the whole point: in the daemon it fails at startup and presents as the
	// store being absent.
	module, err := store.New(peerCapability, mux)
	if err != nil {
		t.Fatalf("the module does not assemble: %v", err)
	}

	advertised := map[uint32]uint32{} // stage -> event
	for _, s := range module.Stages() {
		if prior, taken := advertised[s.StageID]; taken {
			t.Errorf("stage %d advertised twice (events %d and %d)", s.StageID, prior, s.EventKind)
		}
		advertised[s.StageID] = s.EventKind
	}

	// Every store family and bind, and every peer stage, reachable together.
	for _, f := range All() {
		if got, ok := advertised[f.Stage]; !ok || got != f.Event {
			t.Errorf("family %q (stage %d) is not advertised", f.Name, f.Stage)
		}
	}
	for _, b := range Binds(bindOnlyDB{}) {
		if got, ok := advertised[b.Stage]; !ok || got != b.Event {
			t.Errorf("bind %q (stage %d) is not advertised", b.Name, b.Stage)
		}
	}
	for _, stage := range []uint32{
		peerwire.StageDelivery, peerwire.StageInbox,
		peerwire.StageGrant, peerwire.StageChannel,
	} {
		want := peerwire.EventKind(store.PrincipalRef, stage)
		if got, ok := advertised[stage]; !ok || got != want {
			t.Errorf("peer stage %d is not advertised (got event %d, want %d)", stage, got, want)
		}
	}

	// And the count matches the contract, so a stage that assembles but is
	// undeclared cannot pass by being individually correct.
	declared := loadStoreComponent(t).Stages
	if len(advertised) != len(declared) {
		t.Errorf("module advertises %d stages, the contract declares %d",
			len(advertised), len(declared))
	}
	for _, s := range declared {
		if got, ok := advertised[s.ID]; !ok || got != s.EventKind {
			t.Errorf("contract declares %s (stage %d, event %d), module advertises %d",
				s.Name, s.ID, s.EventKind, got)
		}
	}
}
