package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

type datasetPlan struct {
	suite    string
	groups   []memory.EvaluationCorpus
	excluded map[string]int
}

type datasetTurn struct {
	ID      string `json:"dia_id"`
	Speaker string `json:"speaker"`
	Text    string `json:"text"`
}
type datasetQuestion struct {
	ID       string          `json:"question_id"`
	Question string          `json:"question"`
	Evidence []string        `json:"evidence"`
	Answer   json.RawMessage `json:"answer"`
}
type datasetSessionTurn struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

// Validate every selected sample before opening a database. Each group will get
// its own isolated Go owner, preserving conversation/haystack evaluation scope.
func readDataset(path, suite string, maxCases int) (datasetPlan, error) {
	plan := datasetPlan{suite: suite, excluded: map[string]int{}}
	switch suite {
	case "locomo", "longmemeval", "locomo-qa", "longmemeval-qa", "locomo-session-support", "locomo-misses", "longmemeval-misses":
	default:
		return plan, errors.New("unsupported memory evaluation suite")
	}
	qaMode := strings.HasSuffix(suite, "-qa")
	support := suite == "locomo-session-support"
	suite = strings.TrimSuffix(strings.TrimSuffix(strings.TrimSuffix(suite, "-qa"), "-misses"), "-session-support")
	if (suite != "locomo" && suite != "longmemeval") || maxCases < 0 {
		return plan, errors.New("dataset evaluation requires locomo or longmemeval and nonnegative max-cases")
	}
	f, err := os.Open(path)
	if err != nil {
		return plan, err
	}
	defer f.Close()
	raw, err := io.ReadAll(io.LimitReader(f, 512<<20+1))
	if err != nil {
		return plan, err
	}
	if len(raw) > 512<<20 {
		return plan, errors.New("dataset exceeds 512 MiB")
	}
	var samples []json.RawMessage
	if err = json.Unmarshal(raw, &samples); err != nil {
		return plan, err
	}
	if len(samples) == 0 {
		return plan, errors.New("dataset must contain samples")
	}
	for index, raw := range samples {
		if maxCases > 0 && len(plan.groups) >= maxCases {
			break
		}
		corpus := memory.EvaluationCorpus{Version: 1}
		scopes := map[string]string{}
		if suite == "locomo" {
			var sample struct {
				Conversation map[string]json.RawMessage `json:"conversation"`
				QA           []datasetQuestion          `json:"qa"`
			}
			if err = json.Unmarshal(raw, &sample); err != nil {
				return plan, err
			}
			if sample.Conversation == nil || sample.QA == nil {
				return plan, fmt.Errorf("sample %d requires conversation and qa", index)
			}
			sessions := []int{}
			for field := range sample.Conversation {
				if strings.HasPrefix(field, "session_") && !strings.HasSuffix(field, "_date_time") {
					n, err := strconv.Atoi(strings.TrimPrefix(field, "session_"))
					if err != nil || n < 1 {
						return plan, fmt.Errorf("invalid session key %q", field)
					}
					sessions = append(sessions, n)
				}
			}
			sort.Ints(sessions)
			for _, number := range sessions {
				field := fmt.Sprintf("session_%d", number)
				var turns []datasetTurn
				if err = json.Unmarshal(sample.Conversation[field], &turns); err != nil {
					return plan, err
				}
				date := ""
				if value, ok := sample.Conversation[field+"_date_time"]; ok {
					if err = json.Unmarshal(value, &date); err != nil {
						return plan, err
					}
				}
				for _, turn := range turns {
					speaker := turn.Speaker
					if speaker == "" {
						speaker = "speaker"
					}
					if strings.TrimSpace(turn.Text) == "" {
						return plan, errors.New("dataset turn is empty")
					}
					scopes[turn.ID] = field
					corpus.Fixtures = append(corpus.Fixtures, memory.EvaluationFixture{FID: turn.ID, Tier: "L2", Kind: "fact", Key: "locomo " + speaker + " " + turn.ID, Content: "[" + date + "] " + speaker + ": " + turn.Text})
				}
			}
			for questionIndex, q := range sample.QA {
				if strings.TrimSpace(q.Question) == "" || (!qaMode && q.Evidence == nil) {
					return plan, fmt.Errorf("sample %d has malformed question", index)
				}
				if !qaMode && len(q.Evidence) == 0 {
					plan.excluded["no_evidence"]++
					continue
				}
				id := q.ID
				if id == "" {
					id = fmt.Sprintf("%d:%d", index, questionIndex)
				}
				expected := q.Evidence
				if support {
					selected := map[string]bool{}
					for _, fid := range expected {
						if scopes[fid] == "" {
							return plan, fmt.Errorf("unknown relevance label %q", fid)
						}
						selected[scopes[fid]] = true
					}
					expected = nil
					for _, f := range corpus.Fixtures {
						if selected[scopes[f.FID]] {
							expected = append(expected, f.FID)
						}
					}
				}
				answer := ""
				if qaMode {
					var err error
					answer, err = datasetAnswer(q.Answer)
					if err != nil {
						return plan, err
					}
				}
				corpus.Cases = append(corpus.Cases, memory.EvaluationCase{ID: id, Query: q.Question, Expected: expected, Answer: answer})
			}
		} else {
			var sample struct {
				ID       string                 `json:"question_id"`
				Question string                 `json:"question"`
				IDs      []string               `json:"haystack_session_ids"`
				Dates    []string               `json:"haystack_dates"`
				Sessions [][]datasetSessionTurn `json:"haystack_sessions"`
				Answers  []string               `json:"answer_session_ids"`
				Answer   json.RawMessage        `json:"answer"`
			}
			if err = json.Unmarshal(raw, &sample); err != nil {
				return plan, err
			}
			if strings.TrimSpace(sample.Question) == "" || sample.IDs == nil || sample.Sessions == nil || sample.Answers == nil || len(sample.IDs) != len(sample.Sessions) || (sample.Dates != nil && len(sample.Dates) != len(sample.IDs)) {
				return plan, fmt.Errorf("sample %d has mismatched or missing history", index)
			}
			if strings.Contains(sample.ID, "_abs") {
				plan.excluded["abstention"]++
				continue
			}
			if len(sample.Answers) == 0 {
				plan.excluded["no_evidence"]++
				continue
			}
			for i, id := range sample.IDs {
				var content strings.Builder
				if sample.Dates != nil {
					fmt.Fprintf(&content, "[%s] ", sample.Dates[i])
				}
				if len(sample.Sessions[i]) == 0 {
					return plan, errors.New("dataset session is empty")
				}
				for _, turn := range sample.Sessions[i] {
					if strings.TrimSpace(turn.Content) == "" {
						return plan, errors.New("dataset turn is empty")
					}
					role := turn.Role
					if role == "" {
						role = "turn"
					}
					fmt.Fprintf(&content, "%s: %s ", role, turn.Content)
				}
				corpus.Fixtures = append(corpus.Fixtures, memory.EvaluationFixture{FID: id, Tier: "L2", Kind: "fact", Key: "longmemeval session " + id, Content: content.String()})
			}
			id := sample.ID
			if id == "" {
				id = strconv.Itoa(index)
			}
			answer := ""
			if qaMode {
				var err error
				answer, err = datasetAnswer(sample.Answer)
				if err != nil {
					return plan, err
				}
			}
			corpus.Cases = []memory.EvaluationCase{{ID: id, Query: sample.Question, Expected: sample.Answers, Answer: answer}}
		}
		if len(corpus.Cases) == 0 {
			continue
		}
		if qaMode {
			err = corpus.ValidateFixtures()
			if len(corpus.Cases) > 4096 {
				err = errors.New("dataset exceeds 4096 questions per sample")
			}
		} else {
			err = corpus.Validate()
		}
		if err != nil {
			return plan, fmt.Errorf("sample %d: %w", index, err)
		}
		plan.groups = append(plan.groups, corpus)
	}
	if len(plan.groups) == 0 {
		return plan, errors.New("dataset has no evidence-labelled retrieval cases")
	}
	return plan, nil
}

