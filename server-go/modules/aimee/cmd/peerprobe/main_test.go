package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// The probe's principal must not collide with any ref the process contract
// declares — for a module OR for a client.
//
// This exists because the collision it catches is invisible while it is being
// made. The probe first ran as ref 67, which is reserved for this module's own
// outbound identity (aimee-db1, for reading the session directory out of db1).
// That was harmless for exactly as long as the outbound client did not exist:
// the moment it is wired, the two are a duplicate principal and the bus refuses
// whichever attaches second, with an error naming a ref rather than either
// party. The cause would be a line committed weeks earlier in a different file,
// by someone with no reason to look at a probe.
//
// A comment saying "69, not 67" would not have prevented the next one. Nothing
// else can: the canonical inventory records DECLARED modules, and a probe is
// deliberately undeclared — putting a test fixture in the process contract
// would be worse. So the check has to live here, next to the constant, and it
// has to read the contract rather than transcribe it.
func TestProbeRefDoesNotCollideWithDeclaredPrincipals(t *testing.T) {
	// peerprobe -> cmd -> aimee -> modules -> server-go -> repo root
	path := filepath.Join("..", "..", "..", "..", "..", "src", "modules", "process-contracts.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read process-contracts.json: %v", err)
	}
	var contracts struct {
		Components []struct {
			ID           string `json:"id"`
			PrincipalRef uint32 `json:"principal_ref"`
		} `json:"components"`
		Clients []struct {
			ID           string `json:"id"`
			PrincipalRef uint32 `json:"principal_ref"`
		} `json:"clients"`
	}
	if err := json.Unmarshal(raw, &contracts); err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(contracts.Components) == 0 {
		t.Fatal("no components parsed — the schema moved and this test would pass vacuously")
	}

	taken := map[uint32]string{}
	for _, c := range contracts.Components {
		if c.PrincipalRef != 0 {
			taken[c.PrincipalRef] = "component " + c.ID
		}
	}
	for _, c := range contracts.Clients {
		if c.PrincipalRef != 0 {
			taken[c.PrincipalRef] = "client " + c.ID
		}
	}

	if owner, clash := taken[principalRef]; clash {
		t.Fatalf("probe ref %d is already %s. Pick one the contract does not declare: "+
			"the bus refuses a duplicate principal, and the failure appears only once "+
			"BOTH parties attach, long after this line was written.", principalRef, owner)
	}

	// Not colliding TODAY is the wrong bar, and this test learned it the hard
	// way: the probe sat at 69, the loop above was green because nothing in THIS
	// contract declared 69, and another session allocated 69 to the control-plane
	// module's outbound identity. The collision then appeared on hardware as
	// "attach denied" in whichever process started second -- which is the probe,
	// so a validation run reported a bus problem that was really a bookkeeping
	// one two repositories away.
	//
	// A contract file cannot warn about a ref someone else is about to take, so
	// the invariant is not "unused" but "somewhere the contract will never
	// allocate from". Clients live just above the module refs and are handed out
	// in ascending order, so a validation-only ref must sit far above the
	// high-water mark with room for the range to grow into.
	const probeRefFloor = 128
	if principalRef < probeRefFloor {
		t.Fatalf("probe ref %d is inside the range the contract allocates from. "+
			"Use one at or above %d: a validation ref has to be somewhere nothing "+
			"will EVER be declared, not merely somewhere nothing is declared yet.",
			principalRef, probeRefFloor)
	}
	highest := uint32(0)
	for ref := range taken {
		if ref > highest {
			highest = ref
		}
	}
	if highest >= probeRefFloor {
		t.Fatalf("the contract now declares ref %d, at or above the probe floor of %d. "+
			"The floor has to move up and this probe with it, or the next client "+
			"allocated lands on top of it.", highest, probeRefFloor)
	}
}
