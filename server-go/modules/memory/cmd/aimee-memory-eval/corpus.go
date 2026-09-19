package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

func readEvaluationJSON(path string, target any) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 4<<20+1))
	if err != nil {
		return err
	}
	if len(data) > 4<<20 {
		return errors.New("evaluation file exceeds 4 MiB")
	}
	return json.Unmarshal(data, target)
}

type evaluationBaseline struct {
	Manifest    memory.EvaluationManifest     `json:"manifest"`
	CaseResults []memory.EvaluationCaseResult `json:"case_results"`
	memory.EvaluationScores
	Threshold float64 `json:"threshold_pct"`
}

func checkBaseline(result memory.EvaluationResult, baseline evaluationBaseline) error {
	scores := result.Scores
	if result.Manifest == nil {
		return errors.New("evaluation result has no manifest")
	}
	if err := result.Manifest.Validate(); err != nil {
		return err
	}
	if err := baseline.Manifest.Validate(); err != nil {
		return err
	}
	got, _ := json.Marshal(result.Manifest)
	want, _ := json.Marshal(baseline.Manifest)
	if !bytes.Equal(got, want) {
		return errors.New("evaluation baseline input/embedding/policy manifest mismatch")
	}
	if len(result.Manifest.CaseIDs) != scores.Cases || len(baseline.CaseResults) != scores.Cases || len(result.Cases) != scores.Cases {
		return errors.New("evaluation baseline lacks complete per-case receipts")
	}
	for i, id := range result.Manifest.CaseIDs {
		if result.Cases[i].ID != id || baseline.CaseResults[i].ID != id {
			return errors.New("evaluation baseline case receipt mismatch")
		}
	}

	if err := validateCaseReceipts(result.Cases, result.Scores); err != nil {
		return err
	}
	if err := validateCaseReceipts(baseline.CaseResults, baseline.EvaluationScores); err != nil {
		return err
	}
	for i, row := range result.Cases {
		got, _ := json.Marshal(row.Expected)
		want, _ := json.Marshal(baseline.CaseResults[i].Expected)
		if !bytes.Equal(got, want) {
			return errors.New("evaluation relevance labels differ from baseline")
		}
	}
	if baseline.Cases != scores.Cases || baseline.Cases < 1 || baseline.Threshold < 0 || baseline.Threshold > 100 || math.IsNaN(baseline.Threshold) {
		return errors.New("evaluation baseline has a different case denominator or invalid threshold")
	}
	for _, metric := range []struct {
		name      string
		got, want float64
	}{
		{"mrr", scores.MRR, baseline.MRR}, {"ndcg_5", scores.NDCG5, baseline.NDCG5},
		{"ndcg_10", scores.NDCG10, baseline.NDCG10}, {"recall_5", scores.Recall5, baseline.Recall5}, {"recall_10", scores.Recall10, baseline.Recall10},
	} {
		if math.IsNaN(metric.got) || math.IsInf(metric.got, 0) || metric.got < 0 || metric.got > 1 || math.IsNaN(metric.want) || math.IsInf(metric.want, 0) || metric.want < 0 || metric.want > 1 {
			return fmt.Errorf("invalid baseline %s", metric.name)
		}
		if metric.got < metric.want*(1-baseline.Threshold/100) {
			return fmt.Errorf("evaluation regression: %s %.6f below baseline %.6f (%.1f%% tolerance)", metric.name, metric.got, metric.want, baseline.Threshold)
		}
	}
	return nil
}
func validateCaseReceipts(rows []memory.EvaluationCaseResult, total memory.EvaluationScores) error {
	if len(rows) != total.Cases || len(rows) == 0 {
		return errors.New("evaluation receipt denominator mismatch")
	}
	sums := [5]float64{}
	for _, row := range rows {
		if row.Scores.Cases != 1 || len(row.Expected) == 0 || len(row.Retrieved) > 20 || math.IsNaN(row.LatencyMS) || math.IsInf(row.LatencyMS, 0) || row.LatencyMS < 0 {
			return errors.New("invalid evaluation case receipt")
		}
		for _, ids := range [][]string{row.Expected, row.Retrieved} {
			seen := map[string]bool{}
			for _, id := range ids {
				if id == "" || seen[id] {
					return errors.New("invalid evaluation receipt fixture IDs")
				}
				seen[id] = true
			}
		}
		for i, value := range []float64{row.Scores.MRR, row.Scores.NDCG5, row.Scores.NDCG10, row.Scores.Recall5, row.Scores.Recall10} {
			if math.IsNaN(value) || math.IsInf(value, 0) || value < 0 || value > 1 {
				return errors.New("invalid evaluation receipt score")
			}
			sums[i] += value
		}
	}
	for i, value := range []float64{total.MRR, total.NDCG5, total.NDCG10, total.Recall5, total.Recall10} {
		if math.IsNaN(value) || math.IsInf(value, 0) || math.Abs(value-sums[i]/float64(len(rows))) > 1e-9 {
			return errors.New("evaluation aggregate disagrees with case receipts")
		}
	}
	return nil
}

