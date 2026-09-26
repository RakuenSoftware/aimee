package memory

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// This artifact is operator configuration, never a data-operation field. An
// override is an authenticated operator admission for one exact owner/version.
// Confirmation additionally identifies a committed update event for that version;
// neither access counters nor a newly authored model claim constitute confirmation.
type horizonOverride struct {
	Version                MemoryRecordVersion `json:"version"`
	Rule                   horizonRule         `json:"rule"`
	ConfirmationGeneration string              `json:"confirmation_generation,omitempty"`
}
type horizonConfiguration struct {
	Policy    horizonPolicy     `json:"policy"`
	Mode      string            `json:"mode"`
	Overrides []horizonOverride `json:"overrides,omitempty"`
}
type horizonPolicyIdentity struct {
	Revision string `json:"revision"`
	Digest   string `json:"digest"`
	Mode     string `json:"mode"`
}

func (c horizonConfiguration) identity() horizonPolicyIdentity {
	raw, _ := json.Marshal(c)
	return horizonPolicyIdentity{c.Policy.Revision, fmt.Sprintf("sha256:%x", sha256.Sum256(raw)), c.Mode}
}
func decodeHorizonConfiguration(raw string) (*horizonConfiguration, error) {
	if raw == "" {
		return nil, nil
	}
	if len(raw) > 65536 {
		return nil, errors.New("memory: utility horizon artifact exceeds 64 KiB")
	}
	var c *horizonConfiguration
	d := json.NewDecoder(strings.NewReader(raw))
	d.DisallowUnknownFields()
	if d.Decode(&c) != nil || c == nil || d.Decode(new(any)) != io.EOF {
		return nil, errors.New("memory: invalid utility horizon artifact")
	}
	if !c.Policy.valid() || (c.Mode != "shadow" && c.Mode != "enforce") || len(c.Overrides) > 128 {
		return nil, errors.New("memory: invalid utility horizon policy")
	}
	seen := map[MemoryRecordVersion]bool{}
	for _, o := range c.Overrides {
		id, err := strconv.ParseInt(o.Version.RecordID, 10, 64)
		if err != nil || !o.Version.validFor(id) || !validHorizonRule(o.Rule) || seen[o.Version] {
			return nil, errors.New("memory: invalid or duplicate horizon override")
		}
		seen[o.Version] = true
		if o.ConfirmationGeneration != "" {
			generation, err := strconv.ParseInt(o.ConfirmationGeneration, 10, 64)
			if err != nil || generation <= 0 || strconv.FormatInt(generation, 10) != o.ConfirmationGeneration || o.Rule.Anchor != "confirmed" {
				return nil, errors.New("memory: invalid horizon confirmation event")
			}
		}
	}
	if len(c.currentSQL("m.", false)) > 16<<10 {
		return nil, errors.New("memory: utility horizon policy exceeds the compiled predicate bound")
	}
	return c, nil
}

var horizonConfigCache struct {
	sync.Mutex
	raw    string
	config *horizonConfiguration
	err    error
}

func configuredUtilityHorizon() (*horizonConfiguration, error) {
	raw := os.Getenv("AIMEE_MEMORY_UTILITY_HORIZON_POLICY")
	horizonConfigCache.Lock()
	defer horizonConfigCache.Unlock()
	if raw != horizonConfigCache.raw {
		horizonConfigCache.config, horizonConfigCache.err = decodeHorizonConfiguration(raw)
		horizonConfigCache.raw = raw
	}
	return horizonConfigCache.config, horizonConfigCache.err
}
func currentHorizonIdentity() *horizonPolicyIdentity {
	c, err := configuredUtilityHorizon()
	if err != nil || c == nil {
		return nil
	}
	id := c.identity()
	return &id
}
func horizonLiteral(value string) string { return "'" + strings.ReplaceAll(value, "'", "''") + "'" }
func horizonPrefix(prefix string, personal bool) string {
	if prefix != "" {
		return prefix
	}
	if personal {
		return "user_memories."
	}
	return "memories."
}
func horizonOwnerTable(personal bool) string {
	if personal {
		return "user_memory_collection_generation"
	}
	return "memory_collection_owner"
}
func horizonJournalTable(personal bool) string {
	if personal {
		return "user_memory_invalidation_outbox"
	}
	return "memory_invalidation_outbox"
}
func horizonDomainSQL(prefix string, personal bool) string {
	if personal {
		return "'personal'"
	}
	return "(" + prefix + "scope_type||':'||" + prefix + "scope_value)"
}
func horizonOverrideSQL(prefix string, personal bool, o horizonOverride) string {
	return prefix + "id=" + o.Version.RecordID + " AND " + prefix + "record_revision=" + o.Version.RecordRevision + " AND (SELECT owner_id::text FROM " + horizonOwnerTable(personal) + " WHERE id=1)=" + horizonLiteral(o.Version.OwnerID)
}

