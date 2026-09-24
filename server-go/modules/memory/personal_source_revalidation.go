package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Private references are checked only against the process's own private store.
// No identifier or payload needs to cross the shared KB boundary. All roots,
// revisions and current eligibility use one statement snapshot.
func (s *postgresDataStore) revalidatePersonalSources(ctx context.Context, request *sourceRevalidation) (bool, error) {
	if s.placement != PlacementServer || !request.valid() {
		return false, errors.New("memory: invalid private source revalidation")
	}
	for _, ref := range request.Sources {
		if ref.Source.Kind != "user_memory_record" {
			return false, errors.New("memory: private source owner required")
		}
	}
	raw, err := json.Marshal(request.Sources)
	if err != nil {
		return false, err
	}
	var count int
	query := `SELECT count(*) FROM jsonb_array_elements($1::jsonb) r(ref)
 WHERE r.ref#>>'{source_version,version,owner_id}'=(SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1)
 AND EXISTS (SELECT 1 FROM user_memories m
 WHERE m.id=(r.ref->>'stable_id')::bigint
 AND m.record_revision::text=r.ref#>>'{source_version,version,record_revision}'
 AND m.lifecycle_state=CASE WHEN r.ref->>'channel'='native_open_commitments' THEN 'pending' ELSE 'active' END
 AND (m.valid_until IS NULL OR m.valid_until>now()))`
	if request.SendGuard == "acquire" {
		query = strings.ReplaceAll(query, "now()", "clock_timestamp()")
	}
	err = s.db.QueryRow(ctx, query, string(raw)).Scan(&count)
	if err == nil && count == len(request.Sources) && request.SendGuard == "acquire" {
		query = strings.ReplaceAll(query, "clock_timestamp()", "(clock_timestamp()+interval '5 seconds')")
		err = s.db.QueryRow(ctx, query, string(raw)).Scan(&count)
	}
	return err == nil && count == len(request.Sources), err
}

func handlePersonalSourceRevalidation(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || options.placement != PlacementServer {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	request := DataRequest{Operation: "personal-source-revalidate"}
	if json.Unmarshal(args["revalidation"], &request.Revalidation) != nil || !request.Revalidation.valid() {
		return commandResult(commandError("invalid_argument", "invalid private source revalidation"))
	}
	raw, _ := json.Marshal(request)
	result, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "private source owner unavailable"))
	}
	var response DataResponse
	if json.Unmarshal(result, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
