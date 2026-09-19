package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"

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
	memory.EvaluationScores
	Threshold float64 `json:"threshold_pct"`
}

func checkBaseline(scores memory.EvaluationScores, baseline evaluationBaseline) error {
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
		if math.IsNaN(metric.want) || math.IsInf(metric.want, 0) || metric.want < 0 || metric.want > 1 {
			return fmt.Errorf("invalid baseline %s", metric.name)
		}
		if metric.got < metric.want*(1-baseline.Threshold/100) {
			return fmt.Errorf("evaluation regression: %s %.6f below baseline %.6f (%.1f%% tolerance)", metric.name, metric.got, metric.want, baseline.Threshold)
		}
	}
	return nil
}
func writeBaseline(path string, scores memory.EvaluationScores) (err error) {
	f, err := os.CreateTemp(filepath.Dir(path), ".memory-baseline-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	encoder := json.NewEncoder(f)
	encoder.SetIndent("", "  ")
	err = encoder.Encode(evaluationBaseline{scores, 5})
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
	var executor egress.Executor
	if memory.EmbedIsHTTP(command) {
		if socket == "" {
			return errors.New("HTTP embedding requires a module bus socket for governed egress")
		}
		client, err := bus.ConnectClient(ctx, socket, 1, egress.MemoryClientRef)
		if err != nil {
			return err
		}
		caller, err := bus.NewConcurrentModuleCaller(ctx, client)
		if err != nil {
			client.Detach()
			return err
		}
		defer func() { caller.CloseAndWait(); client.Detach() }()
		executor, err = egress.NewBusAuthorizer(caller)
		if err != nil {
			return err
		}
	}
	var baseline evaluationBaseline
	if baselinePath != "" && !update {
		if err := readEvaluationJSON(baselinePath, &baseline); err != nil {
			return err
		}
	}
	var result memory.EvaluationResult
	// Close the isolated database before publishing scores or replacing a baseline.
	err := evaluationSession(ctx, schema, dimension, func(db *postgres.EvaluationStore) error {
		var err error
		result, err = memory.EvaluateCorpus(ctx, db, executor, corpus, command)
		return err
	})
	if err != nil {
		return err
	}
	if baselinePath != "" {
		if update {
			err = writeBaseline(baselinePath, result.Scores)
		} else {
			err = checkBaseline(result.Scores, baseline)
		}
		if err != nil {
			return err
		}
	}
	rendered, err := memory.FormatEvaluation(result, path, format, fields, profile)
	if err != nil {
		return err
	}
	_, err = output.Write(rendered)
	return err
}
