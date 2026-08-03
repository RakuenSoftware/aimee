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
		kind      uint32
	}{{"learning", 8, 6145}, {"routing", 9, 6401}, {"tools", 11, 6913}, {"skills", 14, 7681}}
	for _, test := range tests {
		config, ok := moduleConfig("/usr/local/libexec/aimee-modules/aimee-module-" + test.name)
		if !ok || config.ModuleName != test.name || config.PrincipalClass != 1 ||
			config.PrincipalRef != test.principal || len(config.Stages) != 1 ||
			config.Stages[0].EventKind != test.kind || config.Stages[0].StageID != 1 ||
			config.Handler == nil {
			t.Fatalf("%s config = %#v, ok=%v", test.name, config, ok)
		}
	}
}

func TestModuleRegistryRejectsUnknownAndBadArguments(t *testing.T) {
	if _, ok := moduleConfig("aimee-module-memory"); ok {
		t.Fatal("C module appeared in Go registry")
	}
	if err := run(context.Background(), []string{"aimee-module-routing"}); !errors.Is(err, errUsage) {
		t.Fatalf("bad arguments error = %v", err)
	}
	if err := run(context.Background(), []string{"aimee-module-unknown", "/unused"}); err == nil {
		t.Fatal("unknown module executable was accepted")
	}
}
