package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os/exec"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode"

	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

type modelReply struct {
	Response         string `json:"response"`
	PromptTokens     int    `json:"prompt_tokens"`
	CompletionTokens int    `json:"completion_tokens"`
}
type evaluationModel func(context.Context, string, string, int) (modelReply, error)

type limitedModelOutput struct{ buffer bytes.Buffer }

func (b *limitedModelOutput) Len() int      { return b.buffer.Len() }
func (b *limitedModelOutput) Bytes() []byte { return b.buffer.Bytes() }

func (b *limitedModelOutput) Write(p []byte) (int, error) {
	if b.Len()+len(p) > 1<<20 {
		return 0, errors.New("evaluation model output exceeds 1 MiB")
	}
	return b.buffer.Write(p)
}

// The existing plain-completion runner owns provider configuration/execution.
// JSON stdin preserves full prompts without shell interpolation or argv limits.
// The agentic run path would consume live hints and record feedback; never use it.
func agentModel(executable string) evaluationModel {
	return func(ctx context.Context, system, prompt string, tokens int) (modelReply, error) {
		var reply modelReply
		if executable == "" {
			return reply, errors.New("QA requires -agent-executable (the configured aimee CLI)")
		}
		ctx, cancel := context.WithTimeout(ctx, 3*time.Minute)
		defer cancel()
		if strings.IndexByte(system, 0) >= 0 || strings.IndexByte(prompt, 0) >= 0 {
			return reply, errors.New("evaluation model request contains NUL")
		}
		temperature := .3
		if system == judgeSystem {
			temperature = 0
		}
		request, err := json.Marshal(map[string]any{"system": system, "prompt": prompt, "max_tokens": tokens, "temperature": temperature})
		if err != nil {
			return reply, err
		}
		if len(request) > 1<<20 {
			return reply, errors.New("evaluation model request exceeds 1 MiB")
		}
		cmd := exec.CommandContext(ctx, executable, "--json", "agent", "generate")
		cmd.Stdin = bytes.NewReader(request)
		cmd.WaitDelay = time.Second
		var output, diagnostic limitedModelOutput
		cmd.Stdout, cmd.Stderr = &output, &diagnostic
		if err := cmd.Run(); err != nil {
			return reply, fmt.Errorf("evaluation agent failed: %w", err)
		}
		if err := json.Unmarshal(output.Bytes(), &reply); err != nil {
			return reply, fmt.Errorf("evaluation agent response: %w", err)
		}
		return reply, nil
	}
}

const answerSystem = "Answer benchmark questions using only the provided memory context. If the context is insufficient, answer Unknown. Respond with only the answer."
const judgeSystem = "You are grading benchmark answers. Compare the candidate answer to the gold answer for semantic equivalence. Ignore wording differences. Be strict about factual disagreement. Return JSON only: {\"score\":1} for correct or {\"score\":0} for incorrect."

var citationMarker = regexp.MustCompile(`\[[1-9][0-9]*\]`)

func normalizeAnswer(s string) string {
	return strings.Join(strings.FieldsFunc(strings.ToLower(s), func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsNumber(r) }), " ")
}
func judgeScore(s string) (int, error) {
	var result struct {
		Score *int `json:"score"`
	}
	if err := json.Unmarshal([]byte(s), &result); err != nil {
		return 0, fmt.Errorf("invalid judge JSON: %w", err)
	}
	if result.Score == nil || (*result.Score != 0 && *result.Score != 1) {
		return 0, errors.New("judge score must be 0 or 1")
	}
	return *result.Score, nil
}
func checkedModel(ctx context.Context, model evaluationModel, system, prompt string, tokens int) (modelReply, error) {
	reply, err := model(ctx, system, prompt, tokens)
	if err != nil {
		return reply, err
	}
	if strings.TrimSpace(reply.Response) == "" || len(reply.Response) > 1<<20 || reply.PromptTokens < 0 || reply.CompletionTokens < 0 {
		return reply, errors.New("empty or invalid evaluation model response")
	}
	// Keep the legacy byte/4 fallback visibly labelled in the report.
	if reply.PromptTokens == 0 {
		reply.PromptTokens = (len(system)+3)/4 + (len(prompt)+3)/4
	}
	if reply.CompletionTokens == 0 {
		reply.CompletionTokens = (len(reply.Response) + 3) / 4
	}
	return reply, ctx.Err()
}

type qaOptions struct {
	topK, tokenBudget, maxFailures int
	reportFailures                 bool
	model                          evaluationModel
}

