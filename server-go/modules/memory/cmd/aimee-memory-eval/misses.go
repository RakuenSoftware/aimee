package main

import (
	"context"
	"errors"
	"fmt"
	"io"

	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

func runDatasetMisses(ctx context.Context, schema string, dimension int, path, suite string, maxCases int, command, socket, format, fields, profile string, limit, maxMisses int, output io.Writer) error {
	if (format != "json" && format != "text") || limit < 1 || limit > 20 || maxMisses < 1 || maxMisses > 100 {
		return errors.New("miss reporting requires json/text, limit 1..20 and max-misses 1..100")
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
	cases, misses := 0, 0
	buckets := map[string]int{}
	reports := []map[string]any{}
	for index, corpus := range plan.groups {
		err = evaluationSession(ctx, schema, dimension, func(db *postgres.EvaluationStore) error {
			module, err := memory.NewEvaluationModule(ctx, db, executor)
			if err != nil {
				return err
			}
			ids, err := module.Seed(corpus.Fixtures, command)
			if err != nil {
				return err
			}
			for _, row := range corpus.Cases {
				expected := make([]string, len(row.Expected))
				for i, fid := range row.Expected {
					expected[i] = ids[fid]
				}
				var result struct {
					Miss   bool   `json:"is_miss"`
					Rank   int    `json:"rank"`
					Bucket string `json:"bucket"`
					Text   string `json:"text"`
				}
				err = module.Call(memory.StageCommand, "runtime", map[string]any{"operation": "benchmark-miss", "query": row.Query, "expected_ids": expected, "limit": limit, "render": len(reports) < maxMisses}, &result)
				if err != nil {
					return err
				}
				cases++
				if result.Miss {
					misses++
					buckets[result.Bucket]++
					if len(reports) < maxMisses {
						reports = append(reports, map[string]any{"sample": index, "id": row.ID, "query": row.Query, "rank": result.Rank, "bucket": result.Bucket, "text": result.Text})
					}
				}
			}
			return nil
		})
		if err != nil {
			return fmt.Errorf("%s sample %d: %w", suite, index, err)
		}
	}
	return writeEvaluationView(map[string]any{"status": "ok", "suite": suite, "dataset": path, "samples": len(plan.groups), "cases": cases, "misses": misses, "limit": limit, "excluded_cases": plan.excluded, "fixture_policy": "full-text-raw-query-v1", "buckets": buckets, "reports": reports}, format, fields, profile, output)
}
