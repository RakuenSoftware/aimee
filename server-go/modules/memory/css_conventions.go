package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Compatibility ontology for previously persisted typed facts. All assertions
// use the canonical graph, its authority rules, evidence and sealed commits.
var typedFactKinds = map[string][2]string{
	"naming_convention": {"project", "scalar"}, "token_strategy": {"project", "scalar"},
	"file_layout": {"project", "scalar"}, "component_owns_styles": {"project", "scalar"},
	"should_match": {"component", "convention"}, "architecture_settled": {"code_site", "scalar"},
	"located_in": {"person", "place"}, "has_ip": {"device", "scalar"},
}

func typedFactAssertion(subject, subjectKind, relation, object, objectKind string, confidence int, source, at string) (factAssertion, error) {
	kinds, ok := typedFactKinds[relation]
	if !ok {
		return factAssertion{}, errors.New("memory: unknown typed relation")
	}
	tailOK := kinds[1] == objectKind || kinds[1] == "scalar" && (objectKind == "" || objectKind == "value")
	if kinds[0] != subjectKind || !tailOK {
		return factAssertion{}, errors.New("memory: invalid typed fact kinds")
	}
	kind := NodeOther
	if objectKind == "scalar" || objectKind == "value" {
		kind = NodeScalar
	}
	return factAssertion{FactCandidate: FactCandidate{
		Subject: subject, Relation: relation, Object: object, SubjectKind: NodeOther, ObjectKind: kind,
		Actor:         FactActor{Principal: "system:kb-maintenance", TransportIdentity: "internal", Role: "system", Rank: 20},
		Evidence:      FactEvidence{SourceKind: "typed_fact_source", SourceID: source, ObservedAt: at, Stance: "supports"},
		AssertionKind: "world_fact", ValidFrom: at,
	}, ConfidenceClass: "B", Confidence: float64(max(0, min(confidence, 100))) / 100, Functional: true}, nil
}

func handleCSSConventions(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	project, ok := args.stringValue("project")
	if !ok || strings.TrimSpace(project) == "" || len(project) > 4096 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	request := DataRequest{Operation: args.stringOr("operation", ""), Project: project, Scope: Scope{Type: ScopeProject, Value: project}}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(map[string]string{"json": string(response.Payload)})
}

func (s *postgresDataStore) syncCSSConventions(ctx context.Context, project string) (int, error) {
	if s.settings == nil {
		return 0, errors.New("memory: CSS configuration unavailable")
	}
	settings, err := s.settings()
	if err != nil {
		return 0, err
	}
	if configNumber(settings, "css_style_graph_enabled") == 0 {
		return 0, nil
	}
	// One snapshot for all three measurements; never turn a failed token query
	// into a false literal-values assertion. Retired generations are excluded.
	var rules, bem, tokens int64
	err = s.db.QueryRow(ctx, `WITH current_rules AS (
 SELECT c.id,c.selector FROM css_rules c JOIN files f ON f.id=c.file_id
 JOIN projects p ON p.id=f.project_id WHERE p.name=$1
 AND p.lifecycle_state='current' AND f.generation=p.current_generation)
 SELECT count(*),count(*) FILTER(WHERE strpos(selector,'__')>0 OR strpos(selector,'--')>0),
 (SELECT count(*) FROM css_declarations d JOIN current_rules c ON c.id=d.rule_id WHERE left(d.property,2)='--')
 FROM current_rules`, project).Scan(&rules, &bem, &tokens)
	if err != nil || rules == 0 {
		return 0, err
	}
	var at string
	if err := s.db.QueryRow(ctx, `SELECT pg_now_text()`).Scan(&at); err != nil {
		return 0, err
	}
	naming, strategy := "flat-utility", "literal-values"
	if bem > 0 {
		naming = "BEM"
	}
	if tokens > 0 {
		strategy = "css-custom-properties"
	}
	for _, item := range [][2]string{{"naming_convention", naming}, {"token_strategy", strategy}} {
		assertion, err := typedFactAssertion(project, "project", item[0], item[1], "scalar", 75, "exemplar-scan", at)
		if err != nil {
			return 0, err
		}
		if _, err = s.assertFact(ctx, assertion); err != nil {
			return 0, err
		}
	}
	// Successful unchanged receipts count too, preserving the public contract.
	return 2, nil
}

type cssConvention struct {
	Relation   string `json:"relation"`
	Value      string `json:"value"`
	Confidence int    `json:"confidence"`
	Source     string `json:"source"`
	AssertedAt string `json:"asserted_at"`
}

func (s *postgresDataStore) cssConventions(ctx context.Context, project string) ([]cssConvention, error) {
	// Preserve the subject-wide view, but exclude hidden or retired memory
	// evidence before applying the cap. Text travels intact, without C buffers.
	rows, err := s.db.Query(ctx, `SELECT e.relation,e.target,CAST(e.confidence*100 AS INTEGER),
 COALESCE((SELECT f.source_id FROM fact_evidence f WHERE f.assertion_id=e.id AND f.invalidated_at='' ORDER BY f.id DESC LIMIT 1),''),e.asserted_at
 FROM entity_edges e WHERE e.source=$1 AND e.edge_class='semantic'
 AND e.lifecycle_state IN ('persistent','promoted') AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0
 AND `+memoryValiditySQL("e.")+` AND `+memoryStartedAtSQL("e.asserted_at", "CURRENT_TIMESTAMP")+`
 AND `+currentMemoryEvidenceSQL("e", "", true)+`
 ORDER BY e.relation,e.id LIMIT 64`, project)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []cssConvention{}
	for rows.Next() {
		var item cssConvention
		if err := rows.Scan(&item.Relation, &item.Value, &item.Confidence, &item.Source, &item.AssertedAt); err != nil {
			return nil, err
		}
		out = append(out, item)
	}
	return out, rows.Err()
}
