package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"os"
	"path/filepath"
	"strconv"
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
	t.Setenv("AIMEE_KB_STORE_EVAL_URL", url)
	script := filepath.Join(t.TempDir(), "embed.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nprintf '[1,0,0]\\n'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	return "../../../../../src/modules/kb/c/schema.sql", script
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
	model := func(_ context.Context, input *canonical.Request) (modelReply, error) {
		system, prompt, tokens := qaFixtureViews(input)
		assertQATaskOrigins(t, input)
		_, _, _ = system, prompt, tokens
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
				return qaFixtureReply("wrong [1]", 0, 0), nil
			}
			return qaFixtureReply("Orion", 7, 2), nil
		}
		if system != judgeSystem || tokens != 64 || !strings.Contains(prompt, "Gold answer: Orion") {
			t.Fatal("wrong judge contract", prompt)
		}
		if answers == 1 {
			return qaFixtureReply(`{"score":0}`, 0, 0), nil
		}
		return qaFixtureReply(`{"score":1}`, 0, 0), nil
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
		opts.model = func(_ context.Context, input *canonical.Request) (modelReply, error) {
			system, prompt, tokens := qaFixtureViews(input)
			_, _, _ = system, prompt, tokens
			calls++
			if calls >= 3 && !badJudge {
				return modelReply{}, errors.New("model unavailable")
			}
			if system == judgeSystem {
				if calls >= 4 {
					return qaFixtureReply(`{"score":null}`, 0, 0), nil
				}
				return qaFixtureReply(`{"score":1}`, 0, 0), nil
			}
			return qaFixtureReply("Orion", 0, 0), nil
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
	model := func(_ context.Context, input *canonical.Request) (modelReply, error) {
		system, prompt, tokens := qaFixtureViews(input)
		_, _, _ = system, prompt, tokens
		if system == judgeSystem {
			return qaFixtureReply(`{"score":1}`, 0, 0), nil
		}
		return qaFixtureReply("Orion", 0, 0), nil
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

func qaFixtureReply(text string, input, output int) modelReply {
	r := canonical.TextResponse(text)
	r.Usage.Input = strconv.Itoa(input)
	r.Usage.Output = strconv.Itoa(output)
	return modelReply{Output: r}
}
func qaCLI(t *testing.T, wire []byte, strictArgs bool) string {
	t.Helper()
	script := filepath.Join(t.TempDir(), "aimee")
	check := ""
	if strictArgs {
		check = "[ \"$#\" -eq 3 ] && [ \"$1\" = --json ] && [ \"$2\" = model ] && [ \"$3\" = generate ] || exit 2\n"
	}
	body := "#!/bin/sh\n" + check + "cat > \"$0.request\"\ncat \"$0.response\"\n"
	if err := os.WriteFile(script, []byte(body), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(script+".response", wire, 0600); err != nil {
		t.Fatal(err)
	}
	return script
}
func qaWire(t *testing.T, status string, r *canonical.Response) []byte {
	t.Helper()
	envelope := map[string]any{"ir": r}
	if status != "" {
		envelope["status"] = status
	}
	wire, err := json.Marshal(envelope)
	if err != nil {
		t.Fatal(err)
	}
	return wire
}
func TestAgentModelUsesRegisteredCanonicalCLIWithoutShell(t *testing.T) {
	response := qaFixtureReply("Orion", 4, 1)
	script := qaCLI(t, qaWire(t, "", response.Output), true)
	prompt := "$(exit 9); " + string(rune(96)) + "exit 8" + string(rune(96))
	reply, err := agentModel(script)(context.Background(), qaFixtureRequest("system", prompt, 256))
	if err != nil || reply.Response != "Orion" || reply.PromptTokens != 4 || reply.CompletionTokens != 1 || reply.Output == nil {
		t.Fatal(reply, err)
	}
	raw, err := os.ReadFile(script + ".request")
	if err != nil {
		t.Fatal(err)
	}
	var request struct {
		IR *canonical.Request `json:"ir"`
	}
	if !canonical.Decode(raw, &request) || request.IR == nil || !request.IR.Valid() {
		t.Fatalf("invalid canonical request %s", raw)
	}
	r := request.IR
	if len(r.System) != 1 || *r.System[0].Text != "system" || len(r.Messages) != 1 || *r.Messages[0].Blocks[0].Text != prompt || len(r.Tools) != 0 || r.ToolChoice.Mode != "none" || *r.MaxTokens != 256 || *r.Temperature != .3 {
		t.Fatal(r)
	}
	policy, evidence := r.System[0].Context, r.Messages[0].Blocks[0].Context
	if policy.Origin != 2 || policy.Authority != 3 || policy.Trust != 2 || evidence.Origin != 6 || evidence.Authority != 1 || evidence.Trust != 2 {
		t.Fatal(policy, evidence)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := agentModel(script)(ctx, qaFixtureRequest("system", "question", 256)); err == nil {
		t.Fatal("cancelled model accepted")
	}
}
func TestAgentModelRejectsLegacyFlatResponse(t *testing.T) {
	// Either command spelling is permitted here to isolate old flat-parser authority.
	legacy, err := json.Marshal(map[string]any{"status": "ok", "response": "Orion", "prompt_tokens": 4, "completion_tokens": 1})
	if err != nil {
		t.Fatal(err)
	}
	script := qaCLI(t, legacy, false)
	if _, err := agentModel(script)(context.Background(), qaFixtureRequest("system", "prompt", 256)); err == nil {
		t.Fatal("legacy flat response accepted")
	}
}
func TestCheckedModelRequiresCanonicalAuthority(t *testing.T) {
	model := func(context.Context, *canonical.Request) (modelReply, error) {
		return modelReply{Response: "POISONED complete answer", PromptTokens: 9}, nil
	}
	if _, err := checkedModel(context.Background(), model, qaFixtureRequest("system", "prompt", 1)); err == nil {
		t.Fatal("injected raw model answer accepted")
	}
	model = func(context.Context, *canonical.Request) (modelReply, error) {
		r := qaFixtureReply("real", 7, 2)
		r.Response = "POISONED"
		r.PromptTokens = 99
		return r, nil
	}
	r, err := checkedModel(context.Background(), model, qaFixtureRequest("system", "prompt", 1))
	if err != nil || r.Response != "real" || r.PromptTokens != 7 || r.CompletionTokens != 2 {
		t.Fatal(r, err)
	}
}
func TestAgentModelCanonicalTerminalAdmission(t *testing.T) {
	text := qaFixtureReply("partial", 0, 0).Output
	for _, stop := range []canonical.StopReason{canonical.StopUnknown, canonical.StopMaxTokens, canonical.StopToolUse, canonical.StopContentFilter, canonical.StopError} {
		t.Run(fmt.Sprint(stop), func(t *testing.T) {
			r := *text
			r.StopReason = stop
			script := qaCLI(t, qaWire(t, "", &r), true)
			reply, err := agentModel(script)(context.Background(), qaFixtureRequest("system", "prompt", 256))
			if err == nil || reply.Output == nil || reply.Output.StopReason != stop || reply.Response != "" {
				t.Fatal("noncomplete canonical output lost or promoted", stop, reply, err)
			}
		})
	}
	if _, err := agentModel(qaCLI(t, qaWire(t, "error", text), true))(context.Background(), qaFixtureRequest("system", "prompt", 256)); err == nil {
		t.Fatal("host error promoted to success")
	}
	media := *text
	media.Blocks = []canonical.ResponseBlock{{Block: canonical.Block{Type: "image", MediaType: "image/png", MediaRef: "https://example.invalid/image"}}}
	if _, err := agentModel(qaCLI(t, qaWire(t, "ok", &media), true))(context.Background(), qaFixtureRequest("system", "prompt", 256)); err == nil {
		t.Fatal("media entered QA text grading")
	}
	usage := *text
	usage.Usage = canonical.ZeroUsage()
	usage.Usage = canonical.ResponseUsage{Unobserved: true}
	if _, err := agentModel(qaCLI(t, qaWire(t, "ok", &usage), true))(context.Background(), qaFixtureRequest("system", "prompt", 256)); err == nil || !strings.Contains(err.Error(), "observed provider token usage") {
		t.Fatal("unknown usage promoted to measured evaluation", err)
	}
	usage.Usage = canonical.ZeroUsage()
	usage.Usage.Input = "18446744073709551615"
	if _, err := agentModel(qaCLI(t, qaWire(t, "ok", &usage), true))(context.Background(), qaFixtureRequest("system", "prompt", 256)); err == nil {
		t.Fatal("overflow usage silently converted")
	}
}
func TestAgentModelBoundsOutput(t *testing.T) {
	script := filepath.Join(t.TempDir(), "aimee")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nhead -c 1048577 /dev/zero\n"), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := agentModel(script)(context.Background(), qaFixtureRequest("system", "prompt", 256)); err == nil {
		t.Fatal("oversized model output accepted")
	}
	if _, err := agentModel(script)(context.Background(), qaFixtureRequest("system", strings.Repeat("x", 1<<20), 256)); err == nil || !strings.Contains(err.Error(), "request exceeds") {
		t.Fatal("oversized request reached subprocess", err)
	}
}

func TestQAUnanswerableKeepsGoldOutOfReader(t *testing.T) {
	schema, command := moduleEvaluationFixture(t)
	sample := strings.Replace(longmemSample, "question-1", "question_abs", 1)
	sample = strings.Replace(sample, `"question":`, `"answer":"GOLD_CANARY","question":`, 1)
	var output bytes.Buffer
	calls := 0
	model := func(_ context.Context, input *canonical.Request) (modelReply, error) {
		system, prompt, tokens := qaFixtureViews(input)
		_, _, _ = system, prompt, tokens
		calls++
		if system == answerSystem {
			if strings.Contains(prompt, "GOLD_CANARY") || strings.Contains(prompt, "Unanswerable from") {
				t.Fatal("gold leaked into reader")
			}
			return qaFixtureReply("Unknown", 0, 0), nil
		}
		if !strings.Contains(prompt, "Unanswerable from supplied history") {
			t.Fatal("unanswerable judge not selected")
		}
		return qaFixtureReply(`{"score":1}`, 0, 0), nil
	}
	if err := runDatasetQA(context.Background(), schema, 3, datasetFile(t, "["+sample+"]"), "longmemeval-qa", 0, command, "", "json", "", "", qaOptions{10, 2000, 5, false, model}, &output); err != nil {
		t.Fatal(err)
	}
	if calls != 2 || !strings.Contains(output.String(), `"unanswerable_abstained":1`) || !strings.Contains(output.String(), `"abstention_precision":1`) {
		t.Fatal(output.String())
	}
}

func TestAgentModelJudgeSamplingAndCanonicalProjection(t *testing.T) {
	response := qaFixtureReply("ignored", 3, 1).Output
	a, b, thinking := "Or", "ion", "private reasoning"
	response.Blocks = []canonical.ResponseBlock{{Block: canonical.Block{Type: "text", Text: &a}}, {Block: canonical.Block{Type: "thinking", Text: &thinking}}, {Block: canonical.Block{Type: "text", Text: &b}}}
	script := qaCLI(t, qaWire(t, "", response), true)
	result, err := agentModel(script)(context.Background(), judgeEvaluationRequest("question", "gold", "candidate evidence", nil))
	if err != nil || result.Response != "Orion" {
		t.Fatal(result, err)
	}
	raw, err := os.ReadFile(script + ".request")
	if err != nil {
		t.Fatal(err)
	}
	var envelope map[string]json.RawMessage
	if !canonical.Decode(raw, &envelope) {
		t.Fatal("bad request")
	}
	var input canonical.Request
	if !canonical.Decode(envelope["ir"], &input) || input.Temperature == nil || *input.Temperature != 0 || input.MaxTokens == nil || *input.MaxTokens != 64 {
		t.Fatal(input)
	}
}
func TestAgentModelNULRejectedBeforeCLI(t *testing.T) {
	script := qaCLI(t, qaWire(t, "ok", qaFixtureReply("valid", 1, 1).Output), true)
	for _, pair := range [][2]string{{"bad" + string(rune(0)), "prompt"}, {"system", "bad" + string(rune(0))}} {
		if _, err := agentModel(script)(context.Background(), qaFixtureRequest(pair[0], pair[1], 64)); err == nil {
			t.Fatal("NUL input accepted")
		}
	}
	if _, err := os.Stat(script + ".request"); !os.IsNotExist(err) {
		t.Fatal("invalid request reached executable", err)
	}
}

func TestAgentModelRejectsContradictoryHostError(t *testing.T) {
	for _, hostError := range []any{"host failed", map[string]any{"kind": "unavailable"}, false} {
		wire, err := json.Marshal(map[string]any{"ir": canonical.TextResponse("observed"), "error": hostError})
		if err != nil {
			t.Fatal(err)
		}
		reply, err := agentModel(qaCLI(t, wire, true))(context.Background(), qaFixtureRequest("system", "prompt", 256))
		if err == nil {
			t.Fatal("contradictory host error promoted to answer")
		}
		if reply.Output == nil || reply.Response != "" {
			t.Fatal("observed canonical output lost or projected as answer", reply)
		}
	}
}

func TestAgentModelCLISuccessEnvelopeAndFailureAuthority(t *testing.T) {
	for _, status := range []string{"", "ok"} {
		t.Run("success-status-"+status, func(t *testing.T) {
			wire := qaWire(t, status, canonical.TextResponse("actual CLI answer"))
			reply, err := agentModel(qaCLI(t, wire, true))(context.Background(), qaFixtureRequest("system", "prompt", 32))
			if err != nil || reply.Output == nil || reply.Response != "actual CLI answer" {
				t.Fatal(reply, err)
			}
		})
	}
	for _, status := range []any{nil, false, true, 0, "", "error", "done"} {
		t.Run(fmt.Sprintf("invalid-status-%v", status), func(t *testing.T) {
			wire, err := json.Marshal(map[string]any{"ir": canonical.TextResponse("observed"), "status": status})
			if err != nil {
				t.Fatal(err)
			}
			reply, err := agentModel(qaCLI(t, wire, true))(context.Background(), qaFixtureRequest("system", "prompt", 32))
			if err == nil || reply.Output == nil || reply.Response != "" {
				t.Fatal("invalid status granted answer authority", reply, err)
			}
		})
	}
	for _, status := range []string{"", "ok"} {
		t.Run("nonzero-exit-status-"+status, func(t *testing.T) {
			script := qaCLI(t, qaWire(t, status, canonical.TextResponse("observed")), true)
			f, err := os.OpenFile(script, os.O_APPEND|os.O_WRONLY, 0)
			if err != nil {
				t.Fatal(err)
			}
			if _, err = f.WriteString("exit 1\n"); err != nil {
				t.Fatal(err)
			}
			f.Close()
			reply, err := agentModel(script)(context.Background(), qaFixtureRequest("system", "prompt", 32))
			if err == nil || reply.Output == nil || reply.Response != "" {
				t.Fatal("failed transport promoted canonical diagnostic", reply, err)
			}
		})
	}
	for _, wire := range []string{`{}`, `{"ir":null}`, `{"status":"ok"}`, `{"ir":{"schema":"wrong"}}`} {
		if reply, err := agentModel(qaCLI(t, []byte(wire), true))(context.Background(), qaFixtureRequest("system", "prompt", 32)); err == nil || reply.Response != "" {
			t.Fatal("missing or malformed canonical observation accepted", reply, err)
		}
	}
	wire, err := json.Marshal(map[string]any{"ir": canonical.TextResponse("no host error"), "error": nil})
	if err != nil {
		t.Fatal(err)
	}
	if reply, err := agentModel(qaCLI(t, wire, true))(context.Background(), qaFixtureRequest("system", "prompt", 32)); err != nil || reply.Response != "no host error" {
		t.Fatal(reply, err)
	}
}

// The canonical primary integration supplies the genuine built CLI and checks
// four actual local-provider requests, so a fake executable cannot satisfy it.
func TestAgentModelLiveRegisteredCLI(t *testing.T) {
	executable := os.Getenv("AIMEE_QA_LIVE_CLI")
	if executable == "" {
		t.Skip("requires the canonical primary local-provider integration")
	}
	for _, test := range []struct {
		prompt string
		stop   canonical.StopReason
	}{
		{"IR_QA_ANSWER", canonical.StopEndTurn},
		{"IR_QA_PARTIAL", canonical.StopMaxTokens},
		{"IR_QA_REFUSED", canonical.StopContentFilter},
		{"IR_QA_TOOL", canonical.StopToolUse},
	} {
		t.Run(test.prompt, func(t *testing.T) {
			reply, err := agentModel(executable)(context.Background(), qaFixtureRequest("Use only the supplied evidence.", test.prompt, 256))
			if reply.Output == nil || !reply.Output.Valid() || reply.Output.StopReason != test.stop || reply.Output.Usage.Input != "10" || reply.Output.Usage.Output != "2" {
				t.Fatalf("actual CLI canonical observation lost: reply=%+v error=%v", reply, err)
			}
			if test.stop == canonical.StopEndTurn {
				if err != nil || reply.Response != "PRIMARY_OK" || reply.PromptTokens != 10 || reply.CompletionTokens != 2 {
					t.Fatal(reply, err)
				}
			} else if err == nil || reply.Response != "" || reply.PromptTokens != 0 || reply.CompletionTokens != 0 {
				t.Fatal("declined/incomplete actual CLI observation promoted to QA answer", reply, err)
			}
		})
	}
}

// Synthetic authored source for transport-only tests; production task producers
// are separately exercised by the dataset QA fixtures above.
func qaFixtureRequest(system, prompt string, tokens int) *canonical.Request {
	temperature := .3
	if system == judgeSystem {
		temperature = 0
	}
	return &canonical.Request{Version: 1, SourceWire: "unspecified", CachePolicy: "native",
		System:   []canonical.Block{qaBlock(system, 2, 3, 2)},
		Messages: []canonical.Message{{Role: "user", ContentScalar: true, Blocks: []canonical.Block{qaBlock(prompt, 6, 1, 2)}}},
		Tools:    []canonical.Tool{}, Stops: []string{}, ToolChoice: canonical.ToolChoice{Mode: "none"}, MaxTokens: &tokens, Temperature: &temperature}
}
func qaFixtureViews(input *canonical.Request) (system, prompt string, tokens int) {
	for _, block := range input.System {
		if block.Text != nil {
			system += *block.Text
		}
	}
	for _, message := range input.Messages {
		for _, block := range message.Blocks {
			if block.Text != nil {
				prompt += *block.Text
			}
		}
	}
	if input.MaxTokens != nil {
		tokens = *input.MaxTokens
	}
	return
}

func assertQATaskOrigins(t *testing.T, input *canonical.Request) {
	t.Helper()
	if !input.Valid() || len(input.System) != 1 || input.System[0].Context == nil {
		t.Fatal("QA producer omitted canonical policy")
	}
	policy := input.System[0].Context
	if policy.Origin != 1 || policy.Authority != 3 || policy.Trust != 4 {
		t.Fatal("fixed grading policy lost its host origin", policy)
	}
	system, _, _ := qaFixtureViews(input)
	blocks := input.Messages[0].Blocks
	want := [][3]int{{2, 2, 2}, {6, 1, 2}, {1, 2, 4}}
	if system == judgeSystem {
		want = [][3]int{{2, 1, 2}, {2, 1, 2}, {7, 1, 2}, {1, 2, 4}}
		if len(blocks) > 1 && strings.Contains(*blocks[1].Text, "Unanswerable from supplied history") {
			want[1] = [3]int{1, 2, 4}
		}
	}
	if len(blocks) != len(want) {
		t.Fatalf("QA blocks=%d want=%d", len(blocks), len(want))
	}
	for i, expected := range want {
		context := blocks[i].Context
		if context == nil || context.Origin != expected[0] || context.Authority != expected[1] || context.Trust != expected[2] || !context.ModelVisible {
			t.Fatalf("QA source block %d has wrong custody: %+v", i, context)
		}
	}
}

func TestAgentModelPreservesCompleteCanonicalInput(t *testing.T) {
	input := judgeEvaluationRequest("question 漢字", "gold", "candidate", nil)
	temperature, topP := .17, .8
	input.Temperature, input.TopP = &temperature, &topP
	input.Stops = []string{"END"}
	input.Metadata = json.RawMessage(`{"exact":9007199254740993123456789}`)
	input.ServiceTier = "future-tier"
	input.Messages[0].Blocks[1].Context.RevisionDomain = "qa-source"
	input.Messages[0].Blocks[1].Context.RevisionScope = "unit"
	input.Messages[0].Blocks[1].Context.RevisionEpoch = "17"
	before, err := json.Marshal(input)
	if err != nil || !input.Valid() {
		t.Fatal("invalid source", err)
	}
	script := qaCLI(t, qaWire(t, "ok", qaFixtureReply("kept", 3, 1).Output), true)
	if _, err := agentModel(script)(t.Context(), input); err != nil {
		t.Fatal(err)
	}
	wire, err := os.ReadFile(script + ".request")
	if err != nil {
		t.Fatal(err)
	}
	var envelope struct {
		IR *canonical.Request `json:"ir"`
	}
	if !canonical.Decode(wire, &envelope) || envelope.IR == nil {
		t.Fatal("missing canonical stdin")
	}
	sent, err := json.Marshal(envelope.IR)
	if err != nil || !bytes.Equal(before, sent) {
		t.Fatal("QA transport changed canonical request", err)
	}
	after, _ := json.Marshal(input)
	if !bytes.Equal(before, after) {
		t.Fatal("QA transport mutated its borrowed source")
	}
}
