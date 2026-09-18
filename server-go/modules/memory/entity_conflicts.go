package memory

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// The former internal registry queue is a private host operation, not a public
// RPC. Re-observing an ambiguity increments priority without reopening it.
func handleEntityConflicts(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	request := DataRequest{Operation: "entity-conflicts", State: args.stringOr("action", ""), Entity: args.stringOr("name", ""), Kind: args.stringOr("status", "")}
	switch request.State {
	case "record", "priority":
		request.Entity = entityAliasName(request.Entity)
		if request.Entity == "" || len(request.Entity) > 255 {
			return commandResult(commandError("invalid_argument", "invalid entity name"))
		}
	case "status":
		var valid bool
		request.ID, valid = entityMutationID(args, "id")
		if !valid || (request.Kind != "open" && request.Kind != "resolved" && request.Kind != "failed") {
			return commandResult(commandError("invalid_argument", "positive id and open/resolved/failed status required"))
		}
	case "count":
		// A nonmatching status returns zero, as in the former internal API.
	default:
		return commandResult(commandError("invalid_argument", "unknown entity conflict action"))
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}

func (s *postgresDataStore) entityConflicts(ctx context.Context, request DataRequest) (map[string]any, error) {
	if s.placement != PlacementKB {
		return nil, errors.New("memory: entity conflicts require KB placement")
	}
	if _, ok := s.db.(store.Tx); !ok {
		return nil, errors.New("memory: entity conflicts require transaction")
	}
	result := map[string]any{"status": "ok"}
	name := entityAliasName(request.Entity)
	switch request.State {
	case "record", "priority":
		if name == "" || len(name) > 255 {
			return nil, errors.New("memory: invalid entity name")
		}
		if request.State == "record" {
			var id int64
			err := s.db.QueryRow(ctx, `INSERT INTO entity_name_conflicts(name_norm,status,priority) VALUES($1,'open',1) ON CONFLICT(name_norm) DO UPDATE SET priority=entity_name_conflicts.priority+1 RETURNING id`, name).Scan(&id)
			if err != nil {
				return nil, err
			}
			result["id"] = id
		} else {
			var priority int64
			err := s.db.QueryRow(ctx, `SELECT priority FROM entity_name_conflicts WHERE name_norm=$1`, name).Scan(&priority)
			if store.IsNoRows(err) {
				result["found"] = false
				result["priority"] = int64(-1)
			} else if err != nil {
				return nil, err
			} else {
				result["found"] = true
				result["priority"] = priority
			}
		}
	case "status":
		if request.ID <= 0 || (request.Kind != "open" && request.Kind != "resolved" && request.Kind != "failed") {
			return nil, errors.New("memory: invalid entity conflict status")
		}
		tag, err := s.db.Exec(ctx, `UPDATE entity_name_conflicts SET status=$2 WHERE id=$1`, request.ID, request.Kind)
		if err != nil {
			return nil, err
		}
		result["updated"] = tag.RowsAffected() == 1
	case "count":
		var count int64
		if err := s.db.QueryRow(ctx, `SELECT count(*) FROM entity_name_conflicts WHERE $1='' OR status=$1`, request.Kind).Scan(&count); err != nil {
			return nil, err
		}
		result["count"] = count
	default:
		return nil, errors.New("memory: unknown entity conflict action")
	}
	return result, nil
}