// A creation anchor is the protected INSERT event, not editable created_at or
// updated_at. Old records without an event remain explicitly unknown. Moving a
// shared record out of its creation audience can make that anchor unavailable;
// we do not bypass journal RLS to recover it.
func horizonAnchorSQL(prefix string, personal bool, rule horizonRule, generation string) string {
	where := "horizon_event.memory_id=" + prefix + "id AND horizon_event.operation='insert' AND horizon_event.record_revision=1"
	if rule.Anchor == "confirmed" {
		if generation == "" {
			return "NULL::timestamptz"
		}
		where = "horizon_event.memory_id=" + prefix + "id AND horizon_event.operation='update' AND horizon_event.record_revision=" + prefix + "record_revision AND horizon_event.generation=" + generation
		if !personal {
			where += " AND horizon_event.scope_type=" + prefix + "scope_type AND horizon_event.scope_value=" + prefix + "scope_value"
		}
	}
	return "(SELECT min(horizon_event.recorded_at) FROM " + horizonJournalTable(personal) + " horizon_event WHERE " + where + ")"
}
func horizonRuleSQL(prefix string, personal bool, rule horizonRule, generation, unknown string) string {
	at := horizonAnchorSQL(prefix, personal, rule, generation)
	deadline := "horizon_anchor.at + " + strconv.FormatInt(rule.DurationSeconds, 10) + " * interval '1 second'"
	if rule.Deadline != "" {
		deadline = "LEAST(" + deadline + "," + horizonLiteral(rule.Deadline) + "::timestamptz)"
	}
	return "(SELECT CASE WHEN horizon_anchor.at IS NULL OR horizon_anchor.at<TIMESTAMPTZ '0001-01-01 00:00:00+00' OR horizon_anchor.at>CURRENT_TIMESTAMP OR horizon_anchor.at + " + strconv.FormatInt(rule.DurationSeconds, 10) + " * interval '1 second'>=TIMESTAMPTZ '10000-01-01 00:00:00+00' THEN " + unknown + " ELSE CURRENT_TIMESTAMP < " + deadline + " END FROM (SELECT " + at + " AS at) horizon_anchor)"
}

// Compile the same finite precedence as the pure evaluator into the shared Go
// eligibility adapter, before every arm's LIMIT. No endpoint owns an age filter.
// Historical/diagnostic policy calls the existing non-horizon base predicate.
func utilityHorizonSQL(prefix string, personal bool) string {
	c, err := configuredUtilityHorizon()
	if err != nil {
		return "FALSE"
	}
	if c == nil || c.Mode != "enforce" {
		return "TRUE"
	}
	return c.currentSQL(prefix, personal)
}
func (c horizonConfiguration) selectionSQL(prefix string, personal bool, fallback string, ruleSQL func(string, bool, horizonRule, string, string) string) string {
	prefix = horizonPrefix(prefix, personal)
	unknown := "FALSE"
	if c.Policy.UnknownRule == "allow" {
		unknown = "TRUE"
	}
	var b bytes.Buffer
	b.WriteString("(CASE")
	keys := make([]string, 0, len(c.Policy.TransientKinds))
	for k, on := range c.Policy.TransientKinds {
		if on {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	for _, kind := range keys {
		b.WriteString(" WHEN " + prefix + "kind=" + horizonLiteral(kind) + " THEN ")
		if rule, ok := c.Policy.Safety[kind]; ok {
			b.WriteString(ruleSQL(prefix, personal, rule, "", unknown))
			continue
		}
		b.WriteString("CASE")
		for _, o := range c.Overrides {
			b.WriteString(" WHEN " + horizonOverrideSQL(prefix, personal, o) + " THEN " + ruleSQL(prefix, personal, o.Rule, o.ConfirmationGeneration, unknown))
		}
		domains := make([]string, 0, len(c.Policy.Domains))
		for d := range c.Policy.Domains {
			domains = append(domains, d)
		}
		sort.Strings(domains)
		for _, domain := range domains {
			b.WriteString(" WHEN " + horizonDomainSQL(prefix, personal) + "=" + horizonLiteral(domain) + " THEN " + ruleSQL(prefix, personal, c.Policy.Domains[domain], "", unknown))
		}
		// A CASE needs at least one WHEN even when only the kind default exists.
		b.WriteString(" WHEN TRUE THEN ")
		if rule, ok := c.Policy.Kinds[kind]; ok {
			b.WriteString(ruleSQL(prefix, personal, rule, "", unknown))
		} else {
			b.WriteString(fallback)
		}
		b.WriteString(" END")
	}
	b.WriteString(" WHEN TRUE THEN " + fallback + " END)")
	return b.String()
}

func (c horizonConfiguration) currentSQL(prefix string, personal bool) string {
	return c.selectionSQL(prefix, personal, "TRUE", horizonRuleSQL)
}
func horizonBoundarySQL(prefix string, personal bool, rule horizonRule, generation, _ string) string {
	at := horizonAnchorSQL(prefix, personal, rule, generation)
	deadline := "horizon_anchor.at + " + strconv.FormatInt(rule.DurationSeconds, 10) + " * interval '1 second'"
	if rule.Deadline != "" {
		deadline = "LEAST(" + deadline + "," + horizonLiteral(rule.Deadline) + "::timestamptz)"
	}
	// A future anchor becoming usable is itself a time-driven population change.
	return "(SELECT CASE WHEN horizon_anchor.at>CURRENT_TIMESTAMP THEN horizon_anchor.at ELSE " + deadline + " END FROM (SELECT " + at + " AS at) horizon_anchor)"
}
func horizonCollectionDeadlineSQL(personal bool, base string) string {
	c, err := configuredUtilityHorizon()
	if err != nil {
		return "to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
	}
	if c == nil || c.Mode != "enforce" {
		return base
	}
	table := "memories"
	if personal {
		table = "user_memories"
	}
	boundary := c.selectionSQL("m.", personal, "NULL::timestamptz", horizonBoundarySQL)
	return "(SELECT COALESCE(to_char(min(boundary) AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'),'') FROM (SELECT NULLIF(" + base + ",'')::timestamptz AS boundary UNION ALL SELECT " + boundary + " FROM " + table + " m) horizons WHERE boundary>CURRENT_TIMESTAMP)"
}
func horizonCollectionPolicyCheckSQL() string {
	if _, err := configuredUtilityHorizon(); err != nil {
		return "FALSE"
	}
	digest := ""
	if id := currentHorizonIdentity(); id != nil {
		digest = id.Digest
	}
	return "COALESCE(r.ref#>>'{source_version,utility_horizon_policy_digest}','')=" + horizonLiteral(digest)
}