func runDatasetQA(ctx context.Context, schema string, dimension int, path, suite string, maxCases int, command, socket, format, fields, profile string, opts qaOptions, output io.Writer) error {
	if format != "text" && format != "json" {
		return errors.New("evaluation format must be json or text")
	}
	if opts.topK < 1 || opts.topK > 32 || opts.tokenBudget < 1 || opts.tokenBudget > 131072 || opts.maxFailures < 1 || opts.maxFailures > 100 || opts.model == nil {
		return errors.New("QA requires top-k 1..32, token-budget 1..131072, max-failures 1..100 and a model")
	}
	plan, err := readDataset(path, suite, maxCases)
	if err != nil {
		return err
	}
	executor, closeExecutor, err := evaluationExecutor(ctx, command, socket)
	if err != nil {
		return err
	}
	defer closeExecutor()
	metrics := map[string]float64{"cases": 0, "accuracy": 0, "exact_match": 0, "avg_retrieved_tokens": 0, "answered_cases": 0, "judged_cases": 0, "cited_answers": 0, "uncited_answers": 0, "low_confidence_answers": 0, "answer_prompt_tokens": 0, "answer_completion_tokens": 0, "judge_prompt_tokens": 0, "judge_completion_tokens": 0}
	failures := []map[string]any{}
	latencies := []float64{}
	for groupIndex, corpus := range plan.groups {
		err = evaluationSession(ctx, schema, dimension, func(db *postgres.EvaluationStore) error {
			module, err := memory.NewEvaluationModule(ctx, db, executor)
			if err != nil {
				return err
			}
			if _, err = module.Seed(corpus.Fixtures, command); err != nil {
				return err
			}
			for _, row := range corpus.Cases {
				start := time.Now()
				var contextResult struct {
					Context string `json:"context"`
					Tokens  int    `json:"tokens"`
				}
				if err = module.Call(memory.StageCommand, "runtime", map[string]any{"operation": "benchmark-context", "query": row.Query, "top_k": opts.topK, "token_budget": opts.tokenBudget, "capacity": 512 * 1024}, &contextResult); err != nil {
					return err
				}
				contextText := contextResult.Context
				if contextText == "" {
					contextText = "[no retrieved context]"
				}
				prompt := fmt.Sprintf("Question: %s\n\nRetrieved memory context:\n%s\nFinal answer:", row.Query, contextText)
				answer, err := checkedModel(ctx, opts.model, answerSystem, prompt, 256)
				if err != nil {
					return err
				}
				prompt = fmt.Sprintf("Question: %s\nGold answer: %s\nCandidate answer: %s\n\nReturn JSON only.", row.Query, row.Answer, answer.Response)
				judge, err := checkedModel(ctx, opts.model, judgeSystem, prompt, 64)
				if err != nil {
					return err
				}
				score, err := judgeScore(judge.Response)
				if err != nil {
					return err
				}
				exact := normalizeAnswer(row.Answer) != "" && normalizeAnswer(row.Answer) == normalizeAnswer(answer.Response)
				metrics["cases"]++
				metrics["answered_cases"]++
				metrics["judged_cases"]++
				metrics["accuracy"] += float64(score)
				if exact {
					metrics["exact_match"]++
				}
				metrics["avg_retrieved_tokens"] += float64(contextResult.Tokens)
				if citationMarker.MatchString(answer.Response) {
					metrics["cited_answers"]++
				} else {
					metrics["uncited_answers"]++
				}
				if strings.HasPrefix(answer.Response, "## Retrieval Confidence: LOW") {
					metrics["low_confidence_answers"]++
				}
				metrics["answer_prompt_tokens"] += float64(answer.PromptTokens)
				metrics["answer_completion_tokens"] += float64(answer.CompletionTokens)
				metrics["judge_prompt_tokens"] += float64(judge.PromptTokens)
				metrics["judge_completion_tokens"] += float64(judge.CompletionTokens)
				latencies = append(latencies, float64(time.Since(start))/float64(time.Millisecond))
				if opts.reportFailures && score == 0 && len(failures) < opts.maxFailures {
					failures = append(failures, map[string]any{"sample": groupIndex, "id": row.ID, "question": row.Query, "gold": row.Answer, "answer": answer.Response, "judge": judge.Response, "exact": exact, "retrieved_tokens": contextResult.Tokens, "context": contextResult.Context})
				}
			}
			return nil
		})
		if err != nil {
			return fmt.Errorf("%s sample %d: %w", suite, groupIndex, err)
		}
	}
	n := metrics["cases"]
	if n == 0 {
		return errors.New("QA evaluation has no cases")
	}
	for _, key := range []string{"accuracy", "exact_match", "avg_retrieved_tokens"} {
		metrics[key] /= n
	}
	metrics["citation_coverage"] = metrics["cited_answers"] / n
	metrics["citation_miss_rate"] = metrics["uncited_answers"] / n
	metrics["hallucination_rate"] = 1 - metrics["accuracy"]
	sort.Float64s(latencies)
	percentile := func(p float64) float64 { return latencies[int(math.Ceil(float64(len(latencies))*p))-1] }
	view := map[string]any{"status": "ok", "suite": suite, "dataset": path, "samples": len(plan.groups), "excluded_cases": plan.excluded, "fixture_policy": "full-text-raw-query-v1", "qa_policy": "module-context-strict-judge-v1", "token_accounting": "provider-or-byte4-estimate", "citation_policy": "numbered-marker-presence", "metrics": metrics, "route_buckets": map[string]any{}, "shape_buckets": map[string]any{}, "latency_scope": "owner-context-answer-judge", "latency": map[string]any{"p50_ms": percentile(.5), "p95_ms": percentile(.95), "p99_ms": percentile(.99), "min_ms": latencies[0], "max_ms": latencies[len(latencies)-1], "queries": len(latencies)}}
	if opts.reportFailures {
		view["failures"] = failures
	}
	return writeEvaluationView(view, format, fields, profile, output)
}

func writeEvaluationView(view map[string]any, format, fields, profile string, output io.Writer) error {
	var raw []byte
	var err error
	if format == "text" {
		raw, err = json.MarshalIndent(view, "", "  ")
		raw = append(raw, '\n')
	} else {
		raw, err = memory.FormatEvaluationJSON(view, fields, profile)
	}
	if err != nil {
		return err
	}
	_, err = output.Write(raw)
	return err
}