func writeBaseline(path string, result memory.EvaluationResult) (err error) {
	if result.Manifest == nil {
		return errors.New("evaluation result has no manifest")
	}
	baseline := evaluationBaseline{Manifest: *result.Manifest, CaseResults: result.Cases, EvaluationScores: result.Scores, Threshold: 5}
	if err := checkBaseline(result, baseline); err != nil {
		return err
	}
	payload, err := json.MarshalIndent(baseline, "", "  ")
	if err != nil {
		return err
	}
	if len(payload)+1 > 4<<20 {
		return errors.New("evaluation baseline exceeds 4 MiB")
	}

	f, err := os.CreateTemp(filepath.Dir(path), ".memory-baseline-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	_, err = f.Write(append(payload, '\n'))
	if err == nil {
		err = f.Sync()
	}
	err = errors.Join(err, f.Close())
	if err != nil {
		return err
	}
	return os.Rename(f.Name(), path)
}

func runCorpus(ctx context.Context, schema string, dimension int, path, command, socket, baselinePath string, update bool, format, fields, profile string, output io.Writer) error {
	if format != "json" && format != "text" {
		return errors.New("evaluation format must be json or text")
	}
	if update && baselinePath == "" {
		return errors.New("update-baseline requires a baseline path")
	}
	var corpus memory.EvaluationCorpus
	if err := readEvaluationJSON(path, &corpus); err != nil {
		return err
	}
	if err := corpus.Validate(); err != nil {
		return err
	}
	if command == "" {
		return errors.New("corpus evaluation requires an embedding command")
	}
	executor, closeExecutor, err := evaluationExecutor(ctx, command, socket)
	if err != nil {
		return err
	}
	defer closeExecutor()
	var baseline evaluationBaseline
	if baselinePath != "" && !update {
		if err := readEvaluationJSON(baselinePath, &baseline); err != nil {
			return err
		}
	}
	var result memory.EvaluationResult
	// Close the isolated database before publishing scores or replacing a baseline.
	err = evaluationSessionSnapshot(ctx, schema, dimension, func(db *postgres.EvaluationStore, schemaSnapshot []byte) error {
		var err error
		result, err = memory.EvaluateCorpus(ctx, db, executor, corpus, command)
		if err == nil {
			result.Manifest.SchemaSHA256 = memory.EvaluationDigest(schemaSnapshot)
		}
		return err
	})
	if err != nil {
		return err
	}
	rendered, err := memory.FormatEvaluation(result, path, format, fields, profile)
	if err != nil {
		return err
	}
	if baselinePath != "" {
		if update {
			err = writeBaseline(baselinePath, result)
		} else {
			err = checkBaseline(result, baseline)
		}
		if err != nil {
			return err
		}
	}
	_, err = output.Write(rendered)
	return err
}

func evaluationExecutor(ctx context.Context, command, socket string) (egress.Executor, func(), error) {
	if strings.TrimSpace(command) == "" {
		return nil, nil, errors.New("evaluation requires an embedding command")
	}
	if !memory.EmbedIsHTTP(command) {
		return nil, func() {}, nil
	}
	if socket == "" {
		return nil, nil, errors.New("HTTP embedding requires a module bus socket for governed egress")
	}
	client, err := bus.ConnectClient(ctx, socket, 1, egress.MemoryClientRef)
	if err != nil {
		return nil, nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		return nil, nil, err
	}
	cleanup := func() { caller.CloseAndWait(); client.Detach() }
	executor, err := egress.NewBusAuthorizer(caller)
	if err != nil {
		cleanup()
		return nil, nil, err
	}
	return executor, cleanup, nil
}
