package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func moduleEvaluationFixture(t *testing.T) (string, string) {
	t.Helper()
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	script := filepath.Join(t.TempDir(), "embed.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nprintf '[1,0,0]\\n'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	return "../../../../../src/modules/db2/c/schema.sql", script
}

func TestQADatasetValidation(t *testing.T) {
	sample := strings.ReplaceAll(locomoSample, `"question":`, `"answer":"Orion","question":`)
	plan, err := readDataset(datasetFile(t, "["+sample+"]"), "locomo-qa", 0)
	if err != nil || len(plan.groups[0].Cases) != 2 || len(plan.excluded) != 0 {
		t.Fatal(plan, err)
	}
	for _, raw := range []string{`null`, `true`, `{}`, `[]`, `""`} {
		if _, err := datasetAnswer(json.RawMessage(raw)); err == nil {
			t.Fatal("invalid gold accepted", raw)
		}
	}
	if got, err := datasetAnswer(json.RawMessage(`1234567890123456789`)); err != nil || got != "1234567890123456789" {
		t.Fatal(got, err)
	}
	if _, err := readDataset(datasetFile(t, "["+locomoSample+"]"), "locomo-qa", 0); err == nil {
		t.Fatal("missing gold accepted")
	}
	for _, raw := range []string{`{"score":true}`, `{"score":0.5}`, `{"score":null}`, `{}`, `{"score":2}`, `{"score":1} trailing`} {
		if _, err := judgeScore(raw); err == nil {
			t.Fatal("invalid judge accepted", raw)
		}
	}
	if normalizeAnswer("É文, Orion!") != "é文 orion" {
		t.Fatal("Unicode exact match normalization")
	}
}

func TestQAUsesIsolatedModuleAndScoredFailureAttempt(t *testing.T) {
	schema, command := moduleEvaluationFixture(t)
	sample := strings.ReplaceAll(locomoSample, `"question":`, `"answer":"Orion","question":`)
	first := strings.ReplaceAll(sample, "nebula location is Orion", "alpha-only nebula location is Orion")
	second := strings.ReplaceAll(sample, "nebula location is Orion", "beta-only nebula location is Orion")
	path := datasetFile(t, "["+first+","+second+"]")
	calls, answers := 0, 0
	model := func(_ context.Context, system, prompt string, tokens int) (modelReply, error) {
		calls++
		if system == answerSystem {
			answers++
			if tokens != 256 || !strings.Contains(prompt, "Retrieved memory context:") {
				t.Fatal("wrong model contract", prompt)
			}
			want, absent := "alpha-only", "beta-only"
			if answers > 2 {
				want, absent = absent, want
			}
			// The 'unknown' question can have no lexical overlap; the semantic fixture
			// embedder still admits the one eligible record.
			if !strings.Contains(prompt, want) || strings.Contains(prompt, absent) {
				t.Fatal("sample storage leaked or context missing", prompt)
			}
			if answers == 1 {
				return modelReply{Response: "wrong [1]"}, nil
			}
			return modelReply{Response: "Orion", PromptTokens: 7, CompletionTokens: 2}, nil
		}
		if system != judgeSystem || tokens != 64 || !strings.Contains(prompt, "Gold answer: Orion") {
			t.Fatal("wrong judge contract", prompt)
		}
		if answers == 1 {
			return modelReply{Response: `{"score":0}`}, nil
		}
		return modelReply{Response: `{"score":1}`}, nil
	}
	var output bytes.Buffer
	opts := qaOptions{topK: 10, tokenBudget: 2000, maxFailures: 1, reportFailures: true, model: model}
	if err := runDatasetQA(context.Background(), schema, 3, path, "locomo-qa", 0, command, "", "json", "", "", opts, &output); err != nil {
		t.Fatal(err)
	}
	var got struct {
		Metrics  map[string]float64 `json:"metrics"`
		Failures []map[string]any   `json:"failures"`
		Samples  int                `json:"samples"`
	}
	if err := json.Unmarshal(output.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if calls != 8 || answers != 4 || got.Samples != 2 || got.Metrics["cases"] != 4 || got.Metrics["accuracy"] != .75 || got.Metrics["exact_match"] != .75 || got.Metrics["cited_answers"] != 1 || len(got.Failures) != 1 || got.Failures[0]["answer"] != "wrong [1]" {
		t.Fatal(calls, output.String())
	}

	// Fail after one complete scored case. No partial score or successful exact-
	// match fallback is published when the next answer or its judge fails.
	for _, badJudge := range []bool{false, true} {
		calls = 0
		output.Reset()
		opts.model = func(_ context.Context, system, prompt string, tokens int) (modelReply, error) {
			calls++
			if calls >= 3 && !badJudge {
				return modelReply{}, errors.New("model unavailable")
			}
			if system == judgeSystem {
				if calls >= 4 {
					return modelReply{Response: `{"score":null}`}, nil
				}
				return modelReply{Response: `{"score":1}`}, nil
			}
			return modelReply{Response: "Orion"}, nil
		}
		if err := runDatasetQA(context.Background(), schema, 3, path, "locomo-qa", 0, command, "", "json", "", "", opts, &output); err == nil || output.Len() != 0 {
			t.Fatal("partial QA report escaped", err, output.String())
		}
	}
}

func TestLongMemQAAndModuleMissReports(t *testing.T) {
	schema, command := moduleEvaluationFixture(t)
	sample := strings.Replace(longmemSample, `"question":`, `"answer":"Orion","question":`, 1)
	path := datasetFile(t, "["+sample+","+sample+"]")
	var output bytes.Buffer
	model := func(_ context.Context, system, prompt string, tokens int) (modelReply, error) {
		if system == judgeSystem {
			return modelReply{Response: `{"score":1}`}, nil
		}
		return modelReply{Response: "Orion"}, nil
	}
	if err := runDatasetQA(context.Background(), schema, 3, path, "longmemeval-qa", 0, command, "", "json", "metrics", "", qaOptions{10, 2000, 5, false, model}, &output); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(output.String(), `"cases":2`) || strings.Contains(output.String(), `fixture_policy`) {
		t.Fatal("QA field projection", output.String())
	}
	for _, suite := range []string{"locomo-misses", "longmemeval-misses"} {
		sample := locomoSample
		if strings.HasPrefix(suite, "long") {
			sample = longmemSample
		}
		output.Reset()
		if err := runDatasetMisses(context.Background(), schema, 3, datasetFile(t, "["+sample+"]"), suite, 0, command, "", "json", "", "", 5, 20, &output); err != nil {
			t.Fatal(err)
		}
		var got struct {
			Cases  int `json:"cases"`
			Misses int `json:"misses"`
		}
		if err := json.Unmarshal(output.Bytes(), &got); err != nil || got.Cases != 1 || got.Misses != 0 {
			t.Fatal(output.String(), err)
		}
	}
}

func TestSessionSupportLabelsFollowEvidenceSessions(t *testing.T) {
	sample := `[{"conversation":{"session_1":[{"dia_id":"a","text":"alpha"},{"dia_id":"b","text":"beta"}],"session_2":[{"dia_id":"c","text":"gamma"}]},"qa":[{"question":"alpha?","evidence":["a"]}]}]`
	plan, err := readDataset(datasetFile(t, sample), "locomo-session-support", 0)
	if err != nil || strings.Join(plan.groups[0].Cases[0].Expected, ",") != "a,b" {
		t.Fatal(plan, err)
	}
}

func TestAgentModelUsesExistingCLIWithoutShell(t *testing.T) {
	script := filepath.Join(t.TempDir(), "aimee")
	if err := os.WriteFile(script, []byte("#!/bin/sh\n[ \"$1\" = --json ] && [ \"$2\" = agent ] && [ \"$3\" = generate ] || exit 2\ncat >/dev/null\nprintf '{\"response\":\"Orion\",\"prompt_tokens\":4,\"completion_tokens\":1}'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	reply, err := agentModel(script)(context.Background(), "system", "$(exit 9); `exit 8`", 256)
	if err != nil || reply.Response != "Orion" {
		t.Fatal(reply, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := agentModel(script)(ctx, "system", "question", 256); err == nil {
		t.Fatal("cancelled model invoked")
	}
}

func TestAgentModelBoundsOutput(t *testing.T) {
	script := filepath.Join(t.TempDir(), "aimee")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nhead -c 1048577 /dev/zero\n"), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := agentModel(script)(context.Background(), "system", "prompt", 256); err == nil {
		t.Fatal("oversized model output accepted")
	}
	if _, err := agentModel(script)(context.Background(), "system", strings.Repeat("x", 1<<20), 256); err == nil || !strings.Contains(err.Error(), "request exceeds") {
		t.Fatal("oversized request reached subprocess", err)
	}
}
