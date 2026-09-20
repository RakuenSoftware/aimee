package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

const vectorSchemaVersion = "v4"

func handleVerifyCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: "vector-verify", Command: args.stringOr("embedding_command", "")}
	_ = json.Unmarshal(args["detail"], &request.Detail)
	_ = json.Unmarshal(args["timings"], &request.Timings)
	commandScope(args, &request)
	body, _ := json.Marshal(request)
	reply, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(reply, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}

type vectorCollectionStatus struct {
	Collection    string `json:"collection"`
	Exists        bool   `json:"collection_exists"`
	Points        int64  `json:"vector_points"`
	IndexedFields string `json:"indexed_fields"`
	Memories      int64  `json:"db2_memories"`
	Units         int64  `json:"db2_units"`
	Chunks        int64  `json:"db2_chunks"`
	Expected      int64  `json:"expected_points"`
	Drift         int64  `json:"drift"`
}

// Report the catalog's deployed indexes instead of assuming every expected
// field is indexed. Missing collections remain a diagnostic result, while
// permissions and SQL failures remain errors.
func (s *postgresDataStore) vectorCollectionStatus(ctx context.Context, table string, request DataRequest) (vectorCollectionStatus, error) {
	result := vectorCollectionStatus{Collection: table, Points: -1}
	var present bool
	err := s.db.QueryRow(ctx, `SELECT to_regclass($1) IS NOT NULL,EXISTS(SELECT 1 FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam WHERE i.indrelid=to_regclass($1) AND i.indisvalid AND a.amname IN ('hnsw','diskann'))`, table).Scan(&present, &result.Exists)
	if err != nil || !present {
		return result, err
	}
	// table is selected only by the two fixed calls below.
	where := ` WHERE ($1 OR primary_scope='global' OR workspace='_shared' OR ($2<>'' AND workspace=$2) OR ($3<>'' AND project=$3))`
	if table == "kb_embeddings" {
		where = ` WHERE ($1 OR project='' OR project='_shared' OR ($3<>'' AND project=$3)) AND $2::text IS NOT NULL`
	}
	if err = s.db.QueryRow(ctx, `SELECT count(*) FROM `+table+where, request.IncludeAll, request.Workspace, request.Project).Scan(&result.Points); err != nil {
		return result, err
	}
	err = s.db.QueryRow(ctx, `SELECT COALESCE(string_agg(DISTINCT a.attname,',' ORDER BY a.attname),'') FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am am ON am.oid=c.relam JOIN pg_attribute a ON a.attrelid=i.indrelid AND a.attnum=ANY(i.indkey) WHERE i.indrelid=to_regclass($1) AND i.indisvalid AND am.amname='btree'`, table).Scan(&result.IndexedFields)
	return result, err
}

type failedVectorOperation struct {
	PointID    int64  `json:"point_id"`
	Collection string `json:"collection"`
	MemoryID   int64  `json:"memory_id"`
	Attempts   int    `json:"attempts"`
	LastError  string `json:"last_error"`
	UpdatedAt  string `json:"updated_at"`
}

