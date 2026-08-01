package main

import (
	"testing"
)

func TestWebchatVaultExportParserRejectsUnknownAndDuplicateRecords(t *testing.T) {
	if _, err := parseWebchatVaultExport("unknown\teA==\n"); err == nil {
		t.Fatal("unknown record accepted")
	}
	if _, err := parseWebchatVaultExport("user\tb3A=\nuser\tb3A=\n"); err == nil {
		t.Fatal("duplicate record accepted")
	}
}
