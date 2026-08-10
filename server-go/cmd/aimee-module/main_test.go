package main

import (
	"context"
	"errors"
	"testing"
)

func TestModuleRegistryMatchesProcessContracts(t *testing.T) {
	tests := []struct {
		name      string
		principal uint32
		events    []uint32
	}{
		{"memory", 7, []uint32{5889, 5890, 5891, 5892, 5893}},
		{"learning", 8, []uint32{6145}},
		{"routing", 9, []uint32{6401}},
		{"delegates", 10, []uint32{6657, 6658, 6659, 6660, 6661, 6662}},
		{"tools", 11, []uint32{6913}},
		{"workspace", 12, []uint32{7169, 7170, 7171}},
		{"git", 13, []uint32{7425, 7426, 7427, 7430}},
		{"skills", 14, []uint32{7681, 7682}},
		{"response-composition", 15, []uint32{7937}},
		{"governance", 19, []uint32{8961}},
		{"roundtable", 21, []uint32{9473, 9475}},
		{"kb-synthesis", 22, []uint32{9729}},
		{"runtime-web", 23, []uint32{9985}},
		{"control-web", 24, []uint32{10241}},
		{"benchmarks", 25, []uint32{10497, 10498}},
		{"sandbox", 26, []uint32{10753, 10754}},
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
