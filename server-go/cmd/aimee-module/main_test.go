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
		{"git", 13, []uint32{7425}},
		{"skills", 14, []uint32{7681}},
		{"response-composition", 15, []uint32{7937}},
		{"roundtable", 21, []uint32{9473}},
		{"benchmarks", 25, []uint32{10497}},
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
	if _, ok := moduleConfig("aimee-module-governance"); ok {
		t.Fatal("C module appeared in Go registry")
	}
	if err := run(context.Background(), []string{"aimee-module-routing"}); !errors.Is(err, errUsage) {
		t.Fatalf("bad arguments error = %v", err)
	}
	if err := run(context.Background(), []string{"aimee-module-unknown", "/unused"}); err == nil {
		t.Fatal("unknown module executable was accepted")
	}
}
