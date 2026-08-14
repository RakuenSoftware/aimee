package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestModuleRegistryMatchesProcessContracts(t *testing.T) {
	tests := []struct {
		name      string
		principal uint32
		events    []uint32
	}{
		{"memory", 7, []uint32{5889, 5890, 5891, 5892, 5893, 5894}},
		{"learning", 8, []uint32{6145}},
		{"routing", 9, []uint32{6401}},
		{"delegates", 10, []uint32{6657, 6658, 6659, 6660, 6661, 6662, 6663, 6664, 6665, 6666, 6667, 6668, 6669, 6670, 6671, 6672, 6673, 6674, 6675, 6676, 6677, 6678}},
		{"tools", 11, []uint32{6913}},
		{"workspace", 12, []uint32{7169, 7170, 7171}},
		{"git", 13, []uint32{7425, 7426, 7427, 7428, 7429, 7430}},
		{"skills", 14, []uint32{7681, 7682}},
		{"response-composition", 15, []uint32{7937}},
		{"governance", 19, []uint32{8961}},
		{"roundtable", 21, []uint32{9473, 9475}},
		{"kb-synthesis", 22, []uint32{9729}},
		{"runtime-web", 23, []uint32{9985}},
		{"control-web", 24, []uint32{10241}},
		{"benchmarks", 25, []uint32{10497, 10498}},
		{"sandbox", 26, []uint32{10753, 10754, 10755, 10756}},
		{"economizer", 27, []uint32{11009, 11010, 11011, 11012, 11013}},
	}
	for _, test := range tests {
		config, ok := moduleConfig("/usr/local/libexec/aimee-modules/aimee-module-" + test.name)
		if !ok || config.ModuleName != test.name || config.PrincipalClass != 1 ||
			config.PrincipalRef != test.principal || len(config.Stages) != len(test.events) ||
			config.Handler == nil {
			t.Fatalf("%s config = %#v, ok=%v", test.name, config, ok)
		}
		// The stage id is derived from the event kind rather than the position:
		// a module may declare a non-contiguous set when one of its stages is
		// registered conditionally. roundtable serves 1 and 3 here because review
		// (stage 2) is only declared when this process can convene one.
		for index, event := range test.events {
			wantStage := event - (4096 + test.principal*256)
			if config.Stages[index].EventKind != event || config.Stages[index].StageID != wantStage {
				t.Fatalf("%s stage %d = %#v, want event %d / stage %d", test.name, index,
					config.Stages[index], event, wantStage)
			}
		}
	}
}

// THE TABLE ABOVE IS HAND-WRITTEN, so on its own it proves only that the code
// matches a second copy of itself. It did exactly that: git shipped for weeks
// advertising 7425,7426,7427,7430 while process-contracts.json declared
// git-forge-request (7428) and git-credential-resolve (7429) as well, and this
// test — despite its name — agreed with the code because someone updated the
// list to match rather than the contract.
//
// Nothing noticed until callers moved onto those stages: the module HANDLES
// both (Handle routes them) and the grant PERMITS both (serve=7425..7430), but a
// stage that is never advertised is never available, so every call returned
// CAPABILITY_ABSENT in ~40ms. That reads as "the module is down" while the
// module is plainly running, and it silently disabled git credential injection
// (pushes asked for a username) and every forge operation.
//
// So compare against the CONTRACT FILE itself. A stage added to the contract and
// not advertised — or advertised and not declared — now fails here.
func TestAdvertisedStagesMatchTheContractFile(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "modules", "process-contracts.json"))
	if err != nil {
		t.Fatalf("read process-contracts.json: %v", err)
	}
	var contracts struct {
		Components []struct {
			ID        string `json:"id"`
			Execution string `json:"execution"`
			Stages    []struct {
				EventKind uint32 `json:"event_kind"`
				Name      string `json:"name"`
			} `json:"stages"`
		} `json:"components"`
	}
	if err := json.Unmarshal(raw, &contracts); err != nil {
		t.Fatalf("parse process-contracts.json: %v", err)
	}
	if len(contracts.Components) == 0 {
		t.Fatal("no components parsed — the schema moved and this test would pass vacuously")
	}

	checked := 0
	for _, declared := range contracts.Components {
		if declared.Execution != "process" || len(declared.Stages) == 0 {
			continue // not a module process; nothing to advertise
		}
		config, ok := moduleConfig("/usr/local/libexec/aimee-modules/aimee-module-" + declared.ID)
		if !ok {
			continue // not built into this binary (e.g. a C-hosted component)
		}
		checked++
		advertised := map[uint32]bool{}
		for _, s := range config.Stages {
			advertised[s.EventKind] = true
		}
		for _, s := range declared.Stages {
			// roundtable-review is registered ONLY when this process can convene a
			// panel (roundtableReviewer() succeeds), which it cannot under test.
			// That conditionality is deliberate and documented above; every other
			// declared stage is unconditional and must be advertised.
			if s.EventKind == 9474 {
				continue
			}
			if !advertised[s.EventKind] {
				t.Errorf("%s declares %s (event %d) in process-contracts.json but never "+
					"advertises it: calls fail CAPABILITY_ABSENT though the module is running",
					declared.ID, s.Name, s.EventKind)
			}
		}
	}
	// Guard the guard: if nothing was compared, this test proves nothing.
	if checked == 0 {
		t.Fatal("no module was actually compared against the contract")
	}
}

func TestModuleRegistryRejectsUnknownAndBadArguments(t *testing.T) {
	if _, ok := moduleConfig("aimee-module-unknown"); ok {
		t.Fatal("unknown module appeared in Go registry")
	}
	if err := run(context.Background(), []string{"aimee-module-routing"}); !errors.Is(err, errUsage) {
		t.Fatalf("bad arguments error = %v", err)
	}
	if err := run(context.Background(), []string{"aimee-module-unknown", "/unused"}); err == nil {
		t.Fatal("unknown module executable was accepted")
	}
}
