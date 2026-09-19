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
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

type corpusExecutor struct {
	queries, documents int
	failQuery          bool
	blockQuery         chan struct{}
}

func (e *corpusExecutor) Do(ctx context.Context, _ uint64, request egress.HTTPRequest) (egress.HTTPResponse, error) {
	if strings.HasSuffix(request.TargetURL, "/health") {
		return egress.HTTPResponse{Status: 200, Body: []byte(`{"serving_id":"corpus-test"}`)}, nil
	}

	if strings.Contains(request.TargetURL, "input_type=query") {
		e.queries++
		if e.blockQuery != nil {
			close(e.blockQuery)
			<-ctx.Done()
			return egress.HTTPResponse{}, ctx.Err()
		}
		if e.failQuery {
			return egress.HTTPResponse{}, errors.New("query embedder failed")
		}
	} else {
		e.documents++
	}
	return egress.HTTPResponse{Status: 200, Body: []byte(`[1,0,0]`)}, nil
}
func validCorpus() memory.EvaluationCorpus {
	return memory.EvaluationCorpus{Version: 1, Fixtures: []memory.EvaluationFixture{{FID: "opaque", Tier: "L2", Kind: "fact", Key: "opaque", Content: "unrelated fixture payload"}}, Cases: []memory.EvaluationCase{{ID: "semantic", Query: "where is the nebula", Expected: []string{"opaque"}}}}
}
func TestCorpusValidation(t *testing.T) {
	for _, mutate := range []func(*memory.EvaluationCorpus){
		func(c *memory.EvaluationCorpus) { c.Version = 2 },
		func(c *memory.EvaluationCorpus) { c.Fixtures = nil },
		func(c *memory.EvaluationCorpus) { c.Cases = nil },
		func(c *memory.EvaluationCorpus) { c.Fixtures = append(c.Fixtures, c.Fixtures[0]) },
		func(c *memory.EvaluationCorpus) { c.Cases[0].Expected = []string{"missing"} },
		func(c *memory.EvaluationCorpus) { c.Cases[0].Expected = []string{"opaque", "opaque"} },
		func(c *memory.EvaluationCorpus) { c.Cases[0].Query = " " },
		func(c *memory.EvaluationCorpus) { c.Fixtures[0].Content = "" },
	} {
		corpus := validCorpus()
		mutate(&corpus)
		if corpus.Validate() == nil {
			t.Fatal("accepted malformed corpus", corpus)
		}
	}
	corpus := validCorpus()
	corpus.Fixtures[0].FID = strings.Repeat("長い", 100)
	corpus.Cases[0].Expected = []string{corpus.Fixtures[0].FID}
	if err := corpus.Validate(); err != nil {
		t.Fatal("full fixture IDs must not truncate", err)
	}
}
func TestCorpusIsolatedSemanticReplay(t *testing.T) {
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	const schema = "../../../../../src/modules/db2/c/schema.sql"
	for _, fail := range []bool{false, true} {
		executor := &corpusExecutor{failQuery: fail}
		err := evaluationSession(context.Background(), schema, 3, func(db *postgres.EvaluationStore) error {
			var before int64
			if err := db.QueryRow(context.Background(), `SELECT count(*) FROM memories`).Scan(&before); err != nil || before != 0 {
				t.Fatal("evaluation reused prior fixtures", before, err)
			}
			result, err := memory.EvaluateCorpus(context.Background(), db, executor, validCorpus(), "http://corpus-test")
			if fail {
				if err == nil || result.Status == "ok" {
					t.Fatal("query failure certified lexical fallback", result, err)
				}
				return nil
			}
			if err != nil {
				return err
			}
			if result.Status != "ok" || result.Scores.Cases != 1 || result.Scores.MRR != 1 || result.Scores.Recall10 != 1 || len(result.LatenciesMS) != 1 {
				t.Fatal(result)
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
		if executor.queries == 0 || executor.documents == 0 {
			t.Fatal("did not exercise document and query embedding", executor)
		}
	}
}
func TestBaselineAtomicAndDenominator(t *testing.T) {
	path := filepath.Join(t.TempDir(), "baseline.json")
	scores := memory.EvaluationScores{MRR: 1, NDCG5: 1, NDCG10: 1, Recall5: 1, Recall10: 1, Cases: 1}
	if err := writeBaseline(path, scores); err != nil {
		t.Fatal(err)
	}
	var baseline evaluationBaseline
	if err := readEvaluationJSON(path, &baseline); err != nil {
		t.Fatal(err)
	}
	if err := checkBaseline(scores, baseline); err != nil {
		t.Fatal(err)
	}
	scores.Cases = 2
	if checkBaseline(scores, baseline) == nil {
		t.Fatal("changed denominator passed")
	}
	scores.Cases = 1
	scores.MRR = .5
	if checkBaseline(scores, baseline) == nil {
		t.Fatal("regression passed")
	}
}

func TestCorpusCommandReplayAndBaselineFailure(t *testing.T) {
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	directory := t.TempDir()
	corpus := validCorpus()
	prefix := strings.Repeat("shared-prefix-", 10)
	corpus.Fixtures[0].FID = prefix + "one"
	corpus.Fixtures = append(corpus.Fixtures, memory.EvaluationFixture{FID: prefix + "two", Tier: "L2", Kind: "fact", Key: "another", Content: "another opaque fixture"})
	corpus.Cases[0].Expected = []string{prefix + "one", prefix + "two"}
	corpus.Cases = append(corpus.Cases, memory.EvaluationCase{ID: "exact", Query: "another", Expected: []string{prefix + "two"}})
	path := filepath.Join(directory, "corpus.json")
	raw, _ := json.Marshal(corpus)
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(directory, "embed.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ncat >/dev/null\nprintf '[1,0,0]\\n'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	baseline := filepath.Join(directory, "baseline.json")
	var output bytes.Buffer
	schema := "../../../../../src/modules/db2/c/schema.sql"
	if err := runCorpus(context.Background(), schema, 3, path, script, "", baseline, true, "json", "metrics", "compact", &output); err != nil {
		t.Fatal(err)
	}
	var decoded map[string]json.RawMessage
	if err := json.Unmarshal(output.Bytes(), &decoded); err != nil || len(decoded) != 2 || string(decoded["status"]) != `"ok"` || !strings.Contains(string(decoded["metrics"]), `"cases":2`) {
		t.Fatal(output.String(), err)
	}
	original, err := os.ReadFile(baseline)
	if err != nil {
		t.Fatal(err)
	}
	output.Reset()
	if err := runCorpus(context.Background(), schema, 3, path, "printf '[1,0]'", "", baseline, true, "json", "", "", &output); err == nil {
		t.Fatal("failed embedder passed")
	}
	after, _ := os.ReadFile(baseline)
	if !bytes.Equal(original, after) || output.Len() != 0 {
		t.Fatal("failed evaluation changed baseline or published scores")
	}
}

func TestCorpusReadErrors(t *testing.T) {
	path := filepath.Join(t.TempDir(), "corpus.json")
	for _, raw := range []string{"{broken", strings.Repeat("x", 4<<20+1)} {
		if err := os.WriteFile(path, []byte(raw), 0600); err != nil {
			t.Fatal(err)
		}
		var corpus memory.EvaluationCorpus
		if readEvaluationJSON(path, &corpus) == nil {
			t.Fatal("invalid input accepted")
		}
	}
	var corpus memory.EvaluationCorpus
	if readEvaluationJSON(path+"missing", &corpus) == nil {
		t.Fatal("missing corpus accepted")
	}
}

func TestCorpusCancellationStopsQueryEmbedding(t *testing.T) {
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		t.Skip("requires disposable PostgreSQL")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	executor := &corpusExecutor{blockQuery: make(chan struct{})}
	done := make(chan struct{})
	go func() {
		defer close(done)
		select {
		case <-executor.blockQuery:
			cancel()
		case <-ctx.Done():
		}
	}()
	started := time.Now()
	err := evaluationSession(ctx, "../../../../../src/modules/db2/c/schema.sql", 3, func(db *postgres.EvaluationStore) error {
		result, err := memory.EvaluateCorpus(ctx, db, executor, validCorpus(), "http://corpus-test")
		if err == nil || result.Status == "ok" {
			t.Error("cancelled query produced valid evaluation", result, err)
		}
		return err
	})
	cancel()
	<-done
	if err == nil || executor.queries != 1 || time.Since(started) > 3*time.Second {
		t.Fatal("cancellation was not propagated", err, executor.queries, time.Since(started))
	}
}
