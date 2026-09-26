package memory

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

type inspectedHealthReceipt struct {
	Assembly json.RawMessage       `json:"assembly"`
	Prepared *providerReceiptEvent `json:"prepared_receipt"`
	Sequence string                `json:"prepared_sequence"`
	State    string                `json:"state"`
	Stages   []string              `json:"stages"`
}

// Legacy receipts contain exact versioned sources but lack some health
// metadata. Keep those omissions visible; neither a source channel nor a
// request ID proves memory kind, independent family, query class or task.
func healthSnapshotFromReceipt(r inspectedHealthReceipt, j *healthJournal, ppm int) (healthSnapshot, error) {
	if r.Prepared == nil || r.Prepared.Binding == nil {
		return healthSnapshot{}, errors.New("missing verified prepared receipt")
	}
	b := r.Prepared.Binding
	seq, err := strconv.ParseUint(r.Sequence, 10, 64)
	if err != nil || seq == 0 {
		return healthSnapshot{}, errors.New("missing verified prepared sequence")
	}
	at, err := time.Parse(time.RFC3339Nano, r.Prepared.At)
	if err != nil {
		return healthSnapshot{}, err
	}
	e := healthInvocation{Request: b.RequestID, Attempt: r.Prepared.AttemptID, Binding: r.Prepared.BindingDigest, At: at,
		Namespace: j.Namespace, Principal: j.Principal, Project: b.Project, Workspace: b.Workspace,
		Purpose: "provider_input", QueryClass: "unclassified", Turn: b.TurnID, SamplePPM: ppm,
		SamplingEpoch: releaseDigest([]any{"health-bernoulli-v1", j.Namespace, j.Principal, j.Project, j.Workspace, j.Key}), Records: []healthRecord{},
		MetadataGaps: []string{"query_fingerprint", "query_class", "task_turn_links", "memory_kind", "family", "trust", "release_labels", "arm_contributions", "final_rank"}}
	if old, ok := j.Attempts[e.Attempt]; ok {
		e.Request = old.Invocation.Request
		e.SamplePPM = old.Invocation.SamplePPM
		e.SamplingEpoch = old.Invocation.SamplingEpoch
	}
	var metadata struct {
		Context   *healthQueryContext     `json:"health_context"`
		Records   []healthRecord          `json:"health_records"`
		Labels    *healthLabelsEnvelope   `json:"health_labels"`
		Execution *healthExecutionContext `json:"health_execution"`
		Release   *healthReleaseCheck     `json:"health_release"`
	}
	if json.Unmarshal(r.Assembly, &metadata) == nil && metadata.Context != nil {
		capture := metadata.Context
		if capture.Namespace == j.Namespace && capture.Project == b.Project && capture.Workspace == b.Workspace && receiptDigestValid(capture.Fingerprint) && (capture.Source == "native_task_hint" || capture.Source == "ingress_query") {
			e.Fingerprint = capture.Fingerprint
			gaps := e.MetadataGaps[:0]
			for _, gap := range e.MetadataGaps {
				if gap != "query_fingerprint" {
					gaps = append(gaps, gap)
				}
			}
			e.MetadataGaps = gaps
		}
	}
	if execution := metadata.Execution; execution != nil && execution.valid(r.Prepared.BindingDigest) {
		e.Task, e.Turn, e.QueryClass = execution.Task, execution.Turn, execution.QueryClass
		gaps := e.MetadataGaps[:0]
		for _, gap := range e.MetadataGaps {
			if gap != "task_turn_links" && gap != "query_class" {
				gaps = append(gaps, gap)
			}
		}
		// A host-owned task/turn identity does not establish adjacency across
		// requests or branches. Repeats remain unknown without an owned link.
		if execution.PreviousTurn != "" {
			e.PreviousTurn = execution.PreviousTurn
			e.MetadataGaps = gaps
		} else {
			e.MetadataGaps = append(gaps, "previous_eligible_turn")
		}
	}
	var refs []typedProjectionRef
	if json.Unmarshal(b.Sources, &refs) != nil {
		return healthSnapshot{}, errors.New("invalid verified source references")
	}
	if b.SourceCoverage != "retained_versioned_inputs" {
		e.MetadataGaps = append(e.MetadataGaps, "versioned_source_coverage")
	}
	for _, ref := range refs {
		if ref.Source == nil {
			continue
		}
		source := ref.Source
		// Collection dependencies are fences, not rendered record occurrences.
		if strings.HasSuffix(source.Kind, "_collection") {
			continue
		}
		identity, _ := json.Marshal([]string{source.Kind, source.Version.OwnerID, source.Version.RecordID})
		historical := source.ReadPolicy != nil && (source.ReadPolicy.Historical || source.ReadPolicy.ValidAt != "" || source.ReadPolicy.BelievedAt != "")
		e.Records = append(e.Records, healthRecord{RecordID: string(identity), VersionID: source.Version.RecordRevision, Kind: "unknown", Historical: historical})
	}
	if labels := metadata.Labels; labels != nil && labels.SourcesDigest == b.SourcesDigest && labels.Labels != nil {
		validation := healthLabelMetrics{}
		if err := validation.add(labels.Labels); err != nil {
			return healthSnapshot{}, err
		}
		e.Labels = labels.Labels
	}
	known := map[string]healthRecord{}
	for _, record := range metadata.Records {
		known[healthVersionKey(record)] = record
	}
	knownKinds, knownTrust, knownFamilies := len(e.Records) > 0, len(e.Records) > 0, len(e.Records) > 0
	knownRelease := len(e.Records) > 0 && healthReleaseValid(metadata.Release, *b, r.Prepared.BindingDigest, at)
	if knownRelease {
		e.ReleaseVerifier = metadata.Release.Verifier
	}
	knownPositions := len(e.Records) > 0
	knownArms := len(e.Records) > 0
	for i, record := range e.Records {
		if observed, ok := known[healthVersionKey(record)]; ok && healthLabelName(observed.Kind) {
			record.Kind, record.LowTrust, record.Family = observed.Kind, observed.LowTrust, observed.Family
			record.Families = validHealthFamilies(observed.Families)
			if validHealthProvenance(observed.Provenance) {
				record.Provenance = observed.Provenance
			}
			record.Positions = mergeHealthPositions(nil, observed.Positions, false)
			record.Arms = validatedHealthArms(observed.Arms)
			record.RankingSteps = validatedHealthRanking(observed.RankingSteps)
			record.SelectionPaths = validatedHealthSelectionPaths(observed.SelectionPaths)
			if healthLabelName(observed.State) {
				record.State = observed.State
			}
			if validHealthConfidenceClass(observed.ConfidenceClass) {
				record.ConfidenceClass = observed.ConfidenceClass
			}
			if assertionTimestamp(observed.ValidFrom) && assertionTimestamp(observed.ValidUntil) {
				record.ValidFrom, record.ValidUntil = observed.ValidFrom, observed.ValidUntil
			}
			e.Records[i] = record
		}
		if knownRelease && !record.Historical {
			eligible := false
			record.LifecycleViolation = &eligible
			e.Records[i] = record
		}
		knownKinds = knownKinds && record.Kind != "unknown"
		knownTrust = knownTrust && record.LowTrust != nil
		knownFamilies = knownFamilies && len(healthRecordFamilies(record)) > 0
		knownPositions = knownPositions && len(record.Positions) > 0
		knownArms = knownArms && healthRecordArmsKnown(record)
	}
	gaps := e.MetadataGaps[:0]
	for _, gap := range e.MetadataGaps {
		if !(gap == "memory_kind" && knownKinds || gap == "trust" && knownTrust || gap == "family" && knownFamilies || gap == "final_rank" && knownPositions || gap == "arm_contributions" && knownArms || gap == "release_labels" && knownRelease) {
			gaps = append(gaps, gap)
		}
	}
	e.MetadataGaps = gaps
	result := healthSnapshot{Invocation: e, PreparedSequence: seq, ResolvedUnsent: r.State == "prepared_without_dispatch"}
	for _, stage := range r.Stages {
		switch stage {
		case "dispatch_admitted":
			result.Admitted = true
		case "dispatch_started":
			result.Started = true
		case "acknowledged":
			result.Acknowledged = true
		case "outcome_unknown":
			result.OutcomeUnknown = true
		}
	}
	return result, nil
}

