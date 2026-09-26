package memory

import (
	"context"
	"encoding/json"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// Reuse the canonical scoped MR-04 owner in its repeatable-read transaction.
// Accept a family only when that snapshot contains the exact recalled revision.
// This initial native path covers personal roots with one established family;
// shared, historical, multi-origin and incomplete ancestry remain unknown.
func healthFamilyFromEvidence(raw json.RawMessage, source *typedSourceVersion) string {
	if source == nil || source.Kind != "user_memory_record" {
		return ""
	}
	var e struct {
		Status   string   `json:"status"`
		Owner    string   `json:"owner_id"`
		Record   string   `json:"record_id"`
		State    string   `json:"lineage_state"`
		Unknown  *int     `json:"unknown_origin_count"`
		Families []string `json:"origin_families"`
		Evidence []struct {
			ID       string `json:"record_id"`
			Revision string `json:"record_revision"`
		} `json:"evidence"`
	}
	if json.Unmarshal(raw, &e) != nil || e.Status != "ok" || e.Owner != source.Version.OwnerID || e.Record != source.Version.RecordID || e.State != "complete" || e.Unknown == nil || *e.Unknown != 0 || len(e.Families) != 1 || !strings.HasPrefix(e.Families[0], "sha256:") || !receiptDigestValid(strings.TrimPrefix(e.Families[0], "sha256:")) {
		return ""
	}
	for _, v := range e.Evidence {
		if v.ID == source.Version.RecordID && v.Revision == source.Version.RecordRevision {
			return e.Families[0]
		}
	}
	return ""
}

func prepareHealthFamilyCapture(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, operation string) {
	delete(args, "_health_records")
	if operation != "native-source-release" || os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" || options.placement != PlacementServer || invocation.PrincipalRef != 0 {
		return
	}
	principal := args.stringOr("principal", "")
	if principal == "" {
		return
	}
	if caller := options.commandContext; caller != nil && (!caller.Authenticated || caller.Principal != principal) {
		return
	}
	var p nativeRecallProjection
	if json.Unmarshal(args["native_projection"], &p) != nil || len(p.HealthRecords) == 0 || len(p.HealthRecords) > 256 {
		return
	}
	sources := map[string]*typedSourceVersion{}
	for _, ref := range p.Sources {
		if ref.Source != nil && ref.Source.Kind == "user_memory_record" && (ref.Source.ReadPolicy == nil || !ref.Source.ReadPolicy.Historical && ref.Source.ReadPolicy.ValidAt == "" && ref.Source.ReadPolicy.BelievedAt == "") {
			sources[healthVersionKey(healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision})] = ref.Source
		}
	}
	budget := invocation.Remaining(50 * time.Millisecond)
	if budget <= 0 {
		return
	}
	parent := options.dataContext
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, budget)
	defer cancel()
	options.dataContext = ctx
	attempted := 0
	for i := range p.HealthRecords {
		row := &p.HealthRecords[i]
		row.Family = ""
		source := sources[healthVersionKey(*row)]
		if source == nil || attempted >= 16 || ctx.Err() != nil {
			continue
		}
		id, err := strconv.ParseInt(source.Version.RecordID, 10, 64)
		if err != nil || id <= 0 {
			continue
		}
		attempted++
		request, _ := json.Marshal(DataRequest{Operation: "evidence", ID: id, Scope: Scope{Type: ScopeUser, Value: "_user"}, Project: args.stringOr("project", ""), Workspace: args.stringOr("workspace", "")})
		raw, status := handleData(options, invocation, request)
		var response DataResponse
		if status == bus.ModuleStatusOK && json.Unmarshal(raw, &response) == nil {
			row.Family = healthFamilyFromEvidence(response.Payload, source)
		}
	}
	args["_health_records"], _ = json.Marshal(p.HealthRecords)
}
