package main

import "testing"

// Instanced plugin modules: `aimee-module-mcp-NAME` hosts exactly one MCP
// server. Unlike every other module these are matched by prefix, because the
// set is a deployment decision rather than a compile-time list.
//
// An instance is given ONE thing: its principal ref. Its event kinds follow from
// that ref by the canonical rule, 4096 + ref*256 + stage. Refs must come from the
// band reserved for plugin instances (tests/baselines/modules/canonical-inventory.yaml).

// A ref inside the reserved band, used wherever a valid instance is needed.
const testRef = "201"

func TestMCPInstanceGetsItsOwnIdentityAndStages(t *testing.T) {
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", testRef)
	config, ok := moduleConfig("aimee-module-mcp-github")
	if !ok {
		t.Fatal("aimee-module-mcp-github was not recognised")
	}
	// The bus identity must be the INSTANCE, not the kind: ten MCP modules
	// sharing one ModuleName would be ten processes claiming one identity.
	if config.ModuleName != "mcp-github" {
		t.Errorf("ModuleName = %q, want mcp-github", config.ModuleName)
	}
	if config.PrincipalRef != 201 {
		t.Errorf("PrincipalRef = %d, want the provisioned 201", config.PrincipalRef)
	}
	if len(config.Stages) != 2 {
		t.Fatalf("got %d stages, want invoke + declare", len(config.Stages))
	}
	if config.Handler == nil {
		t.Error("no handler bound")
	}
	// The kinds are the ref's, not something separately supplied.
	for _, s := range config.Stages {
		if want := 4096 + 201*256; s.EventKind != uint32(want+1) && s.EventKind != uint32(want+2) {
			t.Errorf("stage kind %d is not derived from ref 201", s.EventKind)
		}
	}
}

func TestMCPInstancesDoNotShareAnIdentity(t *testing.T) {
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", testRef)
	a, okA := moduleConfig("aimee-module-mcp-github")
	b, okB := moduleConfig("aimee-module-mcp-jira")
	if !okA || !okB {
		t.Fatal("instanced modules not recognised")
	}
	if a.ModuleName == b.ModuleName {
		t.Fatalf("two instances share ModuleName %q", a.ModuleName)
	}
}

func TestMCPInstanceRefusesToStartWithoutAProvisionedPrincipal(t *testing.T) {
	// An instanced module cannot allocate its own authorization identity, and
	// defaulting to one would let two plugins share a grant. Fail closed.
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", "")
	if _, ok := moduleConfig("aimee-module-mcp-github"); ok {
		t.Fatal("instance started with no principal ref")
	}
	for _, bad := range []string{"0", "-1", "notanumber", "4294967296"} {
		t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", bad)
		if _, ok := moduleConfig("aimee-module-mcp-github"); ok {
			t.Errorf("instance accepted principal ref %q", bad)
		}
	}
}

func TestMCPInstanceRefusesARefOutsideTheReservedBand(t *testing.T) {
	// Kinds are derived from the ref, so a ref outside the band derives kinds
	// inside some canonical module's 256-kind block. 28, 29 and 30 are postgres,
	// db2 and db1 -- the three the retired 11264 range actually collided with.
	// One kind has exactly one serving slot (bus_route.c:109), so the loser is
	// refused at attach, silently from the operator's side. Fail closed here.
	for _, bad := range []string{"1", "28", "29", "30", "101", "199", "456", "1000"} {
		t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", bad)
		if _, ok := moduleConfig("aimee-module-mcp-github"); ok {
			t.Errorf("instance accepted out-of-band principal ref %q", bad)
		}
	}
}

func TestMCPInstanceRefusesAStaleEventBase(t *testing.T) {
	// AIMEE_MODULE_EVENT_BASE is retired. A deployment provisioned under the old
	// scheme still has it set AND a .grant naming the old kinds -- which live in
	// postgres's, db2's or db1's block. Ignoring it would leave that grant in
	// place, so an instance carrying a base that disagrees with the derivation
	// must refuse and say to re-provision.
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", testRef)
	t.Setenv("AIMEE_MODULE_EVENT_BASE", "11264")
	if _, ok := moduleConfig("aimee-module-mcp-github"); ok {
		t.Error("instance accepted a stale event base from the retired range")
	}

	// A leftover that happens to AGREE with the derivation is harmless.
	t.Setenv("AIMEE_MODULE_EVENT_BASE", "55553") // 4096 + 201*256 + 1
	if _, ok := moduleConfig("aimee-module-mcp-github"); !ok {
		t.Error("instance refused an event base that agrees with the derivation")
	}
}

func TestMCPInstanceRequiresAUsableName(t *testing.T) {
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", testRef)
	if _, ok := moduleConfig("aimee-module-mcp-"); ok {
		t.Error("empty instance name accepted")
	}
	// A name with nothing registry-representable in it cannot own a command
	// group, so it must not start rather than register nameless commands.
	if _, ok := moduleConfig("aimee-module-mcp----"); ok {
		t.Error("instance with no usable command group accepted")
	}
}

func TestMCPInstancesAreProvisionedDistinctEventKinds(t *testing.T) {
	// Distinct REFS are now the whole mechanism: there is no separate base to
	// get wrong, so two instances collide only if they share a ref.
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", "201")
	a, okA := moduleConfig("aimee-module-mcp-github")
	t.Setenv("AIMEE_MODULE_PRINCIPAL_REF", "202")
	b, okB := moduleConfig("aimee-module-mcp-jira")
	if !okA || !okB {
		t.Fatal("instanced modules not recognised")
	}
	for _, sa := range a.Stages {
		for _, sb := range b.Stages {
			if sa.EventKind == sb.EventKind {
				t.Fatalf("instances share event kind %d; the bus would refuse the second",
					sa.EventKind)
			}
		}
	}
}
