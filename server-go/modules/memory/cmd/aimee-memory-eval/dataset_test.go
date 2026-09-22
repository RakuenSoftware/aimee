package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const locomoSample = `{"conversation":{"session_1_date_time":"2026-01-02","session_1":[{"dia_id":"D1:1","speaker":"A","text":"nebula location is Orion"}]},"qa":[{"question_id":"skip","question":"unknown","evidence":[]},{"question_id":"kept","question":"Where is the nebula?","evidence":["D1:1"]}]}`
const longmemSample = `{"question_id":"question-1","question":"Where is the nebula?","haystack_session_ids":["session-1"],"haystack_dates":["2026-01-02"],"haystack_sessions":[[{"role":"user","content":"nebula location is Orion"}]],"answer_session_ids":["session-1"]}`

func datasetFile(t *testing.T, raw string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "dataset.json")
	if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
		t.Fatal(err)
	}
	return path
}
func TestDatasetParsingAndDenominators(t *testing.T) {
	for _, suite := range []string{"locomo", "longmemeval"} {
		sample := locomoSample
		if suite == "longmemeval" {
			sample = longmemSample
		}
		path := datasetFile(t, "["+sample+","+sample+"]")
		plan, err := readDataset(path, suite, 0)
		if err != nil || len(plan.groups) != 2 || len(plan.groups[0].Cases) != 1 {
			t.Fatal(plan, err)
		}
		if plan.groups[0].Cases[0].Query != "Where is the nebula?" {
			t.Fatal("query was normalized or truncated", plan)
		}
		if suite == "locomo" && (plan.excluded["no_evidence"] != 2 || plan.groups[0].Cases[0].ID != "kept") {
			t.Fatal("question identity/denominator drift", plan)
		}
		plan, err = readDataset(path, suite, 1)
		if err != nil || len(plan.groups) != 1 {
			t.Fatal(plan, err)
		}
	}
	// Full source text and source IDs survive, including a common long prefix.
	full := strings.Repeat("é文", 1200)
	sample := strings.ReplaceAll(locomoSample, "nebula location is Orion", full)
	plan, err := readDataset(datasetFile(t, "["+sample+"]"), "locomo", 0)
	if err != nil || !strings.Contains(plan.groups[0].Fixtures[0].Content, full) {
		t.Fatal("source text truncated", err)
	}
	abs := strings.Replace(longmemSample, "question-1", "question_abs", 1)
	plan, err = readDataset(datasetFile(t, "["+abs+","+longmemSample+"]"), "longmemeval", 0)
	if err != nil || plan.excluded["abstention"] != 1 || len(plan.groups) != 1 {
		t.Fatal(plan, err)
	}
	for _, row := range []struct{ suite, raw string }{
		{"locomo", "null"}, {"locomo", "[]"}, {"locomo", "[{}]"},
		{"locomo", "[" + strings.Replace(locomoSample, `["D1:1"]`, `["missing"]`, 1) + "]"},
		{"locomo", "[" + strings.Replace(locomoSample, `["D1:1"]`, `["D1:1","D1:1"]`, 1) + "]"},
		{"longmemeval", "[" + strings.Replace(longmemSample, `"haystack_session_ids":["session-1"]`, `"haystack_session_ids":["session-1","extra"]`, 1) + "]"},
		{"longmemeval", "[" + strings.Replace(longmemSample, `"answer_session_ids":["session-1"]`, `"answer_session_ids":["unknown"]`, 1) + "]"},
	} {
		if _, err := readDataset(datasetFile(t, row.raw), row.suite, 0); err == nil {
			t.Fatal("malformed dataset accepted", row)
		}
	}
}

func TestDatasetIsolatedGoReplay(t *testing.T) {
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	dir := t.TempDir()
	script := filepath.Join(dir, "embed.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nprintf '[1,0,0]\\n'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	schema := "../../../../../src/modules/db2/c/schema.sql"
	for _, suite := range []string{"locomo", "longmemeval"} {
		sample := locomoSample
		if suite == "longmemeval" {
			sample = longmemSample
		}
		// Repeated source IDs and keys in separate groups must not share an owner.
		path := datasetFile(t, "["+sample+","+sample+"]")
		var output bytes.Buffer
		if err := runDataset(context.Background(), schema, 3, path, suite, 0, script, "", "json", "", "", &output); err != nil {
			t.Fatal(err)
		}
		var got struct {
			Suite   string `json:"suite"`
			Samples int    `json:"samples"`
			Metrics struct {
				Cases int     `json:"cases"`
				MRR   float64 `json:"mrr"`
			}
			Excluded      map[string]int `json:"excluded_cases"`
			FixturePolicy string         `json:"fixture_policy"`
		}
		if err := json.Unmarshal(output.Bytes(), &got); err != nil || got.Suite != suite || got.Samples != 2 || got.Metrics.Cases != 2 || got.Metrics.MRR != 1 || got.FixturePolicy != "full-text-raw-query-v1" {
			t.Fatal(output.String(), err)
		}
		output.Reset()
		if err := runDataset(context.Background(), schema, 3, path, suite, 0, "printf '[1,0]'", "", "json", "", "", &output); err == nil || output.Len() != 0 {
			t.Fatal("failed embedding published partial dataset scores", err, output.String())
		}
	}
}
