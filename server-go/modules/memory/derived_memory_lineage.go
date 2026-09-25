package memory

// Every declared ancestor is checked under the caller's RLS context and the
// same statement snapshot. A bounded path walk detects cycles; exhausting either
// bound refuses eligibility instead of silently treating a prefix as complete.
// Existing card validation still verifies its producer snapshot and digest.
func currentDerivedMemoryInputsSQL(prefix string, historical bool) string {
	if prefix == "" {
		prefix = "memories."
	}
	policy := baseCurrentMemorySQL("lineage_parent.")
	if historical {
		policy = baseHistoricalMemoryInspectionSQL("lineage_parent.")
	}
	observation := `(CASE WHEN declared.source_kind='memory-cognify-input-v1' THEN declared.source_ref::jsonb END)`
	return `(CASE WHEN NOT EXISTS(SELECT 1 FROM memory_lineage declared
 WHERE declared.object_type='memory' AND declared.object_id=` + prefix + `id
 AND (declared.source_kind IN ('memory','memory-cognify-input-v1')
 OR (declared.source_kind='metadata' AND declared.source_ref LIKE 'memory-cognify-v1:%')))
 AND NOT EXISTS(SELECT 1 FROM memory_units card WHERE card.memory_id=` + prefix + `id AND card.is_episode_card=1 AND card.unit_type='episode_card')
 THEN TRUE ELSE (WITH RECURSIVE lineage_walk(id,revision,path,depth,cycle,valid) AS (
 SELECT ` + prefix + `id,` + prefix + `record_revision::text,ARRAY[` + prefix + `id],0,FALSE,TRUE
 UNION ALL
 SELECT (input.value->>'record_id')::bigint,input.value->>'record_revision',
 walk.path||(input.value->>'record_id')::bigint,walk.depth+1,
 (input.value->>'record_id')::bigint=ANY(walk.path),
 input.valid
 FROM lineage_walk walk CROSS JOIN LATERAL (
 SELECT ` + observation + ` AS value,
 (` + observation + `->>'schema_version'='1'
 AND ` + observation + `->>'owner_id'=(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)
 AND ` + observation + `->>'derived_revision'=walk.revision) AS valid
 FROM memory_lineage declared WHERE declared.object_type='memory' AND declared.object_id=walk.id
 AND declared.source_kind='memory-cognify-input-v1'
 UNION ALL
 SELECT card_input,TRUE FROM memory_units card JOIN memory_lineage card_observation
 ON card_observation.object_type='memory_unit' AND card_observation.object_id=card.id
 AND card_observation.source_kind='episode-card-input-v1'
 CROSS JOIN LATERAL jsonb_array_elements((CASE WHEN card_observation.source_kind='episode-card-input-v1'
 THEN card_observation.source_ref::jsonb END)->'inputs') card_input
 WHERE card.memory_id=walk.id AND card.is_episode_card=1 AND card.unit_type='episode_card'
 ) input WHERE NOT walk.cycle AND walk.depth<16
 ), bounded AS MATERIALIZED(SELECT * FROM lineage_walk LIMIT 257)
 SELECT (SELECT count(*) FROM bounded)<=256 AND NOT EXISTS(
 SELECT 1 FROM bounded walk LEFT JOIN LATERAL (
 SELECT lineage_parent.id FROM memories lineage_parent WHERE lineage_parent.id=walk.id
 AND lineage_parent.record_revision::text=walk.revision AND ` + policy + `
 AND ` + currentEpisodeCardInputsSQL("lineage_parent.", historical) + `
 LIMIT 1) allowed ON TRUE
 WHERE allowed.id IS NULL OR walk.cycle OR walk.valid IS DISTINCT FROM TRUE
 OR (walk.depth=16 AND (EXISTS(SELECT 1 FROM memory_lineage further
 WHERE further.object_type='memory' AND further.object_id=walk.id
 AND further.source_kind IN ('memory','memory-cognify-input-v1'))
 OR EXISTS(SELECT 1 FROM memory_units card WHERE card.memory_id=walk.id AND card.is_episode_card=1)))
 OR EXISTS(SELECT 1 FROM memory_lineage legacy WHERE legacy.object_type='memory'
 AND legacy.object_id=walk.id AND legacy.source_kind='memory'
 AND NOT EXISTS(SELECT 1 FROM memory_lineage declared WHERE declared.object_type='memory'
 AND declared.object_id=walk.id AND declared.source_kind='memory-cognify-input-v1'
 AND 'memory:'||(` + observation + `->>'record_id')=legacy.source_ref)
 AND NOT EXISTS(SELECT 1 FROM memory_units card JOIN memory_lineage card_observation
 ON card_observation.object_type='memory_unit' AND card_observation.object_id=card.id
 AND card_observation.source_kind='episode-card-input-v1'
 CROSS JOIN LATERAL jsonb_array_elements((CASE WHEN card_observation.source_kind='episode-card-input-v1'
 THEN card_observation.source_ref::jsonb END)->'inputs') card_input
 WHERE card.memory_id=walk.id AND card.is_episode_card=1 AND card.unit_type='episode_card'
 AND 'memory:'||(card_input->>'record_id')=legacy.source_ref))
 OR (EXISTS(SELECT 1 FROM memory_lineage producer WHERE producer.object_type='memory'
 AND producer.object_id=walk.id AND producer.source_kind='metadata' AND producer.source_ref LIKE 'memory-cognify-v1:%')
 AND NOT EXISTS(SELECT 1 FROM memory_lineage declared WHERE declared.object_type='memory'
 AND declared.object_id=walk.id AND declared.source_kind='memory-cognify-input-v1')))
 ) END)`
}