func (s *postgresDataStore) verifyVectors(ctx context.Context, trace uint64, executor egress.Executor, request DataRequest) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	memory, err := s.vectorCollectionStatus(ctx, "memory_embeddings", request)
	if err != nil {
		return nil, err
	}
	kb, err := s.vectorCollectionStatus(ctx, "kb_embeddings", request)
	if err != nil {
		return nil, err
	}
	var active, stored string
	var lockHeld bool
	err = s.db.QueryRow(ctx, `SELECT (SELECT count(*) FROM memories),(SELECT count(*) FROM memory_units u JOIN memories m ON m.id=u.memory_id),(SELECT count(*) FROM kb_documents WHERE $3 OR project='' OR project='_shared' OR ($4<>'' AND project=$4)),COALESCE((SELECT version FROM memory_active_embedder WHERE id=1),''),COALESCE((SELECT value FROM kb_meta WHERE key='vector_schema_version'),''),EXISTS(SELECT 1 FROM pg_locks WHERE locktype='advisory' AND granted AND classid=$1::oid AND objid=$2::oid AND objsubid=1 AND database=(SELECT oid FROM pg_database WHERE datname=current_database()))`, uint32(uint64(vectorRebuildLock)>>32), uint32(uint64(vectorRebuildLock)&0xffffffff), request.IncludeAll, request.Project).Scan(&memory.Memories, &memory.Units, &kb.Chunks, &active, &stored, &lockHeld)
	if err != nil {
		return nil, err
	}
	memory.Expected = memory.Memories + memory.Units
	kb.Expected = kb.Chunks
	if memory.Points >= 0 {
		memory.Drift = memory.Points - memory.Expected
	}
	if kb.Points >= 0 {
		kb.Drift = kb.Points - kb.Expected
	}
	var pending, failed, ok, stuck int64
	err = s.db.QueryRow(ctx, `SELECT count(*) FILTER(WHERE status='pending'),count(*) FILTER(WHERE status='failed'),count(*) FILTER(WHERE status='ok'),count(*) FILTER(WHERE status='failed' AND attempts >= $1) FROM vector_index_ops v WHERE $2 OR EXISTS(SELECT 1 FROM memories m WHERE m.id=v.memory_id)`, vectorRetryLimit(), request.IncludeAll).Scan(&pending, &failed, &ok, &stuck)
	if err != nil {
		return nil, err
	}
	var samples int64
	var p50, p90, p95, p99, maximum *float64
	err = s.db.QueryRow(ctx, `SELECT count(*),percentile_cont(0.5) WITHIN GROUP(ORDER BY lag),percentile_cont(0.9) WITHIN GROUP(ORDER BY lag),percentile_cont(0.95) WITHIN GROUP(ORDER BY lag),percentile_cont(0.99) WITHIN GROUP(ORDER BY lag),max(lag) FROM (
 SELECT extract(epoch FROM(v.indexed_at::timestamp-m.created_at::timestamp)) AS lag FROM vector_index_ops v JOIN memories m ON m.id=v.memory_id
 WHERE v.status='ok' AND NULLIF(v.indexed_at,'') IS NOT NULL AND m.created_at<>'' AND v.indexed_at::timestamp >= CURRENT_TIMESTAMP-INTERVAL '7 days') s WHERE lag>=0`).Scan(&samples, &p50, &p90, &p95, &p99, &maximum)
	if err != nil {
		return nil, err
	}
	lag := map[string]any{"samples": samples}
	if samples == 0 {
		lag["state"] = "unmeasured"
	} else {
		lag["p50_secs"], lag["p90_secs"], lag["p95_secs"], lag["p99_secs"], lag["max_secs"] = p50, p90, p95, p99, maximum
	}
	command, err := s.embeddingCommand(request.Command)
	if err != nil {
		return nil, err
	}
	embedded := Embed(ctx, trace, executor, EmbedRequest{BaseURL: command, InputType: "document", Text: "probe", MaxDim: 4000})
	dimension, dimErr := s.vectorDimension(ctx)
	if memory.Exists && dimErr != nil {
		return nil, dimErr
	}
	schemaMatch := stored == vectorSchemaVersion || stored == "" && memory.Expected == 0 && kb.Chunks == 0
	dimMatches := embedded.Dim > 0 && embedded.Dim == dimension && !embedded.Truncated
	missing := func(actual string, required []string) int {
		fields := strings.Split(actual, ",")
		n := 0
		for _, field := range required {
			found := false
			for _, present := range fields {
				found = found || present == field
			}
			if !found {
				n++
			}
		}
		return n
	}
	result := map[string]any{"status": "ok", "server_version": "pgvector", "active_embedder_version": active,
		"memory": memory, "kb": kb, "schema": map[string]any{"expected": vectorSchemaVersion, "stored": stored, "match": schemaMatch}, "rebuild_lock_held": lockHeld,
		"embedder":        map[string]any{"dim": embedded.Dim, "expected_dim": dimension, "ok": dimMatches, "unauthorized": embedded.Unauthorized, "unavailable": embedded.Unavailable, "error": embedded.Error},
		"index_ops":       map[string]any{"ok": ok, "pending": pending, "failed": failed, "stuck": stuck, "write_to_readable_lag": lag},
		"payload_indexes": map[string]any{"memory_missing": missing(memory.IndexedFields, []string{"record_type", "primary_scope", "workspace", "project"}), "kb_missing": missing(kb.IndexedFields, []string{"project"})},
		"ok":              memory.Exists && kb.Exists && memory.Drift == 0 && kb.Drift == 0 && failed == 0 && stuck == 0 && schemaMatch && dimMatches}
	if request.Detail {
		rows, err := s.db.Query(ctx, `SELECT point_id,collection,COALESCE(memory_id,0),attempts,COALESCE(last_error,''),COALESCE(updated_at,'') FROM vector_index_ops v WHERE status='failed' AND ($1 OR EXISTS(SELECT 1 FROM memories m WHERE m.id=v.memory_id)) ORDER BY attempts DESC,updated_at DESC,point_id LIMIT 20`, request.IncludeAll)
		if err != nil {
			return nil, err
		}
		details := []failedVectorOperation{}
		for rows.Next() {
			var row failedVectorOperation
			if err := rows.Scan(&row.PointID, &row.Collection, &row.MemoryID, &row.Attempts, &row.LastError, &row.UpdatedAt); err != nil {
				rows.Close()
				return nil, err
			}
			details = append(details, row)
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return nil, err
		}
		result["failed_ops"] = details
	}
	if request.Timings {
		trials := 0
		var total, maxUS int64
		if memory.Exists && dimMatches {
			for i := 0; i < 10 && ctx.Err() == nil; i++ {
				probe := Embed(ctx, trace, executor, EmbedRequest{BaseURL: command, InputType: "query", Text: fmt.Sprintf("probe query %d", i), MaxDim: 4000})
				if probe.Dim != dimension || probe.Truncated {
					continue
				}
				vector := make([]float64, len(probe.Vector))
				for j, v := range probe.Vector {
					vector[j] = float64(v)
				}
				start := time.Now()
				_, err := s.SearchVectors(ctx, vector, "memory", request.Workspace, request.Project, request.IncludeAll, 5)
				elapsed := time.Since(start).Microseconds()
				if err != nil {
					return nil, err
				}
				trials++
				total += elapsed
				if elapsed > maxUS {
					maxUS = elapsed
				}
			}
		}
		result["timings"] = map[string]any{"trials": trials, "total_us": total, "max_us": maxUS}
	}
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	return json.Marshal(result)
}