// Only the host that reads and verifies the owned WORM ledger may supply these
// rows. PrincipalRef rejects plugin/module callers; the HTTP/CLI adapter must
// derive health_principal from authenticated request context and never forward
// a principal, namespace or ledger supplied in a public request body.
func handleRetrievalHealth(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if options.placement != PlacementServer {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	principal := args.stringOr("health_principal", "")
	if principal == "" || len(principal) > 128 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if caller := options.commandContext; caller != nil && (!caller.Authenticated || caller.Principal != principal) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" {
		return commandResult(commandError("disabled", "optional retrieval health collection is disabled; required receipts are unchanged"))
	}
	now := time.Now().UTC()
	window, err := time.ParseDuration(args.stringOr("window", "24h"))
	if err != nil || window <= 0 || window > healthRetention {
		return commandResult(commandError("invalid_argument", "health window must be positive and at most 24h"))
	}
	if args.stringOr("operation", "") == "health-plan" {
		scanFrom := now.Add(-window)
		if 2*window <= healthRetention {
			scanFrom = scanFrom.Add(-window)
		}
		return commandResult(map[string]any{"status": "ok", "from": now.Add(-window).Format(time.RFC3339Nano), "scan_from": scanFrom.Format(time.RFC3339Nano), "until": now.Format(time.RFC3339Nano)})
	}
	s, ok := options.data.(*postgresDataStore)
	if !ok {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	budget := invocation.Remaining(5 * time.Second)
	if budget <= 0 {
		return nil, bus.ModuleStatusCancelled
	}
	parent := options.dataContext
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, budget)
	defer cancel()
	if args.stringOr("operation", "") == "health-import" {
		// Reuse the receipt owner's strict ordering, binding, source commitment
		// and dispatcher checks, rather than interpreting unverified row JSON.
		encoded, status := inspectProviderReceipts(args)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		raw, err := bus.DecodeCommandResult(encoded)
		var inspected struct {
			Status   string                   `json:"status"`
			Receipts []inspectedHealthReceipt `json:"receipts"`
		}
		if err != nil || json.Unmarshal(raw, &inspected) != nil || inspected.Status != "ok" {
			return commandResult(commandError("unavailable", "verified receipt population unavailable"))
		}
		ppm := 1000000
		if value := os.Getenv("AIMEE_MEMORY_HEALTH_SAMPLE_PPM"); value != "" {
			ppm, err = strconv.Atoi(value)
			if err != nil || ppm <= 0 || ppm > 1000000 {
				return commandResult(commandError("invalid_configuration", "invalid invocation sampling probability"))
			}
		}
		for _, r := range inspected.Receipts {
			if r.Prepared == nil || r.Prepared.Binding == nil {
				return nil, bus.ModuleStatusInternal
			}
			b := r.Prepared.Binding
			_, err = s.updateHealthStore(ctx, principal, b.Project, b.Workspace, now, func(j *healthJournal) error {
				e, err := healthSnapshotFromReceipt(r, j, ppm)
				if err != nil {
					return err
				}
				return j.apply(e, now)
			})
			if err != nil {
				return commandResult(commandError("unavailable", "retrieval health import did not complete; required receipt remains authoritative"))
			}
		}
		return commandResult(map[string]any{"status": "ok", "imported_attempts": len(inspected.Receipts), "source": "verified_local_receipt_ledger"})
	}
	project, workspace := args.stringOr("project", ""), args.stringOr("workspace", "")
	from, until := now.Add(-window), now
	if args.stringOr("from", "") != "" || args.stringOr("until", "") != "" {
		from, err = time.Parse(time.RFC3339Nano, args.stringOr("from", ""))
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		until, err = time.Parse(time.RFC3339Nano, args.stringOr("until", ""))
		if err != nil || !from.Before(until) || until.After(now) || until.Sub(from) > healthRetention {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	j, err := s.readHealthStore(ctx, principal, project, workspace)
	if store.IsNoRows(err) {
		return commandResult(map[string]any{"status": "ok", "collection_state": "not_collected", "text": "Retrieval health: no collected population for this exact scope; coverage and metrics are unknown.\n", "window_complete": false, "metrics": nil})
	}
	if err != nil {
		return commandResult(commandError("unavailable", "retrieval health store unavailable"))
	}
	p := healthPopulation{Namespace: j.Namespace, Principal: principal, Project: project, Workspace: workspace,
		Purpose: args.stringOr("purpose", "provider_input"), QueryClass: args.stringOr("query_class", "unclassified"), Stage: args.stringOr("stage", "dispatched"), From: from, Until: until}
	r, err := j.report(p, now)
	if err != nil {
		return commandResult(commandError("invalid_argument", "unsupported health population"))
	}
	if !args.boolean("collection_complete") {
		r.Complete = false
	}
	baseline := healthBaseline{State: "unavailable", Reason: "comparison_exceeds_retention", Alerts: []healthAlert{}}
	if priorFrom := from.Add(-until.Sub(from)); !priorFrom.Before(now.Add(-healthRetention)) {
		priorPopulation := p
		priorPopulation.From, priorPopulation.Until = priorFrom, from
		prior, priorErr := j.report(priorPopulation, now)
		if priorErr == nil {
			baseline = compareHealthBaseline(r, prior)
		}
	}
	result := map[string]any{"status": "ok", "health": r, "baseline": baseline, "text": healthReportText(r) + healthBaselineText(baseline), "record_population": "versioned_memory_inputs", "collector": "authenticated_local_receipt_scan", "time_basis": "prepared_at", "stage_basis": "latest_verified_snapshot"}
	if args.boolean("traces") {
		page := j.traceReferences(p)
		result["traces"] = page
		result["text"] = result["text"].(string) + healthTraceText(page)
	}
	return commandResult(result)
}
