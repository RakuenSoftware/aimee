package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

type cognifyRelation struct {
	Subject  string `json:"subject"`
	Relation string `json:"relation"`
	Object   string `json:"object"`
	FactText string `json:"fact_text"`
}
type cognifyClaim struct {
	Subject   string `json:"subject"`
	Attribute string `json:"attribute"`
	Value     string `json:"value"`
	Kind      string `json:"kind"`
}
type cognifyCoref struct {
	Pronoun    string  `json:"pronoun"`
	Entity     string  `json:"entity"`
	Confidence float64 `json:"confidence"`
}
type cognifyResult struct {
	Status     string            `json:"status"`
	UnitID     int64             `json:"unit_id"`
	Queued     bool              `json:"queued"`
	Summary    string            `json:"summary"`
	MemoryKind string            `json:"memory_kind"`
	Relations  []cognifyRelation `json:"relations"`
	Claims     []cognifyClaim    `json:"claims"`
	Coref      []cognifyCoref    `json:"coref_bindings"`
}

// Preserve the permissive field contract, but bound the entire model response.
// A malformed item cannot discard its valid siblings; strings are never clipped.
func parseCognify(raw []byte) (cognifyResult, error) {
	out := cognifyResult{Status: "ok", Relations: []cognifyRelation{}, Claims: []cognifyClaim{}, Coref: []cognifyCoref{}}
	var obj commandArgs
	if len(raw) > episodeMaxOutput || json.Unmarshal(raw, &obj) != nil || obj == nil {
		return out, errors.New("memory: malformed cognification response")
	}
	out.Summary, out.MemoryKind = obj.stringOr("summary", ""), obj.stringOr("memory_kind", "")
	items := func(key string) []commandArgs {
		var rawItems []json.RawMessage
		if json.Unmarshal(obj[key], &rawItems) != nil {
			return nil
		}
		items := []commandArgs{}
		for _, raw := range rawItems {
			var item commandArgs
			if json.Unmarshal(raw, &item) == nil && item != nil {
				items = append(items, item)
			}
		}
		return items
	}
	for _, item := range items("relations") {
		a, okA := item.stringValue("subject")
		b, okB := item.stringValue("relation")
		c, okC := item.stringValue("object")
		if okA && okB && okC && len(out.Relations) < 16 {
			out.Relations = append(out.Relations, cognifyRelation{a, b, c, item.stringOr("fact_text", "")})
		}
	}
	for _, item := range items("claims") {
		a, okA := item.stringValue("subject")
		b, okB := item.stringValue("attribute")
		c, okC := item.stringValue("value")
		kind := item.stringOr("kind", "fact")
		if kind == "" {
			kind = "fact"
		}
		if okA && okB && okC && len(out.Claims) < 16 {
			out.Claims = append(out.Claims, cognifyClaim{a, b, c, kind})
		}
	}
	for _, item := range items("coref_bindings") {
		entity := item.stringOr("entity", "")
		confidence, ok := item.number("confidence")
		if !ok {
			confidence = 0.5
		}
		if entity != "" && len(out.Coref) < 8 {
			out.Coref = append(out.Coref, cognifyCoref{item.stringOr("pronoun", ""), entity, confidence})
		}
	}
	return out, nil
}

var errCognifyDisabled = errors.New("memory: cognification is disabled or unconfigured")
var errDerivedSource = errors.New("memory: derivation source is missing, retired, suppressed, cyclic or exceeds bounds")

func (s *postgresDataStore) cognifySettings() (command string, async bool, err error) {
	if s.settings == nil {
		return "", false, errCognifyDisabled
	}
	values, err := s.settings()
	if err != nil {
		return "", false, err
	}
	command, _ = values["memory_cognify_command"].(string)
	if configNumber(values, "memory_cognify_enabled") == 0 || strings.TrimSpace(command) == "" {
		return "", false, errCognifyDisabled
	}
	return command, configNumber(values, "memory_cognify_async_enabled") != 0, nil
}

