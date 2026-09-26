package memory

import (
	"context"
	"encoding/json"
	"os"
	"sort"
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
	families := healthFamiliesFromEvidence(raw, source)
	if len(families) == 1 {
		return families[0]
	}
	return ""
}

func healthFamiliesFromEvidence(raw json.RawMessage, source *typedSourceVersion) []string {
	if source == nil || (source.Kind != "user_memory_record" && source.Kind != "memory_record") {
		return nil
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
	if json.Unmarshal(raw, &e) != nil || e.Status != "ok" || e.Owner != source.Version.OwnerID || e.Record != source.Version.RecordID || e.State != "complete" || e.Unknown == nil || *e.Unknown != 0 {
		return nil
	}
	families := validHealthFamilies(e.Families)
	for _, v := range e.Evidence {
		if v.ID == source.Version.RecordID && v.Revision == source.Version.RecordRevision {
			return families
		}
	}
	return nil
}
func validHealthFamilies(families []string) []string {
	if len(families) == 0 || len(families) > 16 {
		return nil
	}
	result := []string{}
	seen := map[string]bool{}
	for _, family := range families {
		if !strings.HasPrefix(family, "sha256:") || !receiptDigestValid(strings.TrimPrefix(family, "sha256:")) {
			return nil
		}
		if !seen[family] {
			result = append(result, family)
			seen[family] = true
		}
	}
	sort.Strings(result)
	return result
}
func healthRecordFamilies(record healthRecord) []string {
	if len(record.Families) > 0 {
		return validHealthFamilies(record.Families)
	}
	// Legacy internal calculator fixtures and previously stored single-family
	// evidence keep their identity; new multi-origin producers validate digests.
	if record.Family != "" {
		return []string{record.Family}
	}
	return nil
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
		if ref.Source != nil && (ref.Source.Kind == "user_memory_record" || ref.Source.Kind == "memory_record") && (ref.Source.ReadPolicy == nil || !ref.Source.ReadPolicy.Historical && ref.Source.ReadPolicy.ValidAt == "" && ref.Source.ReadPolicy.BelievedAt == "") {
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
		source := sources[healthVersionKey(*row)]
		if source != nil && source.Kind == "memory_record" {
			row.Families = validHealthFamilies(row.Families)
			row.Family = ""
			if len(row.Families) == 1 {
				row.Family = row.Families[0]
			}
			continue
		}
		row.Family = ""
		row.Families = nil
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
			row.Families = healthFamiliesFromEvidence(response.Payload, source)
			if len(row.Families) == 1 {
				row.Family = row.Families[0]
			}
		}
	}
	args["_health_records"], _ = json.Marshal(p.HealthRecords)
}

// This runs after the required recall transaction completed, in separate bounded
// owner reads. Telemetry timeout/rollback cannot abort the serving transaction.
func captureSharedRecallFamilies(options handlerOptions, invocation bus.ModuleInvocation, request DataRequest, payload json.RawMessage) []healthRecord {
	if options.placement != PlacementKB || os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" {
		return nil
	}
	var bundle recallBundle
	if len(payload) > 64<<10 || json.Unmarshal(payload, &bundle) != nil {
		return nil
	}
	budget := invocation.Remaining(50 * time.Millisecond)
	if budget <= 0 {
		return nil
	}
	parent := options.dataContext
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, budget)
	defer cancel()
	options.dataContext = ctx
	records := []healthRecord{}
	seen := map[string]bool{}
	attempted := 0
	for _, section := range [][]RecallRecord{bundle.Identity, bundle.Preferences, bundle.ActiveContext, bundle.OpenCommitments} {
		for _, row := range section {
			if row.Version == nil || row.Historical || row.Store != "kb" {
				continue
			}
			source := &typedSourceVersion{Kind: "memory_record", Version: *row.Version}
			record := healthRecord{RecordID: healthSourceIdentity(source), VersionID: row.Version.RecordRevision, Kind: row.Kind}
			key := healthVersionKey(record)
			if seen[key] {
				continue
			}
			seen[key] = true
			if attempted >= 16 || ctx.Err() != nil {
				return records
			}
			attempted++
			evidence := DataRequest{Operation: "evidence", ID: row.ID, Scope: request.Scope, Project: request.Project, Workspace: request.Workspace, IncludeAll: request.IncludeAll}
			wire, _ := json.Marshal(evidence)
			raw, status := handleData(options, invocation, wire)
			var response DataResponse
			if status == bus.ModuleStatusOK && json.Unmarshal(raw, &response) == nil {
				record.Families = healthFamiliesFromEvidence(response.Payload, source)
			}
			if len(record.Families) > 0 {
				if len(record.Families) == 1 {
					record.Family = record.Families[0]
				}
				records = append(records, record)
			}
		}
	}
	return records
}
func mergeNativeHealthFamilies(records []healthRecord, raw json.RawMessage) []healthRecord {
	var observed []healthRecord
	if len(raw) > 12000 || json.Unmarshal(raw, &observed) != nil || len(observed) > 16 {
		return records
	}
	known := map[string][]string{}
	for _, record := range observed {
		known[healthVersionKey(record)] = validHealthFamilies(record.Families)
	}
	for i, record := range records {
		if families := known[healthVersionKey(record)]; len(families) > 0 {
			records[i].Families = families
			if len(families) == 1 {
				records[i].Family = families[0]
			}
		}
	}
	return records
}