func runDataset(ctx context.Context, schema string, dimension int, path, suite string, maxCases int, command, socket, format, fields, profile string, output io.Writer) error {
	if format != "json" && format != "text" {
		return errors.New("evaluation format must be json or text")
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
	total := memory.EvaluationResult{Suite: suite, Samples: len(plan.groups), ExcludedCases: plan.excluded}
	for index, corpus := range plan.groups {
		var result memory.EvaluationResult
		err = evaluationSession(ctx, schema, dimension, func(db *postgres.EvaluationStore) error {
			var err error
			result, err = memory.EvaluateCorpus(ctx, db, executor, corpus, command)
			return err
		})
		if err != nil {
			return fmt.Errorf("%s sample %d: %w", suite, index, err)
		}
		if result.Status != "ok" || result.Scores.Cases != len(corpus.Cases) || len(result.LatenciesMS) != len(corpus.Cases) {
			return errors.New("dataset evaluation returned a partial denominator")
		}
		n := float64(result.Scores.Cases)
		total.Scores.MRR += result.Scores.MRR * n
		total.Scores.NDCG5 += result.Scores.NDCG5 * n
		total.Scores.NDCG10 += result.Scores.NDCG10 * n
		total.Scores.Recall5 += result.Scores.Recall5 * n
		total.Scores.Recall10 += result.Scores.Recall10 * n
		total.Scores.Cases += result.Scores.Cases
		total.LatenciesMS = append(total.LatenciesMS, result.LatenciesMS...)
	}
	n := float64(total.Scores.Cases)
	total.Scores.MRR /= n
	total.Scores.NDCG5 /= n
	total.Scores.NDCG10 /= n
	total.Scores.Recall5 /= n
	total.Scores.Recall10 /= n
	total.Status = "ok"
	rendered, err := memory.FormatEvaluation(total, path, format, fields, profile)
	if err != nil {
		return err
	}
	_, err = output.Write(rendered)
	return err
}

func datasetAnswer(raw json.RawMessage) (string, error) {
	var text string
	if json.Unmarshal(raw, &text) == nil && strings.TrimSpace(text) != "" {
		return text, nil
	}
	var number json.Number
	if string(raw) != "null" && json.Unmarshal(raw, &number) == nil && number != "" {
		return number.String(), nil
	}
	return "", errors.New("QA question requires a nonempty string or numeric answer")
}