// Apply the same versioned, scoped ancestry gate used by retrieval before
// releasing source text to an extractor. Unknown or stale lineage fails closed.
func (s *postgresDataStore) derivedSourcesAllowed(ctx context.Context, root int64) error {
	var allowed bool
	if err := s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memories m WHERE m.id=$1 AND `+currentMemorySQL("m.")+`)`, root).Scan(&allowed); err != nil {
		return err
	}
	if !allowed {
		return errDerivedSource
	}
	return nil
}

func (s *postgresDataStore) cognifySource(ctx context.Context, id int64) (Record, error) {
	var r Record
	err := s.db.QueryRow(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence FROM memories
 WHERE id=$1 AND lifecycle_state='active' AND activation_suppressed=0 FOR UPDATE`, id).
		Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence)
	if store.IsNoRows(err) {
		return r, ErrMemoryNotFound
	}
	return r, err
}

func (s *postgresDataStore) cognify(ctx context.Context, id int64, command string, async bool) (cognifyResult, error) {
	out := cognifyResult{Status: "ok", UnitID: id, Relations: []cognifyRelation{}, Claims: []cognifyClaim{}, Coref: []cognifyCoref{}}
	if _, ok := s.db.(store.Tx); !ok {
		return out, errors.New("memory: cognification requires a transaction")
	}
	source, err := s.cognifySource(ctx, id)
	if err != nil {
		return out, err
	}
	if err = s.derivedSourcesAllowed(ctx, id); err != nil {
		return out, err
	}
	if async {
		_, err = s.db.Exec(ctx, `INSERT INTO kb_async_jobs(kind,document_id,project,status,updated_at)
 VALUES('memory_cognify',$1,'memory','pending',pg_now_text()) ON CONFLICT(kind,document_id) DO UPDATE SET
 status='pending',attempts=0,last_error='',next_attempt_at='',generation=kb_async_jobs.generation+1,updated_at=pg_now_text()`, id)
		out.Queued = err == nil
		return out, err
	}
	text, err := screenMemoryWrite(source.Key, source.Content)
	if err != nil {
		return out, err
	}
	if text == "" || len(text) > episodeMaxInput {
		return out, errEpisodeCapacity
	}
	input, _ := json.Marshal(map[string]any{"unit_id": id, "text": text})
	if len(input) > episodeMaxInput {
		return out, errEpisodeCapacity
	}
	runner := s.episodeCommand
	if runner == nil {
		runner = runEpisodeCommand
	}
	attempt, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	raw, err := runner(attempt, command, input)
	if err != nil {
		return out, errors.New("memory: cognifier execution failed")
	}
	// Screen the complete output before parsing, returning, or persisting it.
	// Parsing precedes redaction so JSON escapes cannot hide credentials.
	out, err = parseCognify(raw)
	if err != nil {
		return out, err
	}
	out.UnitID = id
	out.Summary, err = screenMemoryText(out.Summary)
	if err != nil {
		return out, err
	}
	for i := range out.Relations {
		r := &out.Relations[i]
		for _, field := range []*string{&r.Subject, &r.Relation, &r.Object, &r.FactText} {
			*field, err = screenMemoryText(*field)
			if err != nil {
				return out, err
			}
		}
	}
	for i := range out.Claims {
		c := &out.Claims[i]
		c.Value, err = screenMemoryWrite(c.Subject+":"+c.Attribute, c.Value)
		if err != nil {
			return out, err
		}
		if scanContent(c.Kind, 0).SensitiveStatus != 0 {
			return out, errSensitiveMemory
		}
	}
	for i := range out.Coref {
		for _, field := range []*string{&out.Coref[i].Pronoun, &out.Coref[i].Entity} {
			*field, err = screenMemoryText(*field)
			if err != nil {
				return out, err
			}
		}
	}
	switch out.MemoryKind {
	case "episodic", "semantic", "procedural":
		_, err = s.db.Exec(ctx, `UPDATE memories SET cognified_memory_kind=$2,updated_at=pg_now_text() WHERE id=$1`, id, out.MemoryKind)
		if err != nil {
			return out, err
		}
	default:
		out.MemoryKind = ""
	}
	for _, r := range out.Relations {
		if r.Subject == "" || r.Relation == "" || r.Object == "" {
			continue
		}
		err = s.writeCognifyRelation(ctx, id, r)
		if err != nil {
			return out, err
		}
	}
	for _, c := range out.Claims {
		if c.Subject == "" || c.Attribute == "" || c.Value == "" {
			continue
		}
		key := c.Subject + ":" + c.Attribute
		if key == source.Key {
			return out, errDerivedSource
		}
		confidence := 0.8
		if c.Kind == "opinion" {
			confidence = 0.5
		}
		// Claims inherit canonical scope. They never acquire user authority from a
		// user-authored source or overwrite an independently authored memory.
		if _, err = s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5747))`, string(source.Scope.Type)+"\x1f"+source.Scope.Value+"\x1f"+key); err != nil {
			return out, err
		}
		var existing int64
		err = s.db.QueryRow(ctx, `SELECT id FROM memories WHERE kind=$1 AND key=$2 AND scope_type=$3 AND scope_value=$4 AND lifecycle_state='active' FOR UPDATE`, c.Kind, key, source.Scope.Type, source.Scope.Value).Scan(&existing)
		if err != nil && !store.IsNoRows(err) {
			return out, err
		}
		if err == nil {
			var owned bool
			if err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memory_lineage WHERE object_type='memory' AND object_id=$1 AND source_kind='metadata' AND source_ref=$2)`, existing, fmt.Sprintf("memory-cognify-v1:%d", id)).Scan(&owned); err != nil {
				return out, err
			}
			if !owned {
				continue
			}
		}
		record, err := s.InsertEpistemic(ctx, DataRequest{Scope: source.Scope, Tier: "L2", Kind: c.Kind, Key: key, Content: c.Value, Confidence: &confidence, SessionID: "cognify"})
		if proposedCorrection(err) != nil {
			continue
		}
		if err != nil {
			return out, err
		}
		if record.ID == id {
			return out, errDerivedSource
		}
		if err = s.captureStoredFactActor(ctx, record.ID, AuthorityModel, nil); err != nil {
			return out, err
		}
		_, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref,confidence)
 SELECT 'memory',$1,kind,ref,$3 FROM (VALUES('memory',$2),('metadata',$4)) AS refs(kind,ref)
 WHERE NOT EXISTS(SELECT 1 FROM memory_lineage WHERE object_type='memory' AND object_id=$1 AND source_kind=kind AND source_ref=ref)`, record.ID, fmt.Sprintf("memory:%d", id), confidence, fmt.Sprintf("memory-cognify-v1:%d", id))
		if err != nil {
			return out, err
		}
		// Bind the copied claim to both exact revisions while the canonical source
		// remains locked. Observation replacement never rewrites the claim itself.
		if _, err = s.db.Exec(ctx, `DELETE FROM memory_lineage WHERE object_type='memory'
 AND object_id=$1 AND source_kind='memory-cognify-input-v1'`, record.ID); err != nil {
			return out, err
		}
		if _, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'memory',child.id,'memory-cognify-input-v1',jsonb_build_object(
 'schema_version',1,'owner_id',(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 'record_id',parent.id::text,'record_revision',parent.record_revision::text,
 'derived_revision',child.record_revision::text)::text
 FROM memories child,memories parent WHERE child.id=$1 AND parent.id=$2`, record.ID, id); err != nil {
			return out, err
		}
		if err = s.registerDerivedMemoryInputs(ctx, record.ID); err != nil {
			return out, err
		}
		// The legacy rule table is global. Scoped preferences remain scoped memories
		// rather than being disclosed to every project through the rule channel.
		// Model extraction may refresh soft guidance, never a protected hard rule.
		// A hard-rule collision also prevents a duplicate soft rule with that title.
		if source.Scope.Type == ScopeGlobal && (out.MemoryKind == "procedural" || c.Kind == "preference" || c.Kind == "policy" || c.Kind == "procedure" || c.Kind == "workflow") {
			err = s.writeCognifyRule(ctx, id, record.ID, key, c.Value)
			if err != nil {
				return out, err
			}
		}
	}
	return out, nil
}

func handleCognifyCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: strings.ReplaceAll(verb, "_", "-"), IncludeAll: true, Limit: args.limit("limit", 16, 64)}
	if verb == "cognify" {
		var ok bool
		request.ID, ok = args.positiveID("unit")
		if !ok {
			request.ID, ok = args.positiveID("id")
		}
		if !ok {
			return commandResult(commandError("invalid_argument", "cognification requires a positive unit id"))
		}
	}
	commandScope(args, &request)
	raw, _ := json.Marshal(request)
	data, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "cognification unavailable or refused"))
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil || response.Payload == nil {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
