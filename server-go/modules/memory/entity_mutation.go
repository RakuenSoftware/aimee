package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

var errEntityTransition = errors.New("memory: entity transition no longer applies")

// Decimal strings allow the entire int64 range through native JSON transports.
// Numeric input is limited to the exact range those transports can represent.
func entityMutationID(args commandArgs, key string) (int64, bool) {
	return args.decimalID(key)
}

func handleEntityMutation(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	operation, action := args.stringOr("operation", ""), args.stringOr("action", "")
	console := operation == "entity-review"
	fail := func(code int, kind, message string) ([]byte, bus.ModuleStatus) {
		if console {
			return ontologyHTTP(code, map[string]string{"error": message})
		}
		return ontologyHTTP(code, commandError(kind, message))
	}
	if console {
		caller := options.commandContext
		if caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" {
			return fail(403, "unauthorized", "authenticated operator required")
		}
	}
	request := DataRequest{Operation: operation, State: action, IncludeAll: true}
	var valid bool
	switch action {
	case "merge":
		request.SourceID, valid = entityMutationID(args, "from_id")
		if valid {
			request.TargetID, valid = entityMutationID(args, "into_id")
		}
		if !valid || request.SourceID == request.TargetID {
			return fail(400, "invalid_argument", "distinct positive integer from_id and into_id required")
		}
	case "unmerge":
		request.ID, valid = entityMutationID(args, "merge_id")
		if !valid {
			return fail(400, "invalid_argument", "positive integer merge_id required")
		}
	default:
		return fail(400, "invalid_argument", "action must be merge or unmerge")
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return fail(503, "unavailable", "entity mutation unavailable")
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	var result struct {
		Status  string `json:"status"`
		Message string `json:"message"`
	}
	if json.Unmarshal(response.Payload, &result) != nil {
		return nil, bus.ModuleStatusInternal
	}
	code := 200
	if result.Status == "error" {
		code = 409
		if !console {
			code = 404
		}
	}
	if console && result.Status == "error" {
		return ontologyHTTP(code, map[string]string{"error": result.Message})
	}
	// Marshal RawMessage directly; never round the returned merge ID through the
	// metadata used only to inspect status above.
	return ontologyHTTP(code, response.Payload)
}

// Merge changes only registry resolution, preserving aliases and one-hop
// behavior. Its external graph record remains consumable by native rollback.
func (s *postgresDataStore) mutateEntity(ctx context.Context, actor FactActor, action string, from, into, mergeID int64) (map[string]any, error) {
	if s.placement != PlacementKB || !validMutationActor(actor) || (actor.Rank != 20 && actor.Rank != 40) {
		return nil, errors.New("memory: invalid entity mutation actor")
	}
	if _, ok := s.db.(store.Tx); !ok {
		return nil, errors.New("memory: entity mutation requires transaction")
	}
	if action != "merge" && action != "unmerge" {
		return nil, errEntityTransition
	}
	if action == "merge" && (from <= 0 || into <= 0 || from == into) || action == "unmerge" && mergeID <= 0 {
		return nil, errEntityTransition
	}
	// Shared with ingestion/review: lock before endpoint rows, always in the same
	// order. The row locks also protect against other database mutation paths.
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return nil, err
	}
	if action == "merge" {
		rows, err := s.db.Query(ctx, `SELECT canonical_id,status FROM entity_registry WHERE canonical_id IN($1,$2) ORDER BY canonical_id FOR UPDATE`, from, into)
		if err != nil {
			return nil, err
		}
		active := 0
		for rows.Next() {
			var id int64
			var status string
			if err = rows.Scan(&id, &status); err != nil {
				break
			}
			if status == "active" {
				active++
			}
		}
		if err == nil {
			err = rows.Err()
		}
		rows.Close()
		if err != nil {
			return nil, err
		}
		if active != 2 {
			return nil, errEntityTransition
		}
	} else {
		err := s.db.QueryRow(ctx, `SELECT from_id,into_id FROM entity_merges WHERE id=$1 AND undone=0 FOR UPDATE`, mergeID).Scan(&from, &into)
		if store.IsNoRows(err) {
			return nil, errEntityTransition
		}
		if err != nil {
			return nil, err
		}
	}
	commit, err := s.openFactCommit(ctx, actor, "entity."+action, "")
	if err != nil {
		return nil, err
	}
	before, after := "active", "merged"
	if action == "merge" {
		if err = s.db.QueryRow(ctx, `INSERT INTO entity_merges(from_id,into_id) VALUES($1,$2) RETURNING id`, from, into).Scan(&mergeID); err != nil {
			return nil, err
		}
		tag, e := s.db.Exec(ctx, `UPDATE entity_registry SET status='merged',merged_into=$2 WHERE canonical_id=$1 AND status='active'`, from, into)
		if e != nil {
			return nil, e
		}
		if tag.RowsAffected() != 1 {
			return nil, errEntityTransition
		}
	} else {
		before, after = "merged", "active"
		tag, e := s.db.Exec(ctx, `UPDATE entity_registry SET status='active',merged_into=0 WHERE canonical_id=$1 AND status='merged' AND merged_into=$2`, from, into)
		if e != nil {
			return nil, e
		}
		if tag.RowsAffected() != 1 {
			return nil, errEntityTransition
		}
		tag, e = s.db.Exec(ctx, `UPDATE entity_merges SET undone=1 WHERE id=$1 AND undone=0`, mergeID)
		if e != nil {
			return nil, e
		}
		if tag.RowsAffected() != 1 {
			return nil, errEntityTransition
		}
	}
	key := strconv.FormatInt(mergeID, 10)
	if _, err = s.db.Exec(ctx, `INSERT INTO fact_graph_changes(commit_id,assertion_id,object_kind,object_key,action,existed_before,existed_after,before_lifecycle,after_lifecycle,diff_detail) VALUES($1,0,'entity_merge',$2,$3,1,1,$4,$5,'external graph transition')`, commit, key, action, before, after); err != nil {
		return nil, err
	}
	if err = s.closeFactObjectCommit(ctx, commit, key); err != nil {
		return nil, err
	}
	result := map[string]any{"status": "ok", "ok": true, "merge_id": mergeID, "commit_id": commit}
	if action == "unmerge" {
		result["undone"] = true
	}
	return result, nil
}
