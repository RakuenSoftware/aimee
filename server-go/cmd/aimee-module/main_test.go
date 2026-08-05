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
		{"delegates", 10, []uint32{6657}},
		{"tools", 11, []uint32{6913}},
		{"workspace", 12, []uint32{7169}},
		{"git", 13, []uint32{7425, 7426}},
		{"skills", 14, []uint32{7681, 7682}},
		{"response-composition", 15, []uint32{7937}},
		{"governance", 19, []uint32{8961}},
		{"roundtable", 21, []uint32{9473}},
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
		for index, event := range test.events {
			if config.Stages[index].EventKind != event || config.Stages[index].StageID != uint32(index+1) {
				t.Fatalf("%s stage %d = %#v", test.name, index+1, config.Stages[index])
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
