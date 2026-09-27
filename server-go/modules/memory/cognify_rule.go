package memory

import (
	"context"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

// The caller holds the canonical extraction source through this transaction.
// A title collision never transfers ownership from an authored rule to a model.
func (s *postgresDataStore) writeCognifyRule(ctx context.Context, producer, input int64, title, description string) error {
	marker := fmt.Sprintf("memory-cognify-rule-v1:%d", producer)
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,5748))`, title); err != nil {
		return err
	}
	var rule int64
	err := s.db.QueryRow(ctx, `WITH owned AS MATERIALIZED (
 SELECT r.id FROM rules r WHERE r.title=$1 AND r.directive_type='soft'
 AND EXISTS(SELECT 1 FROM memory_lineage l WHERE l.object_type='rule' AND l.object_id=r.id
 AND l.source_kind='metadata' AND l.source_ref=$3)
 AND NOT EXISTS(SELECT 1 FROM rules protected WHERE protected.title=$1 AND protected.directive_type='hard')
 ORDER BY r.id LIMIT 1 FOR UPDATE
), updated AS (
 UPDATE rules SET description=$2,weight=LEAST(100,weight+50),updated_at=pg_now_text(),last_reinforced_at=pg_now_text()
 WHERE id IN(SELECT id FROM owned) RETURNING id
), inserted AS (
 INSERT INTO rules(polarity,title,description,weight,domain,directive_type,created_at,updated_at,last_reinforced_at)
 SELECT 'principle',$1,$2,50,'memory-cognify','soft',pg_now_text(),pg_now_text(),pg_now_text()
 WHERE NOT EXISTS(SELECT 1 FROM rules WHERE title=$1) RETURNING id
) SELECT id FROM updated UNION ALL SELECT id FROM inserted`, title, description, marker).Scan(&rule)
	if store.IsNoRows(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'rule',$1,'metadata',$2 WHERE NOT EXISTS(SELECT 1 FROM memory_lineage
 WHERE object_type='rule' AND object_id=$1 AND source_kind='metadata' AND source_ref=$2)`, rule, marker); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `DELETE FROM memory_lineage WHERE object_type='rule' AND object_id=$1 AND source_kind='memory-cognify-input-v1'`, rule); err != nil {
		return err
	}
	tag, err := s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'rule',r.id,'memory-cognify-input-v1',jsonb_build_object('schema_version',1,
 'owner_id',o.owner_id::text,'record_id',m.id::text,'record_revision',m.record_revision::text,
 'derived_revision',r.record_revision::text,'derived_digest',`+ruleInputDigestSQL("r.")+`)::text
 FROM rules r,memories m,memory_collection_owner o WHERE r.id=$1 AND m.id=$2 AND o.id=1
 AND m.scope_type='global' AND m.scope_value='_global'`, rule, input)
	if err != nil {
		return err
	}
	if tag.RowsAffected() != 1 {
		return errDerivedSource
	}
	_, err = s.db.Exec(ctx, `SELECT derived_memory_declare('cognified_rule',$1,
 jsonb_build_array(jsonb_build_object('input_kind','memory','input_id',m.id::text,
 'input_version',m.record_revision::text,'extractor_version','memory-cognify-rule-v1',
 'contribution','essential')),'suppress') FROM memories m WHERE m.id=$2`, fmt.Sprint(rule), input)
	return err
}

func currentRuleInputsSQL(prefix string) string {
	return "(" + currentCognifiedRuleInputsSQL(prefix) + ") AND (COALESCE(" + prefix + "domain,'')<>'anti-pattern' OR (" + currentLegacyRuleInputsSQL("rule", prefix, ruleInputDigestSQL(prefix)) + "))"
}

func currentCognifiedRuleInputsSQL(prefix string) string {
	observation := `(CASE WHEN l.source_kind='memory-cognify-input-v1' THEN l.source_ref::jsonb END)`
	return `((COALESCE(` + prefix + `domain,'')<>'memory-cognify' AND NOT EXISTS(SELECT 1 FROM memory_lineage owner WHERE owner.object_type='rule'
 AND owner.object_id=` + prefix + `id AND owner.source_kind='metadata'
 AND owner.source_ref LIKE 'memory-cognify-rule-v1:%')) OR (
 EXISTS(SELECT 1 FROM memory_lineage l WHERE l.object_type='rule' AND l.object_id=` + prefix + `id
 AND l.source_kind='memory-cognify-input-v1')
 AND NOT EXISTS(SELECT 1 FROM memory_lineage l LEFT JOIN LATERAL (
 SELECT m.id FROM memories m WHERE m.id::text=` + observation + `->>'record_id'
 AND m.record_revision::text=` + observation + `->>'record_revision'
 AND m.scope_type='global' AND m.scope_value='_global' AND ` + baseCurrentMemorySQL("m.") + ` AND ` + derivedMemoryInputsForAudienceSQL("m.", false, "lineage_parent.scope_type='global' AND lineage_parent.scope_value='_global'") + `
 LIMIT 1) parent ON TRUE
 WHERE l.object_type='rule' AND l.object_id=` + prefix + `id AND l.source_kind='memory-cognify-input-v1'
 AND (parent.id IS NULL OR (` + observation + `->>'schema_version'='1'
 AND ` + observation + `->>'owner_id'=(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 AND ` + observation + `->>'derived_digest'=` + ruleInputDigestSQL(prefix) + `) IS DISTINCT FROM TRUE))))`
}

// Reinforcement, decay and an explicit authority promotion do not rewrite the
// copied claim. Bind its text separately; cached rule release still checks the
// complete canonical rule revision and its own applicability interval.
func ruleInputDigestSQL(prefix string) string {
	return `encode(sha256(convert_to(jsonb_build_array(` + prefix + `title,` + prefix + `description)::text,'UTF8')),'hex')`
}

// Legacy feedback inference is permitted only from directly authored rules.
// Unknown decision provenance remains inspectable but cannot mint guidance.
func authoredRuleInputSQL(prefix string) string {
	return `COALESCE(` + prefix + `domain,'') NOT IN ('memory-cognify','anti-pattern') AND ` + memoryUnexpiredAtSQL(prefix+`expires_at`, `CURRENT_TIMESTAMP`) + `
 AND NOT EXISTS(SELECT 1 FROM memory_lineage generated WHERE generated.object_type='rule' AND generated.object_id=` + prefix + `id
 AND (generated.source_kind='memory-cognify-input-v1' OR (generated.source_kind='metadata' AND generated.source_ref LIKE 'memory-cognify-rule-v1:%')))`
}
func currentLegacyRuleInputsSQL(objectType, prefix, digest string) string {
	collection := ""
	if objectType == "memory" {
		collection = ` AND observed.source_ref::jsonb->>'collection_revision'=(SELECT to_jsonb(o)->>'rules_revision' FROM memory_collection_owner o WHERE id=1)`
	}
	return `EXISTS(SELECT 1 FROM memory_lineage observed WHERE observed.object_type='` + objectType + `' AND observed.object_id=` + prefix + `id AND observed.source_kind='legacy-rule-input-v1')
 AND NOT EXISTS(SELECT 1 FROM memory_lineage observed LEFT JOIN rules source
 ON source.id::text=observed.source_ref::jsonb->>'record_id'
 AND source.record_revision::text=observed.source_ref::jsonb->>'record_revision'
 AND ` + authoredRuleInputSQL("source.") + `
 WHERE observed.object_type='` + objectType + `' AND observed.object_id=` + prefix + `id AND observed.source_kind='legacy-rule-input-v1'
 AND (source.id IS NULL OR (observed.source_ref::jsonb->>'schema_version'='1'
 AND observed.source_ref::jsonb->>'owner_id'=(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 AND observed.source_ref::jsonb->>'output_digest'=` + digest + collection + `) IS DISTINCT FROM TRUE))`
}
func memoryClaimDigestSQL(prefix string) string {
	row := prefix[:len(prefix)-1]
	return `encode(sha256(convert_to(jsonb_build_array(to_jsonb(` + row + `)->>'key',to_jsonb(` + row + `)->>'content')::text,'UTF8')),'hex')`
}
