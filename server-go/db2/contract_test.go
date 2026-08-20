package db2

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"os"
	"strings"
	"testing"
)

// replyVector is one positive reply fixture. It is named so that shared
// helpers can take a selector for the field they care about.
type replyVector struct {
	Flags                 uint32            `json:"flags"`
	Result                uint32            `json:"result"`
	Dimension             uint32            `json:"dimension"`
	Count                 uint64            `json:"count"`
	DeletedCount          uint32            `json:"deleted_count"`
	PrunedCount           uint32            `json:"pruned_count"`
	NormalizedCount       uint32            `json:"normalized_count"`
	ProjectCount          uint32            `json:"project_count"`
	PurgedCount           uint32            `json:"purged_count"`
	RequeuedCount         uint32            `json:"requeued_count"`
	ExpiredCount          uint32            `json:"expired_count"`
	DirectivesExpired     uint32            `json:"directives_expired"`
	MarkedCount           uint32            `json:"marked_count"`
	ResetCount            uint32            `json:"reset_count"`
	RequeuedRows          uint32            `json:"requeued_rows"`
	DemotedArtifacts      uint32            `json:"demoted_artifacts"`
	ReenqueuedOps         uint32            `json:"reenqueued_ops"`
	ExtractJobs           uint32            `json:"extract_jobs"`
	RouteCount            uint32            `json:"route_count"`
	IdentitiesWritten     uint32            `json:"identities_written"`
	BuildDepsWritten      uint32            `json:"build_deps_written"`
	RulesTouched          uint32            `json:"rules_touched"`
	ItemsRescored         uint32            `json:"items_rescored"`
	Acquired              uint32            `json:"acquired"`
	DriftCandidates       uint64            `json:"drift_candidates"`
	ReleaseID             uint64            `json:"release_id"`
	LastTraceID           uint64            `json:"last_trace_id"`
	ArchivedCount         uint32            `json:"archived_count"`
	Tagged                uint32            `json:"tagged"`
	InForce               uint32            `json:"in_force"`
	Present               uint32            `json:"present"`
	UpdatedRows           uint32            `json:"updated_rows"`
	Content               string            `json:"content"`
	SessionIDReply        string            `json:"session_id"`
	RefKey                string            `json:"ref_key"`
	MaxUpdatedAt          string            `json:"max_updated_at"`
	RowKey                string            `json:"key"`
	RowKind               string            `json:"kind"`
	RowUseCount           uint32            `json:"use_count"`
	Truncated             uint32            `json:"truncated"`
	Label                 string            `json:"label"`
	ConfidenceBits        uint64            `json:"confidence_binary64_bits"`
	SalienceBits          uint64            `json:"salience_binary64_bits"`
	Tier                  string            `json:"tier"`
	UseCases              string            `json:"use_cases"`
	LastUsedAt            string            `json:"last_used_at"`
	CreatedAt             string            `json:"created_at"`
	UpdatedAt             string            `json:"updated_at"`
	RowSourceSession      string            `json:"source_session"`
	ProvenanceCategory    string            `json:"provenance_category"`
	DeletedRows           uint32            `json:"deleted_rows"`
	DemotedCount          uint32            `json:"demoted_count"`
	AvgEffectivenessBits  uint64            `json:"avg_effectiveness_bits"`
	LowEffectivenessCount uint32            `json:"low_effectiveness_count"`
	HighImpactCount       uint32            `json:"high_impact_count"`
	MemoryIDs             []uint64          `json:"memory_ids"`
	SnapshotsDeleted      uint32            `json:"snapshots_deleted"`
	ContradictionsDeleted uint32            `json:"contradictions_deleted"`
	Counters              map[string]uint32 `json:"counters"`
	TierCounts            []uint32          `json:"tier_counts"`
	KindCounts            []uint32          `json:"kind_counts"`
	Total                 uint32            `json:"total"`
	Conflicts             uint32            `json:"conflicts"`
	Level0Deleted         uint32            `json:"level0_deleted"`
	StaleLevel1Deleted    uint32            `json:"stale_level1_deleted"`
	CascadedCount         uint32            `json:"cascaded_count"`
	PromotedCount         uint32            `json:"promoted_count"`
	ReclassifiedCount     uint32            `json:"reclassified_count"`
	Exists                uint32            `json:"exists"`
	Referenced            uint32            `json:"referenced"`
	Fresh                 uint32            `json:"fresh"`
	Confirmed             uint32            `json:"confirmed"`
	Rejected              uint32            `json:"rejected"`
	FenceActive           uint32            `json:"active"`
	Found                 uint32            `json:"found"`
	ID                    uint64            `json:"id"`
	Size                  uint32            `json:"size"`
	InUse                 uint32            `json:"in_use"`
	Waiters               uint32            `json:"waiters"`
	LeaseGrants           uint64            `json:"lease_grants"`
	LeaseTimeouts         uint64            `json:"lease_timeouts"`
	Stuck                 uint64            `json:"stuck"`
	Poisoned              uint64            `json:"poisoned"`
	RefusedCount          uint64            `json:"refused_count"`
	LastOffered           uint32            `json:"last_offered"`
	Available             uint32            `json:"available"`
	Active                uint32            `json:"active_connections"`
	Maximum               uint32            `json:"max_connections"`
	IsReplica             uint32            `json:"is_replica"`
	ReplicaLag            uint64            `json:"replica_lag_bytes"`
	TargetDim             uint32            `json:"target_dimension"`
	StartedEpoch          uint64            `json:"started_epoch"`
	WasInProgress         uint32            `json:"was_in_progress"`
	RecordedDim           uint32            `json:"recorded_dimension"`
	RunningDim            uint32            `json:"running_dimension"`
	ServingID             string            `json:"serving_id"`
	Hex                   string            `json:"hex"`
}

type wireBaseline struct {
	CatalogSHA256 string `json:"catalog_sha256"`
	WireVersion   uint32 `json:"wire_version"`
	BodyEnvelope  struct {
		HeaderLen uint32 `json:"header_len"`
		Request   struct {
			Positive string `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []struct {
				Result uint32 `json:"result"`
				Hex    string `json:"hex"`
			} `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"reply"`
	} `json:"body_envelope"`
	Operations []struct {
		Name    string `json:"name"`
		Family  string `json:"family"`
		Request struct {
			Positive                   string   `json:"positive"`
			SourceSession              string   `json:"source_session"`
			Key                        string   `json:"key"`
			Kind                       string   `json:"kind"`
			SessionID                  string   `json:"session_id"`
			Tokens                     string   `json:"tokens"`
			Cleared                    string   `json:"cleared"`
			TierA                      string   `json:"tier_a"`
			TierB                      string   `json:"tier_b"`
			MemoryID                   uint64   `json:"memory_id"`
			LinkID                     uint64   `json:"link_id"`
			AsOf                       string   `json:"as_of"`
			ScopeType                  string   `json:"scope_type"`
			Content                    string   `json:"content"`
			Workspace                  string   `json:"workspace"`
			HasValue                   uint32   `json:"has_value"`
			ValueBits                  uint64   `json:"value_bits"`
			ThresholdBits              uint64   `json:"threshold_bits"`
			LowThresholdBits           uint64   `json:"low_threshold_bits"`
			MaximumIDs                 uint32   `json:"maximum_ids"`
			Limit                      uint32   `json:"limit"`
			ScopeFlags                 uint32   `json:"scope_flags"`
			Term                       string   `json:"term"`
			AnchorID                   uint64   `json:"anchor_id"`
			AnchorZero                 string   `json:"anchor_zero"`
			UnitID                     uint64   `json:"unit_id"`
			NormalizedKey              string   `json:"normalized_key"`
			HideArchived               uint32   `json:"hide_archived"`
			Tier                       string   `json:"tier"`
			Unfiltered                 string   `json:"unfiltered"`
			EntitySeed                 string   `json:"entity_seed"`
			Keyword                    string   `json:"keyword"`
			Unselected                 string   `json:"unselected"`
			RecordID                   uint64   `json:"record_id"`
			DocumentID                 uint64   `json:"document_id"`
			LastTraceID                uint64   `json:"last_trace_id"`
			Pattern                    string   `json:"pattern"`
			SourceRef                  string   `json:"source_ref"`
			ArtifactID                 string   `json:"artifact_id"`
			Sink                       string   `json:"sink"`
			EntityID                   string   `json:"entity_id"`
			TurnID                     string   `json:"turn_id"`
			BlobRef                    string   `json:"blob_ref"`
			QueryNorm                  string   `json:"query_norm"`
			StateKey                   string   `json:"state_key"`
			FinishedAt                 string   `json:"finished_at"`
			JobID                      string   `json:"job_id"`
			State                      string   `json:"state"`
			Collection                 string   `json:"collection"`
			LastError                  string   `json:"last_error"`
			StateValue                 string   `json:"state_value"`
			Version                    string   `json:"version"`
			UpdatedAt                  string   `json:"updated_at"`
			Window                     string   `json:"window"`
			ContentHash                string   `json:"content_hash"`
			Scope                      string   `json:"scope"`
			FilePath                   string   `json:"file_path"`
			CertIssuer                 string   `json:"cert_issuer"`
			CertSerialNorm             string   `json:"cert_serial_norm"`
			Project                    string   `json:"project"`
			StaleL1Tier                string   `json:"stale_l1_tier"`
			MaximumKinds               uint32   `json:"maximum_kinds"`
			DemoteTier                 string   `json:"demote_tier"`
			SourceTier                 string   `json:"source_tier"`
			TargetTier                 string   `json:"target_tier"`
			Kinds                      []string `json:"kinds"`
			ConfidenceBits             uint64   `json:"confidence_bits"`
			UseCount                   uint32   `json:"use_count"`
			StableDays                 uint32   `json:"stable_days"`
			GatedKind                  string   `json:"gated_kind"`
			RequireApproval            uint32   `json:"require_approval"`
			OpenPositive               string   `json:"open_positive"`
			Approver                   string   `json:"approver"`
			Note                       string   `json:"note"`
			BarePositive               string   `json:"bare_positive"`
			Promotions                 uint32   `json:"promotions"`
			Demotions                  uint32   `json:"demotions"`
			Expirations                uint32   `json:"expirations"`
			ConflictWindowDays         uint32   `json:"conflict_window_days"`
			SnapshotRetentionDays      uint32   `json:"snapshot_retention_days"`
			ContradictionRetentionDays uint32   `json:"contradiction_retention_days"`
			PromoteUseCount            uint32   `json:"promote_use_count"`
			PromoteConfidenceBits      uint64   `json:"promote_confidence_bits"`
			DecisionPoint              string   `json:"decision_point"`
			RelType                    string   `json:"rel_type"`
			ProspectiveID              uint64   `json:"prospective_id"`
			NewState                   string   `json:"new_state"`
			GenerationID               uint64   `json:"generation_id"`
			ErrorMessage               string   `json:"error_message"`
			SourceHash                 string   `json:"source_hash"`
			ProjectID                  uint64   `json:"project_id"`
			PathGlob                   string   `json:"path_glob"`
			DecisionID                 uint64   `json:"decision_id"`
			Outcome                    string   `json:"outcome"`
			Status                     string   `json:"status"`
			RevisitWhen                string   `json:"revisit_when"`
			TaskID                     uint64   `json:"task_id"`
			IngestJobID                uint64   `json:"ingest_job_id"`
			DryRun                     uint32   `json:"dry_run"`
			RuleID                     uint32   `json:"rule_id"`
			ProposalID                 uint32   `json:"proposal_id"`
			RuleRowID                  uint32   `json:"rule_row_id"`
			MinRows                    uint32   `json:"min_rows"`
			MaxAttempts                uint32   `json:"max_attempts"`
			SceneMemoryID              uint64   `json:"scene_memory_id"`
			SceneID                    uint64   `json:"scene_id"`
			UnitIDA                    uint64   `json:"unit_id_a"`
			UnitIDB                    uint64   `json:"unit_id_b"`
			CitingArtifactID           string   `json:"citing_artifact_id"`
			SourceKind                 string   `json:"source_kind"`
			SourceID                   string   `json:"source_id"`
			FromArtifactID             string   `json:"from_artifact_id"`
			ToArtifactID               string   `json:"to_artifact_id"`
			LinkKind                   string   `json:"link_kind"`
			ArmID                      string   `json:"arm_id"`
			RollbackArm                string   `json:"rollback_arm"`
			RuleText                   string   `json:"rule_text"`
			RuleReason                 string   `json:"rule_reason"`
			ProposedBy                 string   `json:"proposed_by"`
			ReleaseID                  uint64   `json:"release_id"`
			DocID                      uint64   `json:"doc_id"`
			DirectiveID                uint64   `json:"directive_id"`
			ResolutionMemoryID         uint64   `json:"resolution_memory_id"`
			DirectiveType              string   `json:"directive_type"`
			FlagReason                 string   `json:"flag_reason"`
			VerdictTag                 string   `json:"verdict_tag"`
			VerdictScope               string   `json:"verdict_scope"`
			DocumentKey                string   `json:"document_key"`
			MappedTo                   string   `json:"mapped_to"`
			NowIso                     string   `json:"now_iso"`
			ReleaseName                string   `json:"release_name"`
			ExemplarProject            string   `json:"exemplar_project"`
			Basename                   string   `json:"basename"`
			Generation                 string   `json:"generation"`
			PurgeID                    string   `json:"purge_id"`
			FileHash                   string   `json:"file_hash"`
			ErrorLowered               string   `json:"error_lowered"`
			Entity                     string   `json:"entity"`
			RelationA                  string   `json:"relation_a"`
			RelationB                  string   `json:"relation_b"`
			OrderByWeight              uint32   `json:"order_by_weight"`
			Relation                   string   `json:"relation"`
			Query                      string   `json:"query"`
			Enrich                     uint32   `json:"enrich"`
			ExcludedProject            string   `json:"excluded_project"`
			Negative                   []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []replyVector `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"reply"`
	} `json:"operations"`
}

func TestBodyEnvelopeMatchesEverySharedCVector(t *testing.T) {
	envelope := loadWireBaseline(t).BodyEnvelope
	if envelope.HeaderLen != EnvelopeHeaderLen {
		t.Fatalf("header length = %d, generated Go = %d", envelope.HeaderLen, EnvelopeHeaderLen)
	}
	payload := []byte{0xaa, 0xbb, 0xcc}
	requestHeader, err := EncodeRequestHeader(0x01020304, 5, uint32(len(payload)))
	if err != nil {
		t.Fatalf("EncodeRequestHeader: %v", err)
	}
	request := append(requestHeader, payload...)
	wantRequest := decodeHex(t, envelope.Request.Positive)
	if string(request) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", request, wantRequest)
	}
	decodedRequest, err := DecodeRequestHeader(wantRequest)
	if err != nil || decodedRequest != (RequestHeader{
		Operation: 0x01020304, Flags: 5, PayloadLen: 3,
	}) {
		t.Fatalf("decoded request = (%+v, %v)", decodedRequest, err)
	}
	for _, vector := range envelope.Request.Negative {
		t.Run("request_"+vector.Mutation, func(t *testing.T) {
			header, err := DecodeRequestHeader(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || header != (RequestHeader{}) {
				t.Fatalf("request = (%+v, %v), want zero/malformed", header, err)
			}
		})
	}
	if header, err := EncodeRequestHeader(0, 0, 0); !errors.Is(err, ErrMalformedEnvelope) || header != nil {
		t.Fatalf("zero-operation request = (%x, %v)", header, err)
	}

	for _, vector := range envelope.Reply.Positive {
		vector := vector
		t.Run("reply_result_"+string(rune('0'+vector.Result)), func(t *testing.T) {
			header, err := EncodeReplyHeader(0x01020304, vector.Result, uint32(len(payload)))
			if err != nil {
				t.Fatalf("EncodeReplyHeader: %v", err)
			}
			reply := append(header, payload...)
			want := decodeHex(t, vector.Hex)
			if string(reply) != string(want) {
				t.Fatalf("reply = %x, want %x", reply, want)
			}
			decoded, err := DecodeReplyHeader(want)
			if err != nil || decoded != (ReplyHeader{
				Operation: 0x01020304, Result: vector.Result, PayloadLen: 3,
			}) {
				t.Fatalf("decoded reply = (%+v, %v)", decoded, err)
			}
		})
	}
	for _, vector := range envelope.Reply.Negative {
		t.Run("reply_"+vector.Mutation, func(t *testing.T) {
			header, err := DecodeReplyHeader(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || header != (ReplyHeader{}) {
				t.Fatalf("reply = (%+v, %v), want zero/malformed", header, err)
			}
		})
	}
	if header, err := EncodeReplyHeader(1, ResultInvalidState+1, 0); !errors.Is(err, ErrMalformedEnvelope) || header != nil {
		t.Fatalf("unknown-result reply = (%x, %v)", header, err)
	}
}

func decodeHex(t *testing.T, value string) []byte {
	t.Helper()
	decoded, err := hex.DecodeString(value)
	if err != nil {
		t.Fatalf("decode fixture %q: %v", value, err)
	}
	return decoded
}

// operationIndex finds an operation by name so that inserting one elsewhere in
// the catalog does not renumber every test below. The guard in
// TestBodyEnvelopeMatchesEverySharedCVector still pins the order by position;
// this is only about not repeating that pinning in each test.
func operationIndex(t *testing.T, name string) int {
	t.Helper()
	for index, operation := range loadWireBaseline(t).Operations {
		if operation.Name == name {
			return index
		}
	}
	t.Fatalf("no operation named %q in the wire baseline", name)
	return -1
}

func loadWireBaseline(t *testing.T) wireBaseline {
	t.Helper()
	raw, err := os.ReadFile("../../tests/baselines/modules/db2-wire-v1.json")
	if err != nil {
		t.Fatalf("read shared C/Go wire baseline: %v", err)
	}
	var baseline wireBaseline
	if err := json.Unmarshal(raw, &baseline); err != nil {
		t.Fatalf("decode shared C/Go wire baseline: %v", err)
	}
	if len(baseline.Operations) != 210 ||
		baseline.Operations[0].Name != "health" ||
		baseline.Operations[1].Name != "embedding_dimension" ||
		baseline.Operations[2].Name != "pool_status" ||
		baseline.Operations[3].Name != "embedding_refusals" ||
		baseline.Operations[4].Name != "postgres_status" ||
		baseline.Operations[5].Name != "reembed_status" ||
		baseline.Operations[6].Name != "reembed_clear" ||
		baseline.Operations[7].Name != "reembed_clear_maintenance" ||
		baseline.Operations[8].Name != "embedder_serving_id" ||
		baseline.Operations[9].Name != "dimension_reset" ||
		baseline.Operations[10].Name != "level3_count" ||
		baseline.Operations[11].Name != "level2_count" ||
		baseline.Operations[12].Name != "orphaned_l0_count" ||
		baseline.Operations[13].Name != "total_count" ||
		baseline.Operations[14].Name != "session_l2_count" ||
		baseline.Operations[15].Name != "key_exists" ||
		baseline.Operations[16].Name != "find_id_by_key_kind" ||
		baseline.Operations[17].Name != "key_exists_in_tier_pair" ||
		baseline.Operations[18].Name != "effectiveness_update" ||
		baseline.Operations[19].Name != "retention_enforce" ||
		baseline.Operations[20].Name != "effectiveness_demote" ||
		baseline.Operations[21].Name != "effectiveness_stats" ||
		baseline.Operations[22].Name != "l2_memory_ids" ||
		baseline.Operations[23].Name != "health_record" ||
		baseline.Operations[24].Name != "health_retention" ||
		baseline.Operations[25].Name != "health_counters" ||
		baseline.Operations[26].Name != "stats_counts" ||
		baseline.Operations[27].Name != "expire" ||
		baseline.Operations[28].Name != "demote" ||
		baseline.Operations[29].Name != "promote_stable" ||
		baseline.Operations[30].Name != "reclassify_directives" ||
		baseline.Operations[31].Name != "record_l4_approval" ||
		baseline.Operations[32].Name != "prune_orphaned_l0" ||
		baseline.Operations[33].Name != "lifecycle_sweep_expired" ||
		baseline.Operations[34].Name != "demote_id" ||
		baseline.Operations[35].Name != "has_workspace_tag" ||
		baseline.Operations[36].Name != "delete_row" ||
		baseline.Operations[37].Name != "touch" ||
		baseline.Operations[38].Name != "link_delete" ||
		baseline.Operations[39].Name != "valid_at" ||
		baseline.Operations[40].Name != "has_scope_type" ||
		baseline.Operations[41].Name != "reject" ||
		baseline.Operations[42].Name != "update_content" ||
		baseline.Operations[43].Name != "decay_confidence" ||
		baseline.Operations[44].Name != "workspace_tag_insert" ||
		baseline.Operations[45].Name != "set_cognified_kind" ||
		baseline.Operations[46].Name != "set_source_session" ||
		baseline.Operations[47].Name != "negation_tokens_update" ||
		baseline.Operations[48].Name != "get_content" ||
		baseline.Operations[49].Name != "get_source_session" ||
		baseline.Operations[50].Name != "pick_first_temporal_ref" ||
		baseline.Operations[51].Name != "count_and_max_updated" ||
		baseline.Operations[52].Name != "top_l2_facts" ||
		baseline.Operations[53].Name != "list_session_scope_priority" ||
		baseline.Operations[54].Name != "collect_alias_matches" ||
		baseline.Operations[55].Name != "collect_entity_matches" ||
		baseline.Operations[56].Name != "collect_event_frame_matches" ||
		baseline.Operations[57].Name != "collect_relation_token_matches" ||
		baseline.Operations[58].Name != "collect_summary_matches" ||
		baseline.Operations[59].Name != "collect_temporal_matches" ||
		baseline.Operations[60].Name != "find_facts_like" ||
		baseline.Operations[61].Name != "list_session_scope_priority_like" ||
		baseline.Operations[62].Name != "negation_fts_search" ||
		baseline.Operations[63].Name != "session_neighbors_before" ||
		baseline.Operations[64].Name != "session_neighbors_after" ||
		baseline.Operations[65].Name != "row_get" ||
		baseline.Operations[66].Name != "row_get_by_unit_id" ||
		baseline.Operations[67].Name != "search_facts_patterns_by_keyword" ||
		baseline.Operations[68].Name != "fact_history" ||
		baseline.Operations[69].Name != "list_rows" ||
		baseline.Operations[70].Name != "aggregate" ||
		baseline.Operations[71].Name != "load_eval_corpus" ||
		baseline.Operations[72].Name != "record_exists" ||
		baseline.Operations[73].Name != "prospective_set_state" ||
		baseline.Operations[74].Name != "dedupe_by_key" ||
		baseline.Operations[75].Name != "scene_member_exists" ||
		baseline.Operations[76].Name != "unit_edge_exists" ||
		baseline.Operations[77].Name != "match_error_keys" ||
		baseline.Operations[78].Name != "memory_ids_by_updated" ||
		baseline.Operations[79].Name != "unit_ids_for_memory" ||
		baseline.Operations[80].Name != "entity_edge_prune_orphans" ||
		baseline.Operations[81].Name != "entity_edge_normalize_weights" ||
		baseline.Operations[82].Name != "project_count" ||
		baseline.Operations[83].Name != "purge_hidden_pollution" ||
		baseline.Operations[84].Name != "requeue_drifted" ||
		baseline.Operations[85].Name != "cross_repo_rebuild_routes" ||
		baseline.Operations[86].Name != "cross_repo_rebuild_identities" ||
		baseline.Operations[87].Name != "cross_repo_rebuild_build_deps" ||
		baseline.Operations[88].Name != "drift_candidates" ||
		baseline.Operations[89].Name != "file_index_delete_project" ||
		baseline.Operations[90].Name != "entity_observation_count" ||
		baseline.Operations[91].Name != "entity_profile_fresh" ||
		baseline.Operations[92].Name != "project_fingerprint" ||
		baseline.Operations[93].Name != "visible_source_hash" ||
		baseline.Operations[94].Name != "entity_profile_card" ||
		baseline.Operations[95].Name != "generation_abort" ||
		baseline.Operations[96].Name != "generation_set_source_hash" ||
		baseline.Operations[97].Name != "generation_publish" ||
		baseline.Operations[98].Name != "purge_files_matching" ||
		baseline.Operations[99].Name != "file_index_delete_current_generation" ||
		baseline.Operations[100].Name != "project_delete" ||
		baseline.Operations[101].Name != "minhash_delete_current_generation" ||
		baseline.Operations[102].Name != "minhash_delete_file" ||
		baseline.Operations[103].Name != "project_current_generation" ||
		baseline.Operations[104].Name != "projection_generation_create" ||
		baseline.Operations[105].Name != "projection_visible_id" ||
		baseline.Operations[106].Name != "unique_file_basename" ||
		baseline.Operations[107].Name != "entity_neighbors" ||
		baseline.Operations[108].Name != "entity_neighbors_filtered" ||
		baseline.Operations[109].Name != "entity_outbound_neighbors" ||
		baseline.Operations[110].Name != "entity_top_partners" ||
		baseline.Operations[111].Name != "entity_top_targets" ||
		baseline.Operations[112].Name != "file_definitions" ||
		baseline.Operations[113].Name != "code_search" ||
		baseline.Operations[114].Name != "code_search_excluding_project" ||
		baseline.Operations[115].Name != "rules_decay" ||
		baseline.Operations[116].Name != "curiosity_rescore_all" ||
		baseline.Operations[117].Name != "mining_seed_job_defaults" ||
		baseline.Operations[118].Name != "proposals_archive_expired" ||
		baseline.Operations[119].Name != "trace_mining_last_id" ||
		baseline.Operations[120].Name != "anti_pattern_bump" ||
		baseline.Operations[121].Name != "anti_pattern_delete" ||
		baseline.Operations[122].Name != "trace_mining_record" ||
		baseline.Operations[123].Name != "anti_pattern_exists_exact" ||
		baseline.Operations[124].Name != "anti_pattern_exists_by_source_ref" ||
		baseline.Operations[125].Name != "artifact_citation_count" ||
		baseline.Operations[126].Name != "commits_in_last_7_days" ||
		baseline.Operations[127].Name != "fidelity_attribution_count" ||
		baseline.Operations[128].Name != "artifact_stamp_reflected" ||
		baseline.Operations[129].Name != "failed_query_bump" ||
		baseline.Operations[130].Name != "artifact_set_state" ||
		baseline.Operations[131].Name != "artifact_register_exemplar" ||
		baseline.Operations[132].Name != "evidence_enqueue" ||
		baseline.Operations[133].Name != "evidence_mark_failed" ||
		baseline.Operations[134].Name != "bandit_arms_list" ||
		baseline.Operations[135].Name != "bandit_promotion_get" ||
		baseline.Operations[136].Name != "decision_log_set_outcome" ||
		baseline.Operations[137].Name != "decision_log_set_status" ||
		baseline.Operations[138].Name != "decision_log_set_revisit" ||
		baseline.Operations[139].Name != "collab_rule_approve" ||
		baseline.Operations[140].Name != "collab_rule_reject" ||
		baseline.Operations[141].Name != "collab_rule_retire" ||
		baseline.Operations[142].Name != "proposal_bump_corroboration" ||
		baseline.Operations[143].Name != "proposal_mark_committed" ||
		baseline.Operations[144].Name != "rules_delete_by_id" ||
		baseline.Operations[145].Name != "calibration_surfaces_with_data" ||
		baseline.Operations[146].Name != "artifact_cite" ||
		baseline.Operations[147].Name != "artifact_link" ||
		baseline.Operations[148].Name != "bandit_promotion_set" ||
		baseline.Operations[149].Name != "collab_rule_propose" ||
		baseline.Operations[150].Name != "rules_delete_by_directive_type" ||
		baseline.Operations[151].Name != "artifact_flag_review" ||
		baseline.Operations[152].Name != "verdict_suppressed" ||
		baseline.Operations[153].Name != "curator_invalidate_doc" ||
		baseline.Operations[154].Name != "rel_types_ensure_seed" ||
		baseline.Operations[155].Name != "doc_delete" ||
		baseline.Operations[156].Name != "task_delete" ||
		baseline.Operations[157].Name != "clear_project" ||
		baseline.Operations[158].Name != "clear_current_project" ||
		baseline.Operations[159].Name != "document_exists" ||
		baseline.Operations[160].Name != "blob_referenced" ||
		baseline.Operations[161].Name != "fence_active" ||
		baseline.Operations[162].Name != "doc_exists_by_hash" ||
		baseline.Operations[163].Name != "pdf_quarantine_confirm" ||
		baseline.Operations[164].Name != "pdf_quarantine_reject" ||
		baseline.Operations[165].Name != "ontology_eval_status" ||
		baseline.Operations[166].Name != "task_update_state" ||
		baseline.Operations[167].Name != "release_add_doc" ||
		baseline.Operations[168].Name != "ontology_approve" ||
		baseline.Operations[169].Name != "ontology_reject" ||
		baseline.Operations[170].Name != "doc_assets_delete_for_doc" ||
		baseline.Operations[171].Name != "ontology_map" ||
		baseline.Operations[172].Name != "release_create" ||
		baseline.Operations[173].Name != "purge_fence_heartbeat" ||
		baseline.Operations[174].Name != "purge_fence_clear" ||
		baseline.Operations[175].Name != "document_stored_hash" ||
		baseline.Operations[176].Name != "document_hash_exists" ||
		baseline.Operations[177].Name != "pdf_tsr_state" ||
		baseline.Operations[178].Name != "document_chunk_ids" ||
		baseline.Operations[179].Name != "vector_rebuild_lock_try_acquire" ||
		baseline.Operations[180].Name != "vector_rebuild_lock_release" ||
		baseline.Operations[181].Name != "release_get_active" ||
		baseline.Operations[182].Name != "enrollment_active" ||
		baseline.Operations[183].Name != "prospective_sweep_expired" ||
		baseline.Operations[184].Name != "directive_sweep_expired" ||
		baseline.Operations[185].Name != "mark_revisit_due" ||
		baseline.Operations[186].Name != "ingest_queue_reset_running" ||
		baseline.Operations[187].Name != "evidence_reembed_all" ||
		baseline.Operations[188].Name != "curator_reembed_all" ||
		baseline.Operations[189].Name != "synth_reenqueue_all" ||
		baseline.Operations[190].Name != "curator_reenqueue_extract_all" ||
		baseline.Operations[191].Name != "directive_suppress" ||
		baseline.Operations[192].Name != "directive_record_surface" ||
		baseline.Operations[193].Name != "async_pending_count" ||
		baseline.Operations[194].Name != "runtime_state_touch" ||
		baseline.Operations[195].Name != "synth_enqueue" ||
		baseline.Operations[196].Name != "synth_mark_done" ||
		baseline.Operations[197].Name != "reembed_mark_finished" ||
		baseline.Operations[198].Name != "mining_job_try_lock" ||
		baseline.Operations[199].Name != "synth_mark_failed" ||
		baseline.Operations[200].Name != "runtime_state_set" ||
		baseline.Operations[201].Name != "set_active_embedder_version" ||
		baseline.Operations[202].Name != "runtime_state_get" ||
		baseline.Operations[203].Name != "ingest_queue_fail" ||
		baseline.Operations[204].Name != "reset_stuck_vector_ops" ||
		baseline.Operations[205].Name != "directive_resolve" ||
		baseline.Operations[206].Name != "css_migration_enumerate" ||
		baseline.Operations[207].Name != "css_migration_assert_conventions" ||
		baseline.Operations[208].Name != "css_migration_rules_doc" ||
		baseline.Operations[209].Name != "retryable_index_failures" {
		t.Fatalf("unexpected operations: %+v", baseline.Operations)
	}
	return baseline
}

func TestLevel3CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "level3_count")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeLevel3CountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeLevel3CountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeLevel3CountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeLevel3CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeLevel3CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeLevel3CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestLevel2CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "level2_count")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeLevel2CountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeLevel2CountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeLevel2CountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeLevel2CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeLevel2CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeLevel2CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestOrphanedL0CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "orphaned_l0_count")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeOrphanedL0CountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeOrphanedL0CountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeOrphanedL0CountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeOrphanedL0CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeOrphanedL0CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeOrphanedL0CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestPruneOrphanedL0MatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "prune_orphaned_l0")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodePruneOrphanedL0Request(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodePruneOrphanedL0Request(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodePruneOrphanedL0Request(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodePruneOrphanedL0Reply(vector.DeletedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		deleted, err := DecodePruneOrphanedL0Reply(got)
		if err != nil || deleted != vector.DeletedCount {
			t.Fatalf("decode = (%d, %v)", deleted, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		deleted, err := DecodePruneOrphanedL0Reply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || deleted != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, deleted, err)
		}
	}
}

func TestLifecycleSweepExpiredMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "lifecycle_sweep_expired")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeLifecycleSweepExpiredRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeLifecycleSweepExpiredRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeLifecycleSweepExpiredRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeLifecycleSweepExpiredReply(vector.ArchivedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		archived, err := DecodeLifecycleSweepExpiredReply(got)
		if err != nil || archived != vector.ArchivedCount {
			t.Fatalf("decode = (%d, %v)", archived, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		archived, err := DecodeLifecycleSweepExpiredReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || archived != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, archived, err)
		}
	}
}

func TestDemoteIDMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "demote_id")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeDemoteIDRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeDemoteIDRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeDemoteIDRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeDemoteIDReply(vector.DemotedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		demoted, err := DecodeDemoteIDReply(got)
		if err != nil || demoted != vector.DemotedCount {
			t.Fatalf("decode = (%d, %v)", demoted, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		demoted, err := DecodeDemoteIDReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || demoted != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, demoted, err)
		}
	}
}

func TestCuratorReenqueueExtractAllMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "curator_reenqueue_extract_all")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCuratorReenqueueExtractAllRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCuratorReenqueueExtractAllRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeSynthReenqueueAllRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("synth decoder accepted an extract request: %v", err)
	}
	if err := DecodeCuratorReembedAllRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("curator decoder accepted an extract request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCuratorReenqueueExtractAllRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCuratorReenqueueExtractAllReply(vector.ExtractJobs)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		jobs, err := DecodeCuratorReenqueueExtractAllReply(got)
		if err != nil || jobs != vector.ExtractJobs {
			t.Fatalf("decode = (%d, %v)", jobs, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		jobs, err := DecodeCuratorReenqueueExtractAllReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || jobs != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, jobs, err)
		}
	}
}

func TestSynthReenqueueAllMatchesEverySharedCVector(t *testing.T) {
	baseline := loadWireBaseline(t)
	operation := baseline.Operations[operationIndex(t, "synth_reenqueue_all")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeSynthReenqueueAllRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeSynthReenqueueAllRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeEvidenceReembedAllRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("evidence decoder accepted a synth request: %v", err)
	}
	if err := DecodeCuratorReembedAllRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("curator decoder accepted a synth request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeSynthReenqueueAllRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeSynthReenqueueAllReply(vector.ReenqueuedOps)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		ops, err := DecodeSynthReenqueueAllReply(got)
		if err != nil || ops != vector.ReenqueuedOps {
			t.Fatalf("decode = (%d, %v)", ops, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		ops, err := DecodeSynthReenqueueAllReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || ops != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, ops, err)
		}
	}
}

func TestCuratorReembedAllMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "curator_reembed_all")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCuratorReembedAllRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCuratorReembedAllRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeEvidenceReembedAllRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("evidence decoder accepted a curator request: %v", err)
	}
	if err := DecodeIngestQueueResetRunningRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("queue-reset decoder accepted a curator request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCuratorReembedAllRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCuratorReembedAllReply(vector.DemotedArtifacts)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		demoted, err := DecodeCuratorReembedAllReply(got)
		if err != nil || demoted != vector.DemotedArtifacts {
			t.Fatalf("decode = (%d, %v)", demoted, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		demoted, err := DecodeCuratorReembedAllReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || demoted != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, demoted, err)
		}
	}
}

func TestEvidenceReembedAllMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "evidence_reembed_all")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEvidenceReembedAllRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEvidenceReembedAllRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeProspectiveSweepExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prospective decoder accepted a reembed request: %v", err)
	}
	if err := DecodeMarkRevisitDueRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("revisit decoder accepted a reembed request: %v", err)
	}
	if err := DecodeIngestQueueResetRunningRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("queue-reset decoder accepted a reembed request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEvidenceReembedAllRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEvidenceReembedAllReply(vector.RequeuedRows)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		rows, err := DecodeEvidenceReembedAllReply(got)
		if err != nil || rows != vector.RequeuedRows {
			t.Fatalf("decode = (%d, %v)", rows, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		rows, err := DecodeEvidenceReembedAllReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || rows != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, rows, err)
		}
	}
}

func TestIngestQueueResetRunningMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "ingest_queue_reset_running")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeIngestQueueResetRunningRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeIngestQueueResetRunningRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeProspectiveSweepExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prospective decoder accepted a queue-reset request: %v", err)
	}
	if err := DecodeDirectiveSweepExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("directive decoder accepted a queue-reset request: %v", err)
	}
	if err := DecodeMarkRevisitDueRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("revisit decoder accepted a queue-reset request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeIngestQueueResetRunningRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeIngestQueueResetRunningReply(vector.ResetCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		reset, err := DecodeIngestQueueResetRunningReply(got)
		if err != nil || reset != vector.ResetCount {
			t.Fatalf("decode = (%d, %v)", reset, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		reset, err := DecodeIngestQueueResetRunningReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || reset != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, reset, err)
		}
	}
}

func TestMarkRevisitDueMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "mark_revisit_due")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeMarkRevisitDueRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeMarkRevisitDueRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeProspectiveSweepExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prospective decoder accepted a revisit request: %v", err)
	}
	if err := DecodeDirectiveSweepExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("directive decoder accepted a revisit request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeMarkRevisitDueRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeMarkRevisitDueReply(vector.MarkedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		marked, err := DecodeMarkRevisitDueReply(got)
		if err != nil || marked != vector.MarkedCount {
			t.Fatalf("decode = (%d, %v)", marked, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		marked, err := DecodeMarkRevisitDueReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || marked != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, marked, err)
		}
	}
}

func TestDirectiveSweepExpiredMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "directive_sweep_expired")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeDirectiveSweepExpiredRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeDirectiveSweepExpiredRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeProspectiveSweepExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prospective decoder accepted a directive request: %v", err)
	}
	// Operation 2 of the index family produces the same bytes. Asserting the
	// share rather than a separation keeps the envelope's actual reach honest.
	if err := DecodeEntityEdgeNormalizeWeightsRequest(wantRequest); err != nil {
		t.Fatalf("expected the index family's second operation to share these bytes: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeDirectiveSweepExpiredRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeDirectiveSweepExpiredReply(vector.DirectivesExpired)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		directives, err := DecodeDirectiveSweepExpiredReply(got)
		if err != nil || directives != vector.DirectivesExpired {
			t.Fatalf("decode = (%d, %v)", directives, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		directives, err := DecodeDirectiveSweepExpiredReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || directives != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, directives, err)
		}
	}
}

func TestProspectiveSweepExpiredMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "prospective_sweep_expired")]
	if operation.Family != "maintenance" {
		t.Fatalf("family = %q, want maintenance", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeProspectiveSweepExpiredRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeProspectiveSweepExpiredRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Operation numbers are unique per family, not globally, so the first
	// operation of the index family produces the same bytes and decodes here.
	// The stage an invocation arrives on is the only discriminator; the C
	// handler test pins that, and this asserts the wire consequence rather
	// than claiming a separation the envelope does not carry.
	if err := DecodeEntityEdgePruneOrphansRequest(wantRequest); err != nil {
		t.Fatalf("expected the index family's first operation to share these bytes: %v", err)
	}
	if err := DecodeRequeueDriftedRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("requeue decoder accepted a sweep request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeProspectiveSweepExpiredRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeProspectiveSweepExpiredReply(vector.ExpiredCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		expired, err := DecodeProspectiveSweepExpiredReply(got)
		if err != nil || expired != vector.ExpiredCount {
			t.Fatalf("decode = (%d, %v)", expired, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		expired, err := DecodeProspectiveSweepExpiredReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || expired != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, expired, err)
		}
	}
}

func TestByIDOperationsMatchEverySharedCVector(t *testing.T) {
	baseline := loadWireBaseline(t)
	for _, operation := range []struct {
		index  int
		name   string
		family string
		id     uint64
		encode func(uint64) ([]byte, error)
		decode func([]byte) (uint64, error)
		reply  func([]byte) error
	}{
		{operationIndex(t, "anti_pattern_bump"), "anti_pattern_bump", "learning", 41,
			EncodeAntiPatternBumpRequest, DecodeAntiPatternBumpRequest, DecodeAntiPatternBumpReply},
		{operationIndex(t, "anti_pattern_delete"), "anti_pattern_delete", "learning", 42,
			EncodeAntiPatternDeleteRequest, DecodeAntiPatternDeleteRequest, DecodeAntiPatternDeleteReply},
		{operationIndex(t, "doc_delete"), "doc_delete", "organization", 43,
			EncodeDocDeleteRequest, DecodeDocDeleteRequest, DecodeDocDeleteReply},
		{operationIndex(t, "task_delete"), "task_delete", "organization", 44,
			EncodeTaskDeleteRequest, DecodeTaskDeleteRequest, DecodeTaskDeleteReply},
	} {
		entry := baseline.Operations[operation.index]
		if entry.Name != operation.name || entry.Family != operation.family {
			t.Fatalf("operation %d = %s/%s, want %s/%s", operation.index, entry.Name,
				entry.Family, operation.name, operation.family)
		}
		want := decodeHex(t, entry.Request.Positive)
		got, err := operation.encode(operation.id)
		if err != nil || string(got) != string(want) {
			t.Fatalf("%s request = (%x, %v), want %x", operation.name, got, err, want)
		}
		if id, err := operation.decode(want); err != nil || id != operation.id {
			t.Fatalf("%s decode = (%d, %v)", operation.name, id, err)
		}
		for _, vector := range entry.Request.Negative {
			if _, err := operation.decode(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("%s negative request %s: %v", operation.name, vector.Mutation, err)
			}
		}
		if err := operation.reply(decodeHex(t, entry.Reply.Positive[0].Hex)); err != nil {
			t.Fatalf("%s positive reply: %v", operation.name, err)
		}
		for _, vector := range entry.Reply.Negative {
			if err := operation.reply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("%s negative reply %s: %v", operation.name, vector.Mutation, err)
			}
		}
	}
}

func TestDirectiveIDOperationsMatchEverySharedCVector(t *testing.T) {
	baseline := loadWireBaseline(t)
	suppress := baseline.Operations[operationIndex(t, "directive_suppress")]
	surface := baseline.Operations[operationIndex(t, "directive_record_surface")]
	if suppress.Family != "maintenance" || surface.Family != "maintenance" {
		t.Fatalf("families = %q/%q, want maintenance", suppress.Family, surface.Family)
	}
	wantSuppress := decodeHex(t, suppress.Request.Positive)
	got, err := EncodeDirectiveSuppressRequest(31)
	if err != nil || string(got) != string(wantSuppress) {
		t.Fatalf("suppress request = (%x, %v), want %x", got, err, wantSuppress)
	}
	if id, err := DecodeDirectiveSuppressRequest(wantSuppress); err != nil || id != 31 {
		t.Fatalf("suppress decode = (%d, %v)", id, err)
	}
	wantSurface := decodeHex(t, surface.Request.Positive)
	// Same payload shape on the same stage, so each decoder must refuse the
	// other: a surfacing read as a suppression would close the directive.
	if _, err := DecodeDirectiveSuppressRequest(wantSurface); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("suppress decoder accepted a surfacing request: %v", err)
	}
	if _, err := DecodeDirectiveRecordSurfaceRequest(wantSuppress); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("surfacing decoder accepted a suppression request: %v", err)
	}
	for _, operation := range []struct {
		name    string
		vectors []struct {
			Mutation string `json:"mutation"`
			Hex      string `json:"hex"`
		}
		decode func([]byte) (uint64, error)
	}{
		{"directive_suppress", suppress.Request.Negative, DecodeDirectiveSuppressRequest},
		{"directive_record_surface", surface.Request.Negative, DecodeDirectiveRecordSurfaceRequest},
	} {
		for _, vector := range operation.vectors {
			if _, err := operation.decode(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("%s negative request %s: %v", operation.name, vector.Mutation, err)
			}
		}
	}
	if err := DecodeDirectiveSuppressReply(decodeHex(t, suppress.Reply.Positive[0].Hex)); err != nil {
		t.Fatalf("positive suppress reply: %v", err)
	}
	if err := DecodeDirectiveRecordSurfaceReply(decodeHex(t, surface.Reply.Positive[0].Hex)); err != nil {
		t.Fatalf("positive surfacing reply: %v", err)
	}
	for _, vector := range suppress.Reply.Negative {
		if err := DecodeDirectiveSuppressReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative suppress reply %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range surface.Reply.Negative {
		if err := DecodeDirectiveRecordSurfaceReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative surfacing reply %s: %v", vector.Mutation, err)
		}
	}
}

func TestTraceMiningLastIDMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "trace_mining_last_id")]
	if operation.Family != "learning" {
		t.Fatalf("family = %q, want learning", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeTraceMiningLastIDRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeTraceMiningLastIDRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeProposalsArchiveExpiredRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("archive decoder accepted a watermark request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeTraceMiningLastIDRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeTraceMiningLastIDReply(vector.LastTraceID)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		watermark, err := DecodeTraceMiningLastIDReply(got)
		if err != nil || watermark != vector.LastTraceID {
			t.Fatalf("decode = (%d, %v)", watermark, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		watermark, err := DecodeTraceMiningLastIDReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || watermark != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, watermark, err)
		}
	}
}

func TestProposalsArchiveExpiredMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "proposals_archive_expired")]
	if operation.Family != "learning" {
		t.Fatalf("family = %q, want learning", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeProposalsArchiveExpiredRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeProposalsArchiveExpiredRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeMiningSeedJobDefaultsRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("mining seed decoder accepted an archive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeProposalsArchiveExpiredRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	if len(operation.Reply.Positive) != 1 {
		t.Fatalf("acknowledgement replies have exactly one positive form, got %d",
			len(operation.Reply.Positive))
	}
	want := decodeHex(t, operation.Reply.Positive[0].Hex)
	if got := EncodeProposalsArchiveExpiredReply(); string(got) != string(want) {
		t.Fatalf("reply = %x, want %x", got, want)
	}
	if err := DecodeProposalsArchiveExpiredReply(want); err != nil {
		t.Fatalf("positive reply: %v", err)
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeProposalsArchiveExpiredReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
}

func TestReleaseGetActiveMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "release_get_active")]
	if operation.Family != "custody" {
		t.Fatalf("family = %q, want custody", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeReleaseGetActiveRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeReleaseGetActiveRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeVectorRebuildLockTryAcquireRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("acquire decoder accepted a release-read request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeReleaseGetActiveRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeReleaseGetActiveReply(vector.ReleaseID)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		id, err := DecodeReleaseGetActiveReply(got)
		if err != nil || id != vector.ReleaseID {
			t.Fatalf("decode = (%d, %v)", id, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		id, err := DecodeReleaseGetActiveReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || id != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, id, err)
		}
	}
}

func TestVectorRebuildLockMatchesEverySharedCVector(t *testing.T) {
	baseline := loadWireBaseline(t)
	acquire := baseline.Operations[operationIndex(t, "vector_rebuild_lock_try_acquire")]
	release := baseline.Operations[operationIndex(t, "vector_rebuild_lock_release")]
	if acquire.Family != "custody" || release.Family != "custody" {
		t.Fatalf("families = %q/%q, want custody", acquire.Family, release.Family)
	}
	wantAcquire := decodeHex(t, acquire.Request.Positive)
	if got := EncodeVectorRebuildLockTryAcquireRequest(); string(got) != string(wantAcquire) {
		t.Fatalf("acquire request = %x, want %x", got, wantAcquire)
	}
	wantRelease := decodeHex(t, release.Request.Positive)
	if got := EncodeVectorRebuildLockReleaseRequest(); string(got) != string(wantRelease) {
		t.Fatalf("release request = %x, want %x", got, wantRelease)
	}
	// Each must refuse the other: a release read as an acquire would hand a
	// caller a lock it never asked for.
	if err := DecodeVectorRebuildLockTryAcquireRequest(wantRelease); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("acquire decoder accepted a release request: %v", err)
	}
	if err := DecodeVectorRebuildLockReleaseRequest(wantAcquire); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("release decoder accepted an acquire request: %v", err)
	}
	for _, vector := range acquire.Request.Negative {
		if err := DecodeVectorRebuildLockTryAcquireRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative acquire request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range release.Request.Negative {
		if err := DecodeVectorRebuildLockReleaseRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative release request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range acquire.Reply.Positive {
		got, err := EncodeVectorRebuildLockTryAcquireReply(vector.Acquired)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive acquire reply = (%x, %v)", got, err)
		}
		flag, err := DecodeVectorRebuildLockTryAcquireReply(got)
		if err != nil || flag != vector.Acquired {
			t.Fatalf("decode = (%d, %v)", flag, err)
		}
	}
	for _, vector := range acquire.Reply.Negative {
		flag, err := DecodeVectorRebuildLockTryAcquireReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || flag != 0 {
			t.Fatalf("negative acquire reply %s = (%d, %v)", vector.Mutation, flag, err)
		}
	}
	if len(release.Reply.Positive) != 1 {
		t.Fatalf("acknowledgement replies have exactly one positive form, got %d",
			len(release.Reply.Positive))
	}
	wantReleaseReply := decodeHex(t, release.Reply.Positive[0].Hex)
	if got := EncodeVectorRebuildLockReleaseReply(); string(got) != string(wantReleaseReply) {
		t.Fatalf("release reply = %x, want %x", got, wantReleaseReply)
	}
	for _, vector := range release.Reply.Negative {
		if err := DecodeVectorRebuildLockReleaseReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative release reply %s: %v", vector.Mutation, err)
		}
	}
}

func TestRelTypesEnsureSeedMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "rel_types_ensure_seed")]
	if operation.Family != "organization" {
		t.Fatalf("family = %q, want organization", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeRelTypesEnsureSeedRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeRelTypesEnsureSeedRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Four families now open, all with a first operation numbered 1, so these
	// bytes decode under each of them. The stage is the discriminator.
	if err := DecodeRulesDecayRequest(wantRequest); err != nil {
		t.Fatalf("expected the learning family's first operation to share these bytes: %v", err)
	}
	if err := DecodeMiningSeedJobDefaultsRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("mining seed decoder accepted a rel-types request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeRelTypesEnsureSeedRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	if len(operation.Reply.Positive) != 1 {
		t.Fatalf("acknowledgement replies have exactly one positive form, got %d",
			len(operation.Reply.Positive))
	}
	want := decodeHex(t, operation.Reply.Positive[0].Hex)
	if got := EncodeRelTypesEnsureSeedReply(); string(got) != string(want) {
		t.Fatalf("reply = %x, want %x", got, want)
	}
	if err := DecodeRelTypesEnsureSeedReply(want); err != nil {
		t.Fatalf("positive reply: %v", err)
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeRelTypesEnsureSeedReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
}

func TestMiningSeedJobDefaultsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "mining_seed_job_defaults")]
	if operation.Family != "learning" {
		t.Fatalf("family = %q, want learning", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeMiningSeedJobDefaultsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeMiningSeedJobDefaultsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeCuriosityRescoreAllRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("rescore decoder accepted a seed request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeMiningSeedJobDefaultsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	// The reply is an acknowledgement, so there is one positive form and no
	// value to compare: the envelope is the whole of it.
	if len(operation.Reply.Positive) != 1 {
		t.Fatalf("acknowledgement replies have exactly one positive form, got %d",
			len(operation.Reply.Positive))
	}
	want := decodeHex(t, operation.Reply.Positive[0].Hex)
	if got := EncodeMiningSeedJobDefaultsReply(); string(got) != string(want) {
		t.Fatalf("reply = %x, want %x", got, want)
	}
	if err := DecodeMiningSeedJobDefaultsReply(want); err != nil {
		t.Fatalf("positive reply: %v", err)
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeMiningSeedJobDefaultsReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
}

func TestCuriosityRescoreAllMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "curiosity_rescore_all")]
	if operation.Family != "learning" {
		t.Fatalf("family = %q, want learning", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCuriosityRescoreAllRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCuriosityRescoreAllRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeRulesDecayRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("decay decoder accepted a rescore request: %v", err)
	}
	// Operation 2 of the index and maintenance families shares these bytes.
	if err := DecodeEntityEdgeNormalizeWeightsRequest(wantRequest); err != nil {
		t.Fatalf("expected the index family's second operation to share these bytes: %v", err)
	}
	if err := DecodeDirectiveSweepExpiredRequest(wantRequest); err != nil {
		t.Fatalf("expected the maintenance family's second operation to share these bytes: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCuriosityRescoreAllRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCuriosityRescoreAllReply(vector.ItemsRescored)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		rescored, err := DecodeCuriosityRescoreAllReply(got)
		if err != nil || rescored != vector.ItemsRescored {
			t.Fatalf("decode = (%d, %v)", rescored, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		rescored, err := DecodeCuriosityRescoreAllReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || rescored != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, rescored, err)
		}
	}
}

func TestRulesDecayMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "rules_decay")]
	if operation.Family != "learning" {
		t.Fatalf("family = %q, want learning", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeRulesDecayRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeRulesDecayRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Three families now share operation number 1, so all three first
	// operations produce these same bytes. Asserting the share rather than a
	// separation keeps the envelope's actual reach honest; the stage is what
	// tells them apart, and the C handler test pins that.
	if err := DecodeEntityEdgePruneOrphansRequest(wantRequest); err != nil {
		t.Fatalf("expected the index family's first operation to share these bytes: %v", err)
	}
	if err := DecodeProspectiveSweepExpiredRequest(wantRequest); err != nil {
		t.Fatalf("expected the maintenance family's first operation to share these bytes: %v", err)
	}
	if err := DecodeCrossRepoRebuildBuildDepsRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("build-dep decoder accepted a decay request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeRulesDecayRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeRulesDecayReply(vector.RulesTouched)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		touched, err := DecodeRulesDecayReply(got)
		if err != nil || touched != vector.RulesTouched {
			t.Fatalf("decode = (%d, %v)", touched, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		touched, err := DecodeRulesDecayReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || touched != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, touched, err)
		}
	}
}

func TestDriftCandidatesMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "drift_candidates")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeDriftCandidatesRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeDriftCandidatesRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// It shares a predicate with requeue_drifted, not an operation number.
	if err := DecodeRequeueDriftedRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("requeue decoder accepted a drift-count request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeDriftCandidatesRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeDriftCandidatesReply(vector.DriftCandidates)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		drift, err := DecodeDriftCandidatesReply(got)
		if err != nil || drift != vector.DriftCandidates {
			t.Fatalf("decode = (%d, %v)", drift, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		drift, err := DecodeDriftCandidatesReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || drift != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, drift, err)
		}
	}
}

func TestCrossRepoRebuildBuildDepsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "cross_repo_rebuild_build_deps")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCrossRepoRebuildBuildDepsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCrossRepoRebuildBuildDepsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeCrossRepoRebuildIdentitiesRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("identity decoder accepted a build-dep request: %v", err)
	}
	// Operation 8 of the maintenance family shares these bytes; the stage
	// separates them, not the envelope.
	if err := DecodeCuratorReenqueueExtractAllRequest(wantRequest); err != nil {
		t.Fatalf("expected the maintenance family's eighth operation to share these bytes: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCrossRepoRebuildBuildDepsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCrossRepoRebuildBuildDepsReply(vector.BuildDepsWritten)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		deps, err := DecodeCrossRepoRebuildBuildDepsReply(got)
		if err != nil || deps != vector.BuildDepsWritten {
			t.Fatalf("decode = (%d, %v)", deps, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		deps, err := DecodeCrossRepoRebuildBuildDepsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || deps != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, deps, err)
		}
	}
}

func TestCrossRepoRebuildIdentitiesMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "cross_repo_rebuild_identities")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCrossRepoRebuildIdentitiesRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCrossRepoRebuildIdentitiesRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeCrossRepoRebuildRoutesRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("route decoder accepted an identity request: %v", err)
	}
	// Operation 7 of the maintenance family shares these bytes; the stage
	// separates them, not the envelope.
	if err := DecodeSynthReenqueueAllRequest(wantRequest); err != nil {
		t.Fatalf("expected the maintenance family's seventh operation to share these bytes: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCrossRepoRebuildIdentitiesRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCrossRepoRebuildIdentitiesReply(vector.IdentitiesWritten)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		written, err := DecodeCrossRepoRebuildIdentitiesReply(got)
		if err != nil || written != vector.IdentitiesWritten {
			t.Fatalf("decode = (%d, %v)", written, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		written, err := DecodeCrossRepoRebuildIdentitiesReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || written != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, written, err)
		}
	}
}

func TestCrossRepoRebuildRoutesMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "cross_repo_rebuild_routes")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCrossRepoRebuildRoutesRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCrossRepoRebuildRoutesRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	if err := DecodeRequeueDriftedRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("requeue decoder accepted a route-rebuild request: %v", err)
	}
	// Operation 6 of the maintenance family shares these bytes; the stage
	// separates them, not the envelope.
	if err := DecodeCuratorReembedAllRequest(wantRequest); err != nil {
		t.Fatalf("expected the maintenance family's sixth operation to share these bytes: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCrossRepoRebuildRoutesRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCrossRepoRebuildRoutesReply(vector.RouteCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		routes, err := DecodeCrossRepoRebuildRoutesReply(got)
		if err != nil || routes != vector.RouteCount {
			t.Fatalf("decode = (%d, %v)", routes, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		routes, err := DecodeCrossRepoRebuildRoutesReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || routes != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, routes, err)
		}
	}
}

func TestRequeueDriftedMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "requeue_drifted")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeRequeueDriftedRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeRequeueDriftedRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Every earlier index operation shares this stage and must refuse it.
	if err := DecodeEntityEdgePruneOrphansRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prune decoder accepted a requeue request: %v", err)
	}
	if err := DecodeEntityEdgeNormalizeWeightsRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("normalize decoder accepted a requeue request: %v", err)
	}
	if err := DecodeProjectCountRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("project_count decoder accepted a requeue request: %v", err)
	}
	if err := DecodePurgeHiddenPollutionRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("purge decoder accepted a requeue request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeRequeueDriftedRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeRequeueDriftedReply(vector.RequeuedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		requeued, err := DecodeRequeueDriftedReply(got)
		if err != nil || requeued != vector.RequeuedCount {
			t.Fatalf("decode = (%d, %v)", requeued, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		requeued, err := DecodeRequeueDriftedReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || requeued != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, requeued, err)
		}
	}
}

func TestPurgeHiddenPollutionMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "purge_hidden_pollution")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodePurgeHiddenPollutionRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodePurgeHiddenPollutionRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Every earlier index operation shares this stage and must refuse it.
	if err := DecodeEntityEdgePruneOrphansRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prune decoder accepted a purge request: %v", err)
	}
	if err := DecodeEntityEdgeNormalizeWeightsRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("normalize decoder accepted a purge request: %v", err)
	}
	if err := DecodeProjectCountRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("project_count decoder accepted a purge request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodePurgeHiddenPollutionRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodePurgeHiddenPollutionReply(vector.PurgedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		purged, err := DecodePurgeHiddenPollutionReply(got)
		if err != nil || purged != vector.PurgedCount {
			t.Fatalf("decode = (%d, %v)", purged, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		purged, err := DecodePurgeHiddenPollutionReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || purged != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, purged, err)
		}
	}
}

func TestProjectCountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "project_count")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeProjectCountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeProjectCountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Same family and stage as the two edge operations, different number.
	if err := DecodeEntityEdgePruneOrphansRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("prune decoder accepted a project_count request: %v", err)
	}
	if err := DecodeEntityEdgeNormalizeWeightsRequest(wantRequest); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("normalize decoder accepted a project_count request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeProjectCountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeProjectCountReply(vector.ProjectCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		projects, err := DecodeProjectCountReply(got)
		if err != nil || projects != vector.ProjectCount {
			t.Fatalf("decode = (%d, %v)", projects, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		projects, err := DecodeProjectCountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || projects != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, projects, err)
		}
	}
}

func TestEntityEdgeNormalizeWeightsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_edge_normalize_weights")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEntityEdgeNormalizeWeightsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEntityEdgeNormalizeWeightsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	// Same family and stage as the prune, different operation number, so this
	// decoder must reject the prune request.
	if err := DecodeEntityEdgeNormalizeWeightsRequest(EncodeEntityEdgePruneOrphansRequest()); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("normalize decoder accepted the prune request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEntityEdgeNormalizeWeightsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEntityEdgeNormalizeWeightsReply(vector.NormalizedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		normalized, err := DecodeEntityEdgeNormalizeWeightsReply(got)
		if err != nil || normalized != vector.NormalizedCount {
			t.Fatalf("decode = (%d, %v)", normalized, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		normalized, err := DecodeEntityEdgeNormalizeWeightsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || normalized != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, normalized, err)
		}
	}
	if EntityEdgeNormalizeWeightsScale != 100 {
		t.Fatalf("scale = %d, want 100", EntityEdgeNormalizeWeightsScale)
	}
}

func TestTopL2FactsMatchesEverySharedCVector(t *testing.T) {
	scopedIDListVectors(t, operationIndex(t, "top_l2_facts"), EncodeTopL2FactsRequest, DecodeTopL2FactsRequest,
		EncodeTopL2FactsReply, DecodeTopL2FactsReply)
}

func TestListSessionScopePriorityMatchesEverySharedCVector(t *testing.T) {
	scopedIDListVectors(t, operationIndex(t, "list_session_scope_priority"), EncodeListSessionScopePriorityRequest,
		DecodeListSessionScopePriorityRequest, EncodeListSessionScopePriorityReply,
		DecodeListSessionScopePriorityReply)
}

// scopedIDListVectors drives one db2-envelope-scoped-u32-u64-list-v1 operation
// through every vector in the baseline. The two operations differ only in which
// identifier they carry, so a shared body keeps them from drifting apart.
func scopedIDListVectors(t *testing.T, index int,
	encodeRequest func(uint32, uint32, string, string) ([]byte, error),
	decodeRequest func([]byte) (uint32, uint32, string, string, error),
	encodeReply func([]uint64) ([]byte, error),
	decodeReply func([]byte) ([]uint64, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(operation.Request.Limit, operation.Request.ScopeFlags,
		operation.Request.Workspace, operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	limit, scopeFlags, workspace, project, err := decodeRequest(request)
	if err != nil || limit != operation.Request.Limit ||
		scopeFlags != operation.Request.ScopeFlags ||
		workspace != operation.Request.Workspace || project != operation.Request.Project {
		t.Fatalf("request decode: %v %d %d %q %q", err, limit, scopeFlags, workspace, project)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := encodeReply(vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		decoded, err := decodeReply(reply)
		if err != nil || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %v", err, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestCollectAliasMatchesMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "collect_alias_matches"), EncodeCollectAliasMatchesRequest, DecodeCollectAliasMatchesRequest,
		EncodeCollectAliasMatchesReply, DecodeCollectAliasMatchesReply)
}

func TestCollectEntityMatchesMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "collect_entity_matches"), EncodeCollectEntityMatchesRequest, DecodeCollectEntityMatchesRequest,
		EncodeCollectEntityMatchesReply, DecodeCollectEntityMatchesReply)
}

func TestCollectEventFrameMatchesMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "collect_event_frame_matches"), EncodeCollectEventFrameMatchesRequest, DecodeCollectEventFrameMatchesRequest,
		EncodeCollectEventFrameMatchesReply, DecodeCollectEventFrameMatchesReply)
}

func TestCollectRelationTokenMatchesMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "collect_relation_token_matches"), EncodeCollectRelationTokenMatchesRequest, DecodeCollectRelationTokenMatchesRequest,
		EncodeCollectRelationTokenMatchesReply, DecodeCollectRelationTokenMatchesReply)
}

func TestCollectSummaryMatchesMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "collect_summary_matches"), EncodeCollectSummaryMatchesRequest, DecodeCollectSummaryMatchesRequest,
		EncodeCollectSummaryMatchesReply, DecodeCollectSummaryMatchesReply)
}

func TestCollectTemporalMatchesMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "collect_temporal_matches"), EncodeCollectTemporalMatchesRequest, DecodeCollectTemporalMatchesRequest,
		EncodeCollectTemporalMatchesReply, DecodeCollectTemporalMatchesReply)
}

// scopedTermListVectors drives one db2-envelope-scoped-string-u32-u64-list-v1
// operation through every vector in the baseline. Six operations share this
// format, so a shared body keeps their coverage from drifting apart.
func scopedTermListVectors(t *testing.T, index int,
	encodeRequest func(string, uint32, uint32, string, string) ([]byte, error),
	decodeRequest func([]byte) (string, uint32, uint32, string, string, error),
	encodeReply func([]uint64) ([]byte, error),
	decodeReply func([]byte) ([]uint64, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(operation.Request.Term, operation.Request.Limit,
		operation.Request.ScopeFlags, operation.Request.Workspace, operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	term, limit, scopeFlags, workspace, project, err := decodeRequest(request)
	if err != nil || term != operation.Request.Term || limit != operation.Request.Limit ||
		scopeFlags != operation.Request.ScopeFlags ||
		workspace != operation.Request.Workspace || project != operation.Request.Project {
		t.Fatalf("request decode: %v %q %d %d %q %q", err, term, limit, scopeFlags, workspace,
			project)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, _, _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := encodeReply(vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		decoded, err := decodeReply(reply)
		if err != nil || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %v", err, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestFindFactsLikeMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "find_facts_like"), EncodeFindFactsLikeRequest, DecodeFindFactsLikeRequest,
		EncodeFindFactsLikeReply, DecodeFindFactsLikeReply)
}

func TestListSessionScopePriorityLikeMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "list_session_scope_priority_like"), EncodeListSessionScopePriorityLikeRequest, DecodeListSessionScopePriorityLikeRequest,
		EncodeListSessionScopePriorityLikeReply, DecodeListSessionScopePriorityLikeReply)
}

func TestNegationFtsSearchMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "negation_fts_search"), EncodeNegationFtsSearchRequest, DecodeNegationFtsSearchRequest,
		EncodeNegationFtsSearchReply, DecodeNegationFtsSearchReply)
}

func TestSessionNeighborsBeforeMatchesEverySharedCVector(t *testing.T) {
	sessionWalkVectors(t, operationIndex(t, "session_neighbors_before"), EncodeSessionNeighborsBeforeRequest, DecodeSessionNeighborsBeforeRequest,
		EncodeSessionNeighborsBeforeReply, DecodeSessionNeighborsBeforeReply)
}

func TestSessionNeighborsAfterMatchesEverySharedCVector(t *testing.T) {
	sessionWalkVectors(t, operationIndex(t, "session_neighbors_after"), EncodeSessionNeighborsAfterRequest, DecodeSessionNeighborsAfterRequest,
		EncodeSessionNeighborsAfterReply, DecodeSessionNeighborsAfterReply)
}

// sessionWalkVectors drives one db2-envelope-string-u64-u32-u64-list-v1
// operation through every vector in the baseline, including the zero anchor --
// which is the one value the two operations answer differently, and so is a
// second positive rather than a mutation.
func sessionWalkVectors(t *testing.T, index int,
	encodeRequest func(string, uint64, uint32) ([]byte, error),
	decodeRequest func([]byte) (string, uint64, uint32, error),
	encodeReply func([]uint64) ([]byte, error),
	decodeReply func([]byte) ([]uint64, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(operation.Request.SessionID, operation.Request.AnchorID,
		operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	sessionID, anchorID, limit, err := decodeRequest(request)
	if err != nil || sessionID != operation.Request.SessionID ||
		anchorID != operation.Request.AnchorID || limit != operation.Request.Limit {
		t.Fatalf("request decode: %v %q %d %d", err, sessionID, anchorID, limit)
	}

	zero, err := encodeRequest(operation.Request.SessionID, 0, operation.Request.Limit)
	if err != nil || hex.EncodeToString(zero) != operation.Request.AnchorZero {
		t.Fatalf("zero-anchor encode: %v %x", err, zero)
	}
	if _, anchorID, _, err := decodeRequest(zero); err != nil || anchorID != 0 {
		t.Fatalf("zero-anchor decode: %v %d", err, anchorID)
	}

	for _, vector := range operation.Request.Negative {
		if _, _, _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := encodeReply(vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		decoded, err := decodeReply(reply)
		if err != nil || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %v", err, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestRowGetMatchesEverySharedCVector(t *testing.T) {
	memoryRowVectors(t, operationIndex(t, "row_get"), loadWireBaseline(t).Operations[operationIndex(t, "row_get")].Request.MemoryID,
		EncodeRowGetRequest, DecodeRowGetRequest, EncodeRowGetReply, DecodeRowGetReply)
}

func TestRowGetByUnitIDMatchesEverySharedCVector(t *testing.T) {
	memoryRowVectors(t, operationIndex(t, "row_get_by_unit_id"), loadWireBaseline(t).Operations[operationIndex(t, "row_get_by_unit_id")].Request.UnitID,
		EncodeRowGetByUnitIDRequest, DecodeRowGetByUnitIDRequest, EncodeRowGetByUnitIDReply,
		DecodeRowGetByUnitIDReply)
}

// memoryRowVectors drives one db2-envelope-u64-memory-row-v1 operation through
// every vector. The row is the only reply on this contract with fourteen
// fields, so every one is compared rather than a count.
func memoryRowVectors(t *testing.T, index int, want uint64,
	encodeRequest func(uint64) ([]byte, error),
	decodeRequest func([]byte) (uint64, error),
	encodeReply func(uint32, *MemoryRow) ([]byte, error),
	decodeReply func([]byte) (uint32, *MemoryRow, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(want)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	if got, err := decodeRequest(request); err != nil || got != want {
		t.Fatalf("request decode: %v %d", err, got)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		var row *MemoryRow
		if vector.Result == ResultOK {
			row = &MemoryRow{
				ID:                 vector.ID,
				Confidence:         math.Float64frombits(vector.ConfidenceBits),
				Salience:           math.Float64frombits(vector.SalienceBits),
				UseCount:           vector.RowUseCount,
				Tier:               vector.Tier,
				Kind:               vector.RowKind,
				Key:                vector.RowKey,
				Content:            vector.Content,
				UseCases:           vector.UseCases,
				LastUsedAt:         vector.LastUsedAt,
				CreatedAt:          vector.CreatedAt,
				UpdatedAt:          vector.UpdatedAt,
				SourceSession:      vector.RowSourceSession,
				ProvenanceCategory: vector.ProvenanceCategory,
			}
		}
		reply, err := encodeReply(vector.Result, row)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		result, decoded, err := decodeReply(reply)
		if err != nil || result != vector.Result {
			t.Fatalf("reply decode: %v %d", err, result)
		}
		if row == nil {
			if decoded != nil {
				t.Fatalf("absent row decoded to %+v", decoded)
			}
			continue
		}
		if decoded == nil || *decoded != *row {
			t.Fatalf("reply decode: %+v, want %+v", decoded, row)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestSearchFactsPatternsByKeywordMatchesEverySharedCVector(t *testing.T) {
	scopedTermListVectors(t, operationIndex(t, "search_facts_patterns_by_keyword"), EncodeSearchFactsPatternsByKeywordRequest,
		DecodeSearchFactsPatternsByKeywordRequest, EncodeSearchFactsPatternsByKeywordReply,
		DecodeSearchFactsPatternsByKeywordReply)
}

// TestFactHistoryMatchesEverySharedCVector covers the one search that carries no
// scope, so it has its own body rather than sharing the scoped one.
func TestFactHistoryMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "fact_history")]

	request, err := EncodeFactHistoryRequest(operation.Request.NormalizedKey,
		operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	key, limit, err := DecodeFactHistoryRequest(request)
	if err != nil || key != operation.Request.NormalizedKey || limit != operation.Request.Limit {
		t.Fatalf("request decode: %v %q %d", err, key, limit)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeFactHistoryRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := EncodeFactHistoryReply(vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		decoded, err := DecodeFactHistoryReply(reply)
		if err != nil || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %v", err, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := DecodeFactHistoryReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

// TestListRowsMatchesEverySharedCVector covers the assembled listing. The
// all-empty request is a second positive rather than a mutation: it is what "no
// filter at all" looks like, and it is a different statement from the filtered
// one.
func TestListRowsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "list_rows")]

	request, err := EncodeListRowsRequest(operation.Request.Limit, operation.Request.ScopeFlags,
		operation.Request.HideArchived, operation.Request.Tier, operation.Request.Kind,
		operation.Request.Workspace, operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	limit, scopeFlags, hideArchived, tier, kind, workspace, project, err :=
		DecodeListRowsRequest(request)
	if err != nil || limit != operation.Request.Limit ||
		scopeFlags != operation.Request.ScopeFlags ||
		hideArchived != operation.Request.HideArchived || tier != operation.Request.Tier ||
		kind != operation.Request.Kind || workspace != operation.Request.Workspace ||
		project != operation.Request.Project {
		t.Fatalf("request decode: %v %d %d %d %q %q %q %q", err, limit, scopeFlags, hideArchived,
			tier, kind, workspace, project)
	}

	unfiltered, err := EncodeListRowsRequest(operation.Request.Limit, 0, 0, "", "", "", "")
	if err != nil || hex.EncodeToString(unfiltered) != operation.Request.Unfiltered {
		t.Fatalf("unfiltered encode: %v %x", err, unfiltered)
	}
	if _, _, _, tier, kind, _, _, err := DecodeListRowsRequest(unfiltered); err != nil ||
		tier != "" || kind != "" {
		t.Fatalf("unfiltered decode: %v %q %q", err, tier, kind)
	}

	for _, vector := range operation.Request.Negative {
		if _, _, _, _, _, _, _, err := DecodeListRowsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := EncodeListRowsReply(vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		decoded, err := DecodeListRowsReply(reply)
		if err != nil || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %v", err, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := DecodeListRowsReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

// TestAggregateMatchesEverySharedCVector covers the aggregation. The
// both-selectors-empty request is a second positive rather than a mutation: it
// is the third of the three statements hiding behind this operation.
func TestAggregateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "aggregate")]

	request, err := EncodeAggregateRequest(operation.Request.EntitySeed,
		operation.Request.Keyword, operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entitySeed, keyword, limit, err := DecodeAggregateRequest(request)
	if err != nil || entitySeed != operation.Request.EntitySeed ||
		keyword != operation.Request.Keyword || limit != operation.Request.Limit {
		t.Fatalf("request decode: %v %q %q %d", err, entitySeed, keyword, limit)
	}

	unselected, err := EncodeAggregateRequest("", "", operation.Request.Limit)
	if err != nil || hex.EncodeToString(unselected) != operation.Request.Unselected {
		t.Fatalf("unselected encode: %v %x", err, unselected)
	}
	if seed, word, _, err := DecodeAggregateRequest(unselected); err != nil || seed != "" ||
		word != "" {
		t.Fatalf("unselected decode: %v %q %q", err, seed, word)
	}

	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeAggregateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := EncodeAggregateReply(vector.Truncated, vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		truncated, decoded, err := DecodeAggregateReply(reply)
		if err != nil || truncated != vector.Truncated || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %d %v", err, truncated, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, _, err := DecodeAggregateReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

// TestLoadEvalCorpusMatchesEverySharedCVector covers the corpus loader,
// including the empty label that means no plan matched.
func TestLoadEvalCorpusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "load_eval_corpus")]

	request, err := EncodeLoadEvalCorpusRequest(operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	if limit, err := DecodeLoadEvalCorpusRequest(request); err != nil ||
		limit != operation.Request.Limit {
		t.Fatalf("request decode: %v %d", err, limit)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeLoadEvalCorpusRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := EncodeLoadEvalCorpusReply(vector.Label, vector.MemoryIDs)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		label, decoded, err := DecodeLoadEvalCorpusReply(reply)
		if err != nil || label != vector.Label || len(decoded) != len(vector.MemoryIDs) {
			t.Fatalf("reply decode: %v %q %v", err, label, decoded)
		}
		for position, id := range vector.MemoryIDs {
			if decoded[position] != id {
				t.Fatalf("reply decode position %d: %d", position, decoded[position])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, _, err := DecodeLoadEvalCorpusReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestRecordExistsMatchesEverySharedCVector(t *testing.T) {
	existenceProbeVectors(t, operationIndex(t, "record_exists"), loadWireBaseline(t).Operations[operationIndex(t, "record_exists")].Request.RecordID,
		EncodeRecordExistsRequest, DecodeRecordExistsRequest, EncodeRecordExistsReply,
		DecodeRecordExistsReply)
}

func TestDocumentExistsMatchesEverySharedCVector(t *testing.T) {
	existenceProbeVectors(t, operationIndex(t, "document_exists"),
		loadWireBaseline(t).Operations[operationIndex(t, "document_exists")].Request.DocumentID,
		EncodeDocumentExistsRequest, DecodeDocumentExistsRequest, EncodeDocumentExistsReply,
		DecodeDocumentExistsReply)
}

// existenceProbeVectors drives one db2-envelope-u64-u32-v1 operation through
// every vector. Both positives matter: a probe that could only say yes would
// pass a test that only checked the yes.
func existenceProbeVectors(t *testing.T, index int, want uint64,
	encodeRequest func(uint64) ([]byte, error),
	decodeRequest func([]byte) (uint64, error),
	encodeReply func(uint32) ([]byte, error),
	decodeReply func([]byte) (uint32, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(want)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	if got, err := decodeRequest(request); err != nil || got != want {
		t.Fatalf("request decode: %v %d", err, got)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := encodeReply(vector.Exists)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		if got, err := decodeReply(reply); err != nil || got != vector.Exists {
			t.Fatalf("reply decode: %v %d", err, got)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

// TestTraceMiningRecordMatchesEverySharedCVector covers the watermark write,
// whose reply carries nothing at all.
func TestTraceMiningRecordMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "trace_mining_record")]

	request, err := EncodeTraceMiningRecordRequest(operation.Request.LastTraceID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	if got, err := DecodeTraceMiningRecordRequest(request); err != nil ||
		got != operation.Request.LastTraceID {
		t.Fatalf("request decode: %v %d", err, got)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeTraceMiningRecordRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := EncodeTraceMiningRecordReply()
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		if err := DecodeTraceMiningRecordReply(reply); err != nil {
			t.Fatalf("reply decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeTraceMiningRecordReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestAntiPatternExistsExactMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "anti_pattern_exists_exact"),
		loadWireBaseline(t).Operations[operationIndex(t, "anti_pattern_exists_exact")].Request.Pattern,
		EncodeAntiPatternExistsExactRequest, DecodeAntiPatternExistsExactRequest,
		EncodeAntiPatternExistsExactReply, DecodeAntiPatternExistsExactReply)
}

func TestAntiPatternExistsBySourceRefMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "anti_pattern_exists_by_source_ref"),
		loadWireBaseline(t).Operations[operationIndex(t, "anti_pattern_exists_by_source_ref")].Request.SourceRef,
		EncodeAntiPatternExistsBySourceRefRequest, DecodeAntiPatternExistsBySourceRefRequest,
		EncodeAntiPatternExistsBySourceRefReply, DecodeAntiPatternExistsBySourceRefReply)
}

func TestArtifactCitationCountMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "artifact_citation_count"),
		loadWireBaseline(t).Operations[operationIndex(t, "artifact_citation_count")].Request.ArtifactID,
		EncodeArtifactCitationCountRequest, DecodeArtifactCitationCountRequest,
		EncodeArtifactCitationCountReply, DecodeArtifactCitationCountReply)
}

func TestCommitsInLast7DaysMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "commits_in_last_7_days"),
		loadWireBaseline(t).Operations[operationIndex(t, "commits_in_last_7_days")].Request.Sink,
		EncodeCommitsInLast7DaysRequest, DecodeCommitsInLast7DaysRequest,
		EncodeCommitsInLast7DaysReply, DecodeCommitsInLast7DaysReply)
}

// singleStringCountVectors drives one db2-envelope-string-u32-v1 operation
// through every vector. The zero answer is a positive too: a probe that could
// only say yes would pass a test that only checked the yes.
func singleStringCountVectors(t *testing.T, index int, want string,
	encodeRequest func(string) ([]byte, error),
	decodeRequest func([]byte) (string, error),
	encodeReply func(uint32) ([]byte, error),
	decodeReply func([]byte) (uint32, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(want)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	if got, err := decodeRequest(request); err != nil || got != want {
		t.Fatalf("request decode: %v %q", err, got)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		// The fixture names the reply field after what it means, so a probe
		// fills exists and a count fills count; only one is ever present.
		answer := uint32(vector.Count)
		if answer == 0 {
			answer = vector.Exists
		}
		if answer == 0 {
			answer = vector.Referenced
		}
		if answer == 0 {
			answer = vector.FenceActive
		}
		if answer == 0 {
			answer = vector.Acquired
		}
		reply, err := encodeReply(answer)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		if got, err := decodeReply(reply); err != nil || got != answer {
			t.Fatalf("reply decode: %v %d", err, got)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestEntityObservationCountMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "entity_observation_count"),
		loadWireBaseline(t).Operations[operationIndex(t, "entity_observation_count")].Request.EntityID,
		EncodeEntityObservationCountRequest, DecodeEntityObservationCountRequest,
		EncodeEntityObservationCountReply, DecodeEntityObservationCountReply)
}

func TestFidelityAttributionCountMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "fidelity_attribution_count"),
		loadWireBaseline(t).Operations[operationIndex(t, "fidelity_attribution_count")].Request.TurnID,
		EncodeFidelityAttributionCountRequest, DecodeFidelityAttributionCountRequest,
		EncodeFidelityAttributionCountReply, DecodeFidelityAttributionCountReply)
}

func TestBlobReferencedMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "blob_referenced"),
		loadWireBaseline(t).Operations[operationIndex(t, "blob_referenced")].Request.BlobRef,
		EncodeBlobReferencedRequest, DecodeBlobReferencedRequest,
		EncodeBlobReferencedReply, DecodeBlobReferencedReply)
}

func TestAsyncPendingCountMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "async_pending_count"),
		loadWireBaseline(t).Operations[operationIndex(t, "async_pending_count")].Request.Kind,
		EncodeAsyncPendingCountRequest, DecodeAsyncPendingCountRequest,
		EncodeAsyncPendingCountReply, DecodeAsyncPendingCountReply)
}

func TestArtifactStampReflectedMatchesEverySharedCVector(t *testing.T) {
	singleStringAckVectors(t, operationIndex(t, "artifact_stamp_reflected"),
		loadWireBaseline(t).Operations[operationIndex(t, "artifact_stamp_reflected")].Request.ArtifactID,
		EncodeArtifactStampReflectedRequest, DecodeArtifactStampReflectedRequest,
		EncodeArtifactStampReflectedReply, DecodeArtifactStampReflectedReply)
}

func TestFailedQueryBumpMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "failed_query_bump"),
		loadWireBaseline(t).Operations[operationIndex(t, "failed_query_bump")].Request.QueryNorm,
		EncodeFailedQueryBumpRequest, DecodeFailedQueryBumpRequest,
		EncodeFailedQueryBumpReply, DecodeFailedQueryBumpReply)
}

func TestFenceActiveMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "fence_active"),
		loadWireBaseline(t).Operations[operationIndex(t, "fence_active")].Request.Project,
		EncodeFenceActiveRequest, DecodeFenceActiveRequest,
		EncodeFenceActiveReply, DecodeFenceActiveReply)
}

func TestRuntimeStateTouchMatchesEverySharedCVector(t *testing.T) {
	singleStringAckVectors(t, operationIndex(t, "runtime_state_touch"),
		loadWireBaseline(t).Operations[operationIndex(t, "runtime_state_touch")].Request.StateKey,
		EncodeRuntimeStateTouchRequest, DecodeRuntimeStateTouchRequest,
		EncodeRuntimeStateTouchReply, DecodeRuntimeStateTouchReply)
}

func TestSynthEnqueueMatchesEverySharedCVector(t *testing.T) {
	singleStringAckVectors(t, operationIndex(t, "synth_enqueue"),
		loadWireBaseline(t).Operations[operationIndex(t, "synth_enqueue")].Request.ArtifactID,
		EncodeSynthEnqueueRequest, DecodeSynthEnqueueRequest,
		EncodeSynthEnqueueReply, DecodeSynthEnqueueReply)
}

func TestSynthMarkDoneMatchesEverySharedCVector(t *testing.T) {
	singleStringAckVectors(t, operationIndex(t, "synth_mark_done"),
		loadWireBaseline(t).Operations[operationIndex(t, "synth_mark_done")].Request.ArtifactID,
		EncodeSynthMarkDoneRequest, DecodeSynthMarkDoneRequest,
		EncodeSynthMarkDoneReply, DecodeSynthMarkDoneReply)
}

func TestReembedMarkFinishedMatchesEverySharedCVector(t *testing.T) {
	singleStringAckVectors(t, operationIndex(t, "reembed_mark_finished"),
		loadWireBaseline(t).Operations[operationIndex(t, "reembed_mark_finished")].Request.FinishedAt,
		EncodeReembedMarkFinishedRequest, DecodeReembedMarkFinishedRequest,
		EncodeReembedMarkFinishedReply, DecodeReembedMarkFinishedReply)
}

func TestMiningJobTryLockMatchesEverySharedCVector(t *testing.T) {
	singleStringCountVectors(t, operationIndex(t, "mining_job_try_lock"),
		loadWireBaseline(t).Operations[operationIndex(t, "mining_job_try_lock")].Request.JobID,
		EncodeMiningJobTryLockRequest, DecodeMiningJobTryLockRequest,
		EncodeMiningJobTryLockReply, DecodeMiningJobTryLockReply)
}

// singleStringAckVectors drives one db2-envelope-string-ack-v1 operation through
// every vector. The reply carries nothing, so what the positive proves is that
// the acknowledgement is the exact empty envelope and not merely a short one.
func singleStringAckVectors(t *testing.T, index int, want string,
	encodeRequest func(string) ([]byte, error),
	decodeRequest func([]byte) (string, error),
	encodeReply func() ([]byte, error),
	decodeReply func([]byte) error) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(want)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	if got, err := decodeRequest(request); err != nil || got != want {
		t.Fatalf("request decode: %v %q", err, got)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := encodeReply()
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		if err := decodeReply(reply); err != nil {
			t.Fatalf("reply decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestArtifactSetStateMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "artifact_set_state"),
		loadWireBaseline(t).Operations[operationIndex(t, "artifact_set_state")].Request.State,
		loadWireBaseline(t).Operations[operationIndex(t, "artifact_set_state")].Request.ArtifactID,
		EncodeArtifactSetStateRequest, DecodeArtifactSetStateRequest,
		EncodeArtifactSetStateReply, DecodeArtifactSetStateReply)
}

func TestArtifactRegisterExemplarMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "artifact_register_exemplar"),
		loadWireBaseline(t).Operations[operationIndex(t, "artifact_register_exemplar")].Request.ArtifactID,
		loadWireBaseline(t).Operations[operationIndex(t, "artifact_register_exemplar")].Request.Collection,
		EncodeArtifactRegisterExemplarRequest, DecodeArtifactRegisterExemplarRequest,
		EncodeArtifactRegisterExemplarReply, DecodeArtifactRegisterExemplarReply)
}

func TestEvidenceEnqueueMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "evidence_enqueue"),
		loadWireBaseline(t).Operations[operationIndex(t, "evidence_enqueue")].Request.ArtifactID,
		loadWireBaseline(t).Operations[operationIndex(t, "evidence_enqueue")].Request.Collection,
		EncodeEvidenceEnqueueRequest, DecodeEvidenceEnqueueRequest,
		EncodeEvidenceEnqueueReply, DecodeEvidenceEnqueueReply)
}

func TestEvidenceMarkFailedMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "evidence_mark_failed"),
		loadWireBaseline(t).Operations[operationIndex(t, "evidence_mark_failed")].Request.ArtifactID,
		loadWireBaseline(t).Operations[operationIndex(t, "evidence_mark_failed")].Request.LastError,
		EncodeEvidenceMarkFailedRequest, DecodeEvidenceMarkFailedRequest,
		EncodeEvidenceMarkFailedReply, DecodeEvidenceMarkFailedReply)
}

func TestSynthMarkFailedMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "synth_mark_failed"),
		loadWireBaseline(t).Operations[operationIndex(t, "synth_mark_failed")].Request.ArtifactID,
		loadWireBaseline(t).Operations[operationIndex(t, "synth_mark_failed")].Request.LastError,
		EncodeSynthMarkFailedRequest, DecodeSynthMarkFailedRequest,
		EncodeSynthMarkFailedReply, DecodeSynthMarkFailedReply)
}

func TestRuntimeStateSetMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "runtime_state_set"),
		loadWireBaseline(t).Operations[operationIndex(t, "runtime_state_set")].Request.StateKey,
		loadWireBaseline(t).Operations[operationIndex(t, "runtime_state_set")].Request.StateValue,
		EncodeRuntimeStateSetRequest, DecodeRuntimeStateSetRequest,
		EncodeRuntimeStateSetReply, DecodeRuntimeStateSetReply)
}

func TestSetActiveEmbedderVersionMatchesEverySharedCVector(t *testing.T) {
	stringPairAckVectors(t, operationIndex(t, "set_active_embedder_version"),
		loadWireBaseline(t).Operations[operationIndex(t, "set_active_embedder_version")].Request.Version,
		loadWireBaseline(t).Operations[operationIndex(t, "set_active_embedder_version")].Request.UpdatedAt,
		EncodeSetActiveEmbedderVersionRequest, DecodeSetActiveEmbedderVersionRequest,
		EncodeSetActiveEmbedderVersionReply, DecodeSetActiveEmbedderVersionReply)
}

// stringPairAckVectors drives one db2-envelope-string-pair-ack-v1 operation
// through every vector. Both strings are compared on decode: a decoder that
// read them in the wrong order would still decode, and the round trip alone
// would not notice.
func stringPairAckVectors(t *testing.T, index int, wantFirst string, wantSecond string,
	encodeRequest func(string, string) ([]byte, error),
	decodeRequest func([]byte) (string, string, error),
	encodeReply func() ([]byte, error),
	decodeReply func([]byte) error) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(wantFirst, wantSecond)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	first, second, err := decodeRequest(request)
	if err != nil || first != wantFirst || second != wantSecond {
		t.Fatalf("request decode: %v %q %q", err, first, second)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		reply, err := encodeReply()
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		if err := decodeReply(reply); err != nil {
			t.Fatalf("reply decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

func TestEntityProfileFreshMatchesEverySharedCVector(t *testing.T) {
	stringPairCountVectors(t, operationIndex(t, "entity_profile_fresh"),
		loadWireBaseline(t).Operations[operationIndex(t, "entity_profile_fresh")].Request.EntityID,
		loadWireBaseline(t).Operations[operationIndex(t, "entity_profile_fresh")].Request.Window,
		func(v replyVector) uint32 { return v.Fresh },
		EncodeEntityProfileFreshRequest, DecodeEntityProfileFreshRequest,
		EncodeEntityProfileFreshReply, DecodeEntityProfileFreshReply)
}

func TestDocExistsByHashMatchesEverySharedCVector(t *testing.T) {
	stringPairCountVectors(t, operationIndex(t, "doc_exists_by_hash"),
		loadWireBaseline(t).Operations[operationIndex(t, "doc_exists_by_hash")].Request.ContentHash,
		loadWireBaseline(t).Operations[operationIndex(t, "doc_exists_by_hash")].Request.Scope,
		func(v replyVector) uint32 { return v.Exists },
		EncodeDocExistsByHashRequest, DecodeDocExistsByHashRequest,
		EncodeDocExistsByHashReply, DecodeDocExistsByHashReply)
}

func TestPdfQuarantineConfirmMatchesEverySharedCVector(t *testing.T) {
	stringPairCountVectors(t, operationIndex(t, "pdf_quarantine_confirm"),
		loadWireBaseline(t).Operations[operationIndex(t, "pdf_quarantine_confirm")].Request.Project,
		loadWireBaseline(t).Operations[operationIndex(t, "pdf_quarantine_confirm")].Request.FilePath,
		func(v replyVector) uint32 { return v.Confirmed },
		EncodePdfQuarantineConfirmRequest, DecodePdfQuarantineConfirmRequest,
		EncodePdfQuarantineConfirmReply, DecodePdfQuarantineConfirmReply)
}

func TestPdfQuarantineRejectMatchesEverySharedCVector(t *testing.T) {
	stringPairCountVectors(t, operationIndex(t, "pdf_quarantine_reject"),
		loadWireBaseline(t).Operations[operationIndex(t, "pdf_quarantine_reject")].Request.Project,
		loadWireBaseline(t).Operations[operationIndex(t, "pdf_quarantine_reject")].Request.FilePath,
		func(v replyVector) uint32 { return v.Rejected },
		EncodePdfQuarantineRejectRequest, DecodePdfQuarantineRejectRequest,
		EncodePdfQuarantineRejectReply, DecodePdfQuarantineRejectReply)
}

func TestEnrollmentActiveMatchesEverySharedCVector(t *testing.T) {
	stringPairCountVectors(t, operationIndex(t, "enrollment_active"),
		loadWireBaseline(t).Operations[operationIndex(t, "enrollment_active")].Request.CertIssuer,
		loadWireBaseline(t).Operations[operationIndex(t, "enrollment_active")].Request.CertSerialNorm,
		// `Active` is the pool's connection count; the field carrying an
		// `active` reply is FenceActive, which this shares with the fence probe.
		func(v replyVector) uint32 { return v.FenceActive },
		EncodeEnrollmentActiveRequest, DecodeEnrollmentActiveRequest,
		EncodeEnrollmentActiveReply, DecodeEnrollmentActiveReply)
}

// stringPairCountVectors drives one db2-envelope-string-pair-u32-v1 operation
// through every vector. The answer is read through a selector rather than
// guessed from whichever field is non-zero: these operations name their answer
// differently, and a zero answer is a legitimate positive.
func stringPairCountVectors(t *testing.T, index int, wantFirst string, wantSecond string,
	answerOf func(replyVector) uint32,
	encodeRequest func(string, string) ([]byte, error),
	decodeRequest func([]byte) (string, string, error),
	encodeReply func(uint32) ([]byte, error),
	decodeReply func([]byte) (uint32, error)) {
	t.Helper()
	operation := loadWireBaseline(t).Operations[index]

	request, err := encodeRequest(wantFirst, wantSecond)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	first, second, err := decodeRequest(request)
	if err != nil || first != wantFirst || second != wantSecond {
		t.Fatalf("request decode: %v %q %q", err, first, second)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := decodeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}

	for _, vector := range operation.Reply.Positive {
		answer := answerOf(vector)
		reply, err := encodeReply(answer)
		if err != nil || hex.EncodeToString(reply) != vector.Hex {
			t.Fatalf("reply encode: %v %x", err, reply)
		}
		if got, err := decodeReply(reply); err != nil || got != answer {
			t.Fatalf("reply decode: %v %d", err, got)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if _, err := decodeReply(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("reply %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProspectiveSetStateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "prospective_set_state")]

	request, err := EncodeProspectiveSetStateRequest(operation.Request.ProspectiveID, operation.Request.NewState)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	prospectiveID, newState, err := DecodeProspectiveSetStateRequest(request)
	if err != nil || prospectiveID != operation.Request.ProspectiveID ||
		newState != operation.Request.NewState {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeProspectiveSetStateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDedupeByKeyMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "dedupe_by_key")]

	request, err := EncodeDedupeByKeyRequest(operation.Request.DryRun)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	dryRun, err := DecodeDedupeByKeyRequest(request)
	if err != nil || dryRun != operation.Request.DryRun {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeDedupeByKeyRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestSceneMemberExistsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "scene_member_exists")]

	request, err := EncodeSceneMemberExistsRequest(operation.Request.SceneMemoryID, operation.Request.SceneID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	sceneMemoryID, sceneID, err := DecodeSceneMemberExistsRequest(request)
	if err != nil || sceneMemoryID != operation.Request.SceneMemoryID ||
		sceneID != operation.Request.SceneID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeSceneMemberExistsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestUnitEdgeExistsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "unit_edge_exists")]

	request, err := EncodeUnitEdgeExistsRequest(operation.Request.UnitIDA, operation.Request.UnitIDB)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	unitIDA, unitIDB, err := DecodeUnitEdgeExistsRequest(request)
	if err != nil || unitIDA != operation.Request.UnitIDA ||
		unitIDB != operation.Request.UnitIDB {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeUnitEdgeExistsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestMatchErrorKeysMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "match_error_keys")]

	request, err := EncodeMatchErrorKeysRequest(operation.Request.ErrorLowered)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	errorLowered, err := DecodeMatchErrorKeysRequest(request)
	if err != nil || errorLowered != operation.Request.ErrorLowered {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeMatchErrorKeysRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestMemoryIdsByUpdatedMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "memory_ids_by_updated")]

	request, err := EncodeMemoryIdsByUpdatedRequest(operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	limit, err := DecodeMemoryIdsByUpdatedRequest(request)
	if err != nil || limit != operation.Request.Limit {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeMemoryIdsByUpdatedRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestUnitIdsForMemoryMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "unit_ids_for_memory")]

	request, err := EncodeUnitIdsForMemoryRequest(operation.Request.MemoryID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	memoryID, err := DecodeUnitIdsForMemoryRequest(request)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeUnitIdsForMemoryRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProjectFingerprintMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "project_fingerprint")]

	request, err := EncodeProjectFingerprintRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeProjectFingerprintRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProjectFingerprintRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestVisibleSourceHashMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "visible_source_hash")]

	request, err := EncodeVisibleSourceHashRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeVisibleSourceHashRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeVisibleSourceHashRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestEntityProfileCardMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_profile_card")]

	request, err := EncodeEntityProfileCardRequest(operation.Request.EntityID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entityID, err := DecodeEntityProfileCardRequest(request)
	if err != nil || entityID != operation.Request.EntityID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeEntityProfileCardRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestGenerationAbortMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "generation_abort")]

	request, err := EncodeGenerationAbortRequest(operation.Request.GenerationID, operation.Request.ErrorMessage)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	generationID, errorMessage, err := DecodeGenerationAbortRequest(request)
	if err != nil || generationID != operation.Request.GenerationID ||
		errorMessage != operation.Request.ErrorMessage {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeGenerationAbortRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestGenerationSetSourceHashMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "generation_set_source_hash")]

	request, err := EncodeGenerationSetSourceHashRequest(operation.Request.GenerationID, operation.Request.SourceHash)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	generationID, sourceHash, err := DecodeGenerationSetSourceHashRequest(request)
	if err != nil || generationID != operation.Request.GenerationID ||
		sourceHash != operation.Request.SourceHash {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeGenerationSetSourceHashRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestGenerationPublishMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "generation_publish")]

	request, err := EncodeGenerationPublishRequest(operation.Request.GenerationID, operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	generationID, project, err := DecodeGenerationPublishRequest(request)
	if err != nil || generationID != operation.Request.GenerationID ||
		project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeGenerationPublishRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestPurgeFilesMatchingMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "purge_files_matching")]

	request, err := EncodePurgeFilesMatchingRequest(operation.Request.ProjectID, operation.Request.PathGlob)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	projectID, pathGlob, err := DecodePurgeFilesMatchingRequest(request)
	if err != nil || projectID != operation.Request.ProjectID ||
		pathGlob != operation.Request.PathGlob {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodePurgeFilesMatchingRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestFileIndexDeleteCurrentGenerationMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "file_index_delete_current_generation")]

	request, err := EncodeFileIndexDeleteCurrentGenerationRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeFileIndexDeleteCurrentGenerationRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeFileIndexDeleteCurrentGenerationRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProjectDeleteMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "project_delete")]

	request, err := EncodeProjectDeleteRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeProjectDeleteRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProjectDeleteRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestMinhashDeleteCurrentGenerationMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "minhash_delete_current_generation")]

	request, err := EncodeMinhashDeleteCurrentGenerationRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeMinhashDeleteCurrentGenerationRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeMinhashDeleteCurrentGenerationRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestMinhashDeleteFileMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "minhash_delete_file")]

	request, err := EncodeMinhashDeleteFileRequest(operation.Request.Project, operation.Request.FilePath)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, filePath, err := DecodeMinhashDeleteFileRequest(request)
	if err != nil || project != operation.Request.Project ||
		filePath != operation.Request.FilePath {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeMinhashDeleteFileRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProjectCurrentGenerationMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "project_current_generation")]

	request, err := EncodeProjectCurrentGenerationRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeProjectCurrentGenerationRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProjectCurrentGenerationRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProjectionGenerationCreateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "projection_generation_create")]

	request, err := EncodeProjectionGenerationCreateRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeProjectionGenerationCreateRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProjectionGenerationCreateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProjectionVisibleIDMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "projection_visible_id")]

	request, err := EncodeProjectionVisibleIDRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeProjectionVisibleIDRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProjectionVisibleIDRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestUniqueFileBasenameMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "unique_file_basename")]

	request, err := EncodeUniqueFileBasenameRequest(operation.Request.Project, operation.Request.Basename)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, basename, err := DecodeUniqueFileBasenameRequest(request)
	if err != nil || project != operation.Request.Project ||
		basename != operation.Request.Basename {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeUniqueFileBasenameRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestEntityNeighborsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_neighbors")]

	request, err := EncodeEntityNeighborsRequest(operation.Request.Entity, operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entity, limit, err := DecodeEntityNeighborsRequest(request)
	if err != nil || entity != operation.Request.Entity ||
		limit != operation.Request.Limit {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeEntityNeighborsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestEntityNeighborsFilteredMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_neighbors_filtered")]

	request, err := EncodeEntityNeighborsFilteredRequest(operation.Request.Entity, operation.Request.RelationA, operation.Request.RelationB, operation.Request.OrderByWeight, operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entity, relationA, relationB, orderByWeight, limit, err := DecodeEntityNeighborsFilteredRequest(request)
	if err != nil || entity != operation.Request.Entity ||
		relationA != operation.Request.RelationA ||
		relationB != operation.Request.RelationB ||
		orderByWeight != operation.Request.OrderByWeight ||
		limit != operation.Request.Limit {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, _, _, err := DecodeEntityNeighborsFilteredRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestEntityOutboundNeighborsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_outbound_neighbors")]

	request, err := EncodeEntityOutboundNeighborsRequest(operation.Request.Entity, operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entity, limit, err := DecodeEntityOutboundNeighborsRequest(request)
	if err != nil || entity != operation.Request.Entity ||
		limit != operation.Request.Limit {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeEntityOutboundNeighborsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestEntityTopPartnersMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_top_partners")]

	request, err := EncodeEntityTopPartnersRequest(operation.Request.Entity, operation.Request.Relation)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entity, relation, err := DecodeEntityTopPartnersRequest(request)
	if err != nil || entity != operation.Request.Entity ||
		relation != operation.Request.Relation {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeEntityTopPartnersRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestEntityTopTargetsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_top_targets")]

	request, err := EncodeEntityTopTargetsRequest(operation.Request.Entity, operation.Request.Relation)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	entity, relation, err := DecodeEntityTopTargetsRequest(request)
	if err != nil || entity != operation.Request.Entity ||
		relation != operation.Request.Relation {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeEntityTopTargetsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestFileDefinitionsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "file_definitions")]

	request, err := EncodeFileDefinitionsRequest(operation.Request.Project, operation.Request.FilePath)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, filePath, err := DecodeFileDefinitionsRequest(request)
	if err != nil || project != operation.Request.Project ||
		filePath != operation.Request.FilePath {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeFileDefinitionsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCodeSearchMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "code_search")]

	request, err := EncodeCodeSearchRequest(operation.Request.Query, operation.Request.Project, operation.Request.Enrich)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	query, project, enrich, err := DecodeCodeSearchRequest(request)
	if err != nil || query != operation.Request.Query ||
		project != operation.Request.Project ||
		enrich != operation.Request.Enrich {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeCodeSearchRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCodeSearchExcludingProjectMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "code_search_excluding_project")]

	request, err := EncodeCodeSearchExcludingProjectRequest(operation.Request.Query, operation.Request.ExcludedProject, operation.Request.Enrich)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	query, excludedProject, enrich, err := DecodeCodeSearchExcludingProjectRequest(request)
	if err != nil || query != operation.Request.Query ||
		excludedProject != operation.Request.ExcludedProject ||
		enrich != operation.Request.Enrich {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeCodeSearchExcludingProjectRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestBanditArmsListMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "bandit_arms_list")]

	request, err := EncodeBanditArmsListRequest(operation.Request.DecisionPoint)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	decisionPoint, err := DecodeBanditArmsListRequest(request)
	if err != nil || decisionPoint != operation.Request.DecisionPoint {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeBanditArmsListRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestBanditPromotionGetMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "bandit_promotion_get")]

	request, err := EncodeBanditPromotionGetRequest(operation.Request.DecisionPoint)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	decisionPoint, err := DecodeBanditPromotionGetRequest(request)
	if err != nil || decisionPoint != operation.Request.DecisionPoint {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeBanditPromotionGetRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDecisionLogSetOutcomeMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "decision_log_set_outcome")]

	request, err := EncodeDecisionLogSetOutcomeRequest(operation.Request.DecisionID, operation.Request.Outcome)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	decisionID, outcome, err := DecodeDecisionLogSetOutcomeRequest(request)
	if err != nil || decisionID != operation.Request.DecisionID ||
		outcome != operation.Request.Outcome {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDecisionLogSetOutcomeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDecisionLogSetStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "decision_log_set_status")]

	request, err := EncodeDecisionLogSetStatusRequest(operation.Request.DecisionID, operation.Request.Status)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	decisionID, status, err := DecodeDecisionLogSetStatusRequest(request)
	if err != nil || decisionID != operation.Request.DecisionID ||
		status != operation.Request.Status {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDecisionLogSetStatusRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDecisionLogSetRevisitMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "decision_log_set_revisit")]

	request, err := EncodeDecisionLogSetRevisitRequest(operation.Request.DecisionID, operation.Request.RevisitWhen)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	decisionID, revisitWhen, err := DecodeDecisionLogSetRevisitRequest(request)
	if err != nil || decisionID != operation.Request.DecisionID ||
		revisitWhen != operation.Request.RevisitWhen {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDecisionLogSetRevisitRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCollabRuleApproveMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "collab_rule_approve")]

	request, err := EncodeCollabRuleApproveRequest(operation.Request.RuleID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	ruleID, err := DecodeCollabRuleApproveRequest(request)
	if err != nil || ruleID != operation.Request.RuleID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeCollabRuleApproveRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCollabRuleRejectMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "collab_rule_reject")]

	request, err := EncodeCollabRuleRejectRequest(operation.Request.RuleID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	ruleID, err := DecodeCollabRuleRejectRequest(request)
	if err != nil || ruleID != operation.Request.RuleID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeCollabRuleRejectRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCollabRuleRetireMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "collab_rule_retire")]

	request, err := EncodeCollabRuleRetireRequest(operation.Request.RuleID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	ruleID, err := DecodeCollabRuleRetireRequest(request)
	if err != nil || ruleID != operation.Request.RuleID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeCollabRuleRetireRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProposalBumpCorroborationMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "proposal_bump_corroboration")]

	request, err := EncodeProposalBumpCorroborationRequest(operation.Request.ProposalID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	proposalID, err := DecodeProposalBumpCorroborationRequest(request)
	if err != nil || proposalID != operation.Request.ProposalID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProposalBumpCorroborationRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestProposalMarkCommittedMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "proposal_mark_committed")]

	request, err := EncodeProposalMarkCommittedRequest(operation.Request.ProposalID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	proposalID, err := DecodeProposalMarkCommittedRequest(request)
	if err != nil || proposalID != operation.Request.ProposalID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeProposalMarkCommittedRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestRulesDeleteByIDMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "rules_delete_by_id")]

	request, err := EncodeRulesDeleteByIDRequest(operation.Request.RuleRowID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	ruleRowID, err := DecodeRulesDeleteByIDRequest(request)
	if err != nil || ruleRowID != operation.Request.RuleRowID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeRulesDeleteByIDRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCalibrationSurfacesWithDataMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "calibration_surfaces_with_data")]

	request, err := EncodeCalibrationSurfacesWithDataRequest(operation.Request.MinRows)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	minRows, err := DecodeCalibrationSurfacesWithDataRequest(request)
	if err != nil || minRows != operation.Request.MinRows {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeCalibrationSurfacesWithDataRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestArtifactCiteMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "artifact_cite")]

	request, err := EncodeArtifactCiteRequest(operation.Request.CitingArtifactID, operation.Request.SourceKind, operation.Request.SourceID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	citingArtifactID, sourceKind, sourceID, err := DecodeArtifactCiteRequest(request)
	if err != nil || citingArtifactID != operation.Request.CitingArtifactID ||
		sourceKind != operation.Request.SourceKind ||
		sourceID != operation.Request.SourceID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeArtifactCiteRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestArtifactLinkMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "artifact_link")]

	request, err := EncodeArtifactLinkRequest(operation.Request.FromArtifactID, operation.Request.ToArtifactID, operation.Request.LinkKind)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	fromArtifactID, toArtifactID, linkKind, err := DecodeArtifactLinkRequest(request)
	if err != nil || fromArtifactID != operation.Request.FromArtifactID ||
		toArtifactID != operation.Request.ToArtifactID ||
		linkKind != operation.Request.LinkKind {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeArtifactLinkRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestBanditPromotionSetMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "bandit_promotion_set")]

	request, err := EncodeBanditPromotionSetRequest(operation.Request.DecisionPoint, operation.Request.ArmID, operation.Request.RollbackArm)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	decisionPoint, armID, rollbackArm, err := DecodeBanditPromotionSetRequest(request)
	if err != nil || decisionPoint != operation.Request.DecisionPoint ||
		armID != operation.Request.ArmID ||
		rollbackArm != operation.Request.RollbackArm {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeBanditPromotionSetRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCollabRuleProposeMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "collab_rule_propose")]

	request, err := EncodeCollabRuleProposeRequest(operation.Request.RuleText, operation.Request.RuleReason, operation.Request.ProposedBy)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	ruleText, ruleReason, proposedBy, err := DecodeCollabRuleProposeRequest(request)
	if err != nil || ruleText != operation.Request.RuleText ||
		ruleReason != operation.Request.RuleReason ||
		proposedBy != operation.Request.ProposedBy {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeCollabRuleProposeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestRulesDeleteByDirectiveTypeMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "rules_delete_by_directive_type")]

	request, err := EncodeRulesDeleteByDirectiveTypeRequest(operation.Request.DirectiveType)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	directiveType, err := DecodeRulesDeleteByDirectiveTypeRequest(request)
	if err != nil || directiveType != operation.Request.DirectiveType {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeRulesDeleteByDirectiveTypeRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestArtifactFlagReviewMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "artifact_flag_review")]

	request, err := EncodeArtifactFlagReviewRequest(operation.Request.ArtifactID, operation.Request.FlagReason)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	artifactID, flagReason, err := DecodeArtifactFlagReviewRequest(request)
	if err != nil || artifactID != operation.Request.ArtifactID ||
		flagReason != operation.Request.FlagReason {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeArtifactFlagReviewRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestVerdictSuppressedMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "verdict_suppressed")]

	request, err := EncodeVerdictSuppressedRequest(operation.Request.VerdictTag, operation.Request.VerdictScope)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	verdictTag, verdictScope, err := DecodeVerdictSuppressedRequest(request)
	if err != nil || verdictTag != operation.Request.VerdictTag ||
		verdictScope != operation.Request.VerdictScope {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeVerdictSuppressedRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCuratorInvalidateDocMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "curator_invalidate_doc")]

	request, err := EncodeCuratorInvalidateDocRequest(operation.Request.Project, operation.Request.FilePath)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, filePath, err := DecodeCuratorInvalidateDocRequest(request)
	if err != nil || project != operation.Request.Project ||
		filePath != operation.Request.FilePath {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeCuratorInvalidateDocRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestOntologyEvalStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "ontology_eval_status")]

	request, err := EncodeOntologyEvalStatusRequest(operation.Request.RelType)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	relType, err := DecodeOntologyEvalStatusRequest(request)
	if err != nil || relType != operation.Request.RelType {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeOntologyEvalStatusRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestTaskUpdateStateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "task_update_state")]

	request, err := EncodeTaskUpdateStateRequest(operation.Request.TaskID, operation.Request.State)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	taskID, state, err := DecodeTaskUpdateStateRequest(request)
	if err != nil || taskID != operation.Request.TaskID ||
		state != operation.Request.State {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeTaskUpdateStateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestReleaseAddDocMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "release_add_doc")]

	request, err := EncodeReleaseAddDocRequest(operation.Request.ReleaseID, operation.Request.DocID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	releaseID, docID, err := DecodeReleaseAddDocRequest(request)
	if err != nil || releaseID != operation.Request.ReleaseID ||
		docID != operation.Request.DocID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeReleaseAddDocRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestOntologyApproveMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "ontology_approve")]

	request, err := EncodeOntologyApproveRequest(operation.Request.RelType)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	relType, err := DecodeOntologyApproveRequest(request)
	if err != nil || relType != operation.Request.RelType {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeOntologyApproveRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestOntologyRejectMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "ontology_reject")]

	request, err := EncodeOntologyRejectRequest(operation.Request.RelType)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	relType, err := DecodeOntologyRejectRequest(request)
	if err != nil || relType != operation.Request.RelType {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeOntologyRejectRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDocAssetsDeleteForDocMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "doc_assets_delete_for_doc")]

	request, err := EncodeDocAssetsDeleteForDocRequest(operation.Request.Project, operation.Request.DocumentKey)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, documentKey, err := DecodeDocAssetsDeleteForDocRequest(request)
	if err != nil || project != operation.Request.Project ||
		documentKey != operation.Request.DocumentKey {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDocAssetsDeleteForDocRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestOntologyMapMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "ontology_map")]

	request, err := EncodeOntologyMapRequest(operation.Request.RelType, operation.Request.MappedTo)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	relType, mappedTo, err := DecodeOntologyMapRequest(request)
	if err != nil || relType != operation.Request.RelType ||
		mappedTo != operation.Request.MappedTo {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeOntologyMapRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestReleaseCreateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "release_create")]

	request, err := EncodeReleaseCreateRequest(operation.Request.ReleaseName)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	releaseName, err := DecodeReleaseCreateRequest(request)
	if err != nil || releaseName != operation.Request.ReleaseName {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeReleaseCreateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestPurgeFenceHeartbeatMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "purge_fence_heartbeat")]

	request, err := EncodePurgeFenceHeartbeatRequest(operation.Request.Project, operation.Request.Generation, operation.Request.PurgeID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, generation, purgeID, err := DecodePurgeFenceHeartbeatRequest(request)
	if err != nil || project != operation.Request.Project ||
		generation != operation.Request.Generation ||
		purgeID != operation.Request.PurgeID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodePurgeFenceHeartbeatRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestPurgeFenceClearMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "purge_fence_clear")]

	request, err := EncodePurgeFenceClearRequest(operation.Request.Project, operation.Request.Generation, operation.Request.PurgeID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, generation, purgeID, err := DecodePurgeFenceClearRequest(request)
	if err != nil || project != operation.Request.Project ||
		generation != operation.Request.Generation ||
		purgeID != operation.Request.PurgeID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodePurgeFenceClearRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDocumentStoredHashMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "document_stored_hash")]

	request, err := EncodeDocumentStoredHashRequest(operation.Request.Project, operation.Request.FilePath)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, filePath, err := DecodeDocumentStoredHashRequest(request)
	if err != nil || project != operation.Request.Project ||
		filePath != operation.Request.FilePath {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDocumentStoredHashRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDocumentHashExistsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "document_hash_exists")]

	request, err := EncodeDocumentHashExistsRequest(operation.Request.Project, operation.Request.FileHash)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, fileHash, err := DecodeDocumentHashExistsRequest(request)
	if err != nil || project != operation.Request.Project ||
		fileHash != operation.Request.FileHash {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDocumentHashExistsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestPdfTsrStateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "pdf_tsr_state")]

	request, err := EncodePdfTsrStateRequest(operation.Request.Project, operation.Request.DocumentKey)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, documentKey, err := DecodePdfTsrStateRequest(request)
	if err != nil || project != operation.Request.Project ||
		documentKey != operation.Request.DocumentKey {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodePdfTsrStateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDocumentChunkIdsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "document_chunk_ids")]

	request, err := EncodeDocumentChunkIdsRequest(operation.Request.Project, operation.Request.FilePath)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, filePath, err := DecodeDocumentChunkIdsRequest(request)
	if err != nil || project != operation.Request.Project ||
		filePath != operation.Request.FilePath {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDocumentChunkIdsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestRuntimeStateGetMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "runtime_state_get")]

	request, err := EncodeRuntimeStateGetRequest(operation.Request.StateKey)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	stateKey, err := DecodeRuntimeStateGetRequest(request)
	if err != nil || stateKey != operation.Request.StateKey {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeRuntimeStateGetRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestIngestQueueFailMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "ingest_queue_fail")]

	request, err := EncodeIngestQueueFailRequest(operation.Request.IngestJobID, operation.Request.ErrorMessage)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	ingestJobID, errorMessage, err := DecodeIngestQueueFailRequest(request)
	if err != nil || ingestJobID != operation.Request.IngestJobID ||
		errorMessage != operation.Request.ErrorMessage {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeIngestQueueFailRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestResetStuckVectorOpsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "reset_stuck_vector_ops")]

	request, err := EncodeResetStuckVectorOpsRequest(operation.Request.MaxAttempts)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	maxAttempts, err := DecodeResetStuckVectorOpsRequest(request)
	if err != nil || maxAttempts != operation.Request.MaxAttempts {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeResetStuckVectorOpsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestDirectiveResolveMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "directive_resolve")]

	request, err := EncodeDirectiveResolveRequest(operation.Request.DirectiveID, operation.Request.ResolutionMemoryID)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	directiveID, resolutionMemoryID, err := DecodeDirectiveResolveRequest(request)
	if err != nil || directiveID != operation.Request.DirectiveID ||
		resolutionMemoryID != operation.Request.ResolutionMemoryID {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeDirectiveResolveRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCssMigrationEnumerateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "css_migration_enumerate")]

	request, err := EncodeCssMigrationEnumerateRequest(operation.Request.Project)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, err := DecodeCssMigrationEnumerateRequest(request)
	if err != nil || project != operation.Request.Project {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeCssMigrationEnumerateRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCssMigrationAssertConventionsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "css_migration_assert_conventions")]

	request, err := EncodeCssMigrationAssertConventionsRequest(operation.Request.Project, operation.Request.NowIso)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	project, nowIso, err := DecodeCssMigrationAssertConventionsRequest(request)
	if err != nil || project != operation.Request.Project ||
		nowIso != operation.Request.NowIso {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeCssMigrationAssertConventionsRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestCssMigrationRulesDocMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "css_migration_rules_doc")]

	request, err := EncodeCssMigrationRulesDocRequest(operation.Request.ExemplarProject)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	exemplarProject, err := DecodeCssMigrationRulesDocRequest(request)
	if err != nil || exemplarProject != operation.Request.ExemplarProject {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeCssMigrationRulesDocRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

// Generated by scripts/db2_sync_go_contract_test.py; edits are overwritten.
func TestRetryableIndexFailuresMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "retryable_index_failures")]

	request, err := EncodeRetryableIndexFailuresRequest(operation.Request.MaxAttempts, operation.Request.Limit)
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {
		t.Fatalf("request encode: %v %x", err, request)
	}
	maxAttempts, limit, err := DecodeRetryableIndexFailuresRequest(request)
	if err != nil || maxAttempts != operation.Request.MaxAttempts ||
		limit != operation.Request.Limit {
		t.Fatalf("request decode: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeRetryableIndexFailuresRequest(decodeHex(t, vector.Hex)); err == nil {
			t.Fatalf("request %s decoded", vector.Mutation)
		}
	}
}

func TestEntityEdgePruneOrphansMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "entity_edge_prune_orphans")]
	if operation.Family != "index" {
		t.Fatalf("family = %q, want index", operation.Family)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEntityEdgePruneOrphansRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEntityEdgePruneOrphansRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEntityEdgePruneOrphansRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEntityEdgePruneOrphansReply(vector.PrunedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		pruned, err := DecodeEntityEdgePruneOrphansReply(got)
		if err != nil || pruned != vector.PrunedCount {
			t.Fatalf("decode = (%d, %v)", pruned, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		pruned, err := DecodeEntityEdgePruneOrphansReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || pruned != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, pruned, err)
		}
	}
	// Index operation 1 and lifecycle operation 1 (health) encode the same
	// operation number, so only the family constants tell them apart.
	if EventEntityEdgePruneOrphans != EventIndex || StageEntityEdgePruneOrphans != FamilyIndex {
		t.Fatalf("index operation did not carry the index family")
	}
	if StageEntityEdgePruneOrphans == FamilyMemory || StageEntityEdgePruneOrphans == FamilyLifecycle {
		t.Fatalf("index family collided with an existing family")
	}
}

func TestCountAndMaxUpdatedMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "count_and_max_updated")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeCountAndMaxUpdatedRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeCountAndMaxUpdatedRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeCountAndMaxUpdatedRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	// Three positive replies: a populated corpus, an empty one, and an
	// aggregate that could not run. The middle and last are the pair that
	// must not collapse into each other.
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeCountAndMaxUpdatedReply(vector.Result, uint32(vector.Count), vector.MaxUpdatedAt)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply %d = (%x, %v)", vector.Result, got, err)
		}
		result, count, stamp, err := DecodeCountAndMaxUpdatedReply(got)
		if err != nil || result != vector.Result || uint64(count) != vector.Count ||
			stamp != vector.MaxUpdatedAt {
			t.Fatalf("decode = (%d, %d, %q, %v)", result, count, stamp, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, count, stamp, err := DecodeCountAndMaxUpdatedReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || count != 0 || stamp != "" {
			t.Fatalf("negative reply %s = (%d, %d, %q, %v)", vector.Mutation, result, count, stamp, err)
		}
	}
	if _, err := EncodeCountAndMaxUpdatedReply(ResultInvalidState, 1, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("unavailable reply carried a count: %v", err)
	}
}

func TestPickFirstTemporalRefMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "pick_first_temporal_ref")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodePickFirstTemporalRefRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodePickFirstTemporalRefRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodePickFirstTemporalRefRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodePickFirstTemporalRefReply(vector.Result, vector.RefKey)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply %d = (%x, %v)", vector.Result, got, err)
		}
		result, refKey, err := DecodePickFirstTemporalRefReply(got)
		if err != nil || result != vector.Result || refKey != vector.RefKey {
			t.Fatalf("decode = (%d, %q, %v)", result, refKey, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, refKey, err := DecodePickFirstTemporalRefReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || refKey != "" {
			t.Fatalf("negative reply %s = (%d, %q, %v)", vector.Mutation, result, refKey, err)
		}
	}
	if _, err := EncodePickFirstTemporalRefReply(ResultOK, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty ok encoded: %v", err)
	}
	if _, err := EncodePickFirstTemporalRefReply(ResultNotFound, "x"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("not_found carried a key: %v", err)
	}
}

func TestGetSourceSessionMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "get_source_session")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeGetSourceSessionRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeGetSourceSessionRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeGetSourceSessionRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	// Two positive replies only: a session, or none. There is deliberately no
	// empty-ok, unlike get_content on the same wire format.
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeGetSourceSessionReply(vector.Result, vector.SessionIDReply)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply %d = (%x, %v)", vector.Result, got, err)
		}
		result, sessionID, err := DecodeGetSourceSessionReply(got)
		if err != nil || result != vector.Result || sessionID != vector.SessionIDReply {
			t.Fatalf("decode = (%d, %q, %v)", result, sessionID, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, sessionID, err := DecodeGetSourceSessionReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || sessionID != "" {
			t.Fatalf("negative reply %s = (%d, %q, %v)", vector.Mutation, result, sessionID, err)
		}
	}
	// The backend cannot tell a blank column from an absent memory, so an
	// empty ok must not be encodable on this side either.
	if _, err := EncodeGetSourceSessionReply(ResultOK, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty ok encoded: %v", err)
	}
	if _, err := EncodeGetSourceSessionReply(ResultNotFound, "x"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("not_found carried a session: %v", err)
	}
	// The read bound matches what set_source_session accepts.
	if GetSourceSessionSessionMax != SetSourceSessionSessionMax {
		t.Fatalf("read bound %d does not match the write bound %d",
			GetSourceSessionSessionMax, SetSourceSessionSessionMax)
	}
}

func TestGetContentMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "get_content")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeGetContentRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeGetContentRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeGetContentRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	// Three positive replies: content, empty content, and no such memory. The
	// middle and last are the pair this operation exists to keep apart.
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeGetContentReply(vector.Result, vector.Content)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply %d = (%x, %v)", vector.Result, got, err)
		}
		result, content, err := DecodeGetContentReply(got)
		if err != nil || result != vector.Result || content != vector.Content {
			t.Fatalf("decode = (%d, %q, %v)", result, content, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, content, err := DecodeGetContentReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || content != "" {
			t.Fatalf("negative reply %s = (%d, %q, %v)", vector.Mutation, result, content, err)
		}
	}
	// not_found carries nothing at all, so it cannot be confused with a row
	// holding an empty string.
	if _, err := EncodeGetContentReply(ResultNotFound, "x"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("not_found carried content: %v", err)
	}
	// The read bound matches what update_content will accept, so a row this
	// side cannot return is a row that side should not have stored.
	if GetContentContentMax != UpdateContentContentMax {
		t.Fatalf("read bound %d does not match the write bound %d",
			GetContentContentMax, UpdateContentContentMax)
	}
	if _, err := EncodeGetContentReply(ResultOK, strings.Repeat("c", GetContentContentMax+1)); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("content past the bound encoded: %v", err)
	}
}

func TestNegationTokensUpdateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "negation_tokens_update")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeNegationTokensUpdateRequest(operation.Request.MemoryID, operation.Request.Tokens)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, tokens, err := DecodeNegationTokensUpdateRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || tokens != operation.Request.Tokens {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, tokens, err)
	}
	// An empty extraction is a positive vector: a memory with no negations
	// still has to clear whatever was stored before.
	wantCleared := decodeHex(t, operation.Request.Cleared)
	gotCleared, err := EncodeNegationTokensUpdateRequest(operation.Request.MemoryID, "")
	if err != nil || string(gotCleared) != string(wantCleared) {
		t.Fatalf("cleared request = (%x, %v), want %x", gotCleared, err, wantCleared)
	}
	memoryID, tokens, err = DecodeNegationTokensUpdateRequest(wantCleared)
	if err != nil || memoryID != operation.Request.MemoryID || tokens != "" {
		t.Fatalf("cleared request decode = (%d, %q, %v)", memoryID, tokens, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeNegationTokensUpdateRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeNegationTokensUpdateReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeNegationTokensUpdateReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeNegationTokensUpdateReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	atBound := strings.Repeat("t", NegationTokensUpdateTokensMax)
	if _, err := EncodeNegationTokensUpdateRequest(42, atBound); err != nil {
		t.Fatalf("tokens at the bound refused: %v", err)
	}
	if _, err := EncodeNegationTokensUpdateRequest(42, atBound+"t"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("tokens past the bound encoded: %v", err)
	}
}

func TestSetSourceSessionMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "set_source_session")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeSetSourceSessionRequest(operation.Request.MemoryID, operation.Request.SessionID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, sessionID, err := DecodeSetSourceSessionRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || sessionID != operation.Request.SessionID {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, sessionID, err)
	}
	// The clear is a positive vector, not a negative one: an empty session is
	// a real request that unsets the column.
	wantCleared := decodeHex(t, operation.Request.Cleared)
	gotCleared, err := EncodeSetSourceSessionRequest(operation.Request.MemoryID, "")
	if err != nil || string(gotCleared) != string(wantCleared) {
		t.Fatalf("cleared request = (%x, %v), want %x", gotCleared, err, wantCleared)
	}
	memoryID, sessionID, err = DecodeSetSourceSessionRequest(wantCleared)
	if err != nil || memoryID != operation.Request.MemoryID || sessionID != "" {
		t.Fatalf("cleared request decode = (%d, %q, %v)", memoryID, sessionID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeSetSourceSessionRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeSetSourceSessionReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeSetSourceSessionReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeSetSourceSessionReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	atBound := strings.Repeat("s", SetSourceSessionSessionMax)
	if _, err := EncodeSetSourceSessionRequest(42, atBound); err != nil {
		t.Fatalf("session at the bound refused: %v", err)
	}
	if _, err := EncodeSetSourceSessionRequest(42, atBound+"s"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("session past the bound encoded: %v", err)
	}
}

func TestSetCognifiedKindMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "set_cognified_kind")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeSetCognifiedKindRequest(operation.Request.MemoryID, operation.Request.Kind)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, kind, err := DecodeSetCognifiedKindRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || kind != operation.Request.Kind {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, kind, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeSetCognifiedKindRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeSetCognifiedKindReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeSetCognifiedKindReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeSetCognifiedKindReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	atBound := strings.Repeat("k", SetCognifiedKindKindMax)
	if _, err := EncodeSetCognifiedKindRequest(42, atBound); err != nil {
		t.Fatalf("kind at the bound refused: %v", err)
	}
	if _, err := EncodeSetCognifiedKindRequest(42, atBound+"k"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("kind past the bound encoded: %v", err)
	}
	// This setter requires a kind; the two that follow accept empty to clear.
	if _, err := EncodeSetCognifiedKindRequest(42, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty kind encoded: %v", err)
	}
}

func TestWorkspaceTagInsertMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "workspace_tag_insert")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeWorkspaceTagInsertRequest(operation.Request.MemoryID, operation.Request.Workspace)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, workspace, err := DecodeWorkspaceTagInsertRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || workspace != operation.Request.Workspace {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, workspace, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeWorkspaceTagInsertRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeWorkspaceTagInsertReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeWorkspaceTagInsertReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeWorkspaceTagInsertReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	// A truncated workspace name is a different workspace, so the bound is
	// enforced rather than clamped.
	atBound := strings.Repeat("w", WorkspaceTagInsertWorkspaceMax)
	if _, err := EncodeWorkspaceTagInsertRequest(42, atBound); err != nil {
		t.Fatalf("workspace at the bound refused: %v", err)
	}
	if _, err := EncodeWorkspaceTagInsertRequest(42, atBound+"w"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("workspace past the bound encoded: %v", err)
	}
	if _, err := EncodeWorkspaceTagInsertRequest(42, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty workspace encoded: %v", err)
	}
}

func TestDecayConfidenceMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "decay_confidence")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeDecayConfidenceRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeDecayConfidenceRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeDecayConfidenceRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeDecayConfidenceReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeDecayConfidenceReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeDecayConfidenceReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	// Three operations on this bus move confidence and each uses a different
	// constant; comparing as bits catches one being copied onto another.
	if math.Float64frombits(DecayConfidenceMultiplierBits) != 0.7 ||
		DecayConfidenceMultiplierBits == DemoteIDMultiplierBits {
		t.Fatalf("multiplier = %v", math.Float64frombits(DecayConfidenceMultiplierBits))
	}
	if _, err := EncodeDecayConfidenceRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero memory encoded: %v", err)
	}
}

func TestUpdateContentMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "update_content")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeUpdateContentRequest(operation.Request.MemoryID, operation.Request.Content)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, content, err := DecodeUpdateContentRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || content != operation.Request.Content {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, content, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeUpdateContentRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeUpdateContentReply(vector.UpdatedRows)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		updated, err := DecodeUpdateContentReply(got)
		if err != nil || updated != vector.UpdatedRows {
			t.Fatalf("decode = (%d, %v)", updated, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		updated, err := DecodeUpdateContentReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || updated != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, updated, err)
		}
	}
	// Exactly the memory record's content width encodes; one byte more is
	// refused rather than silently truncated by whatever reads the row back.
	atBound := strings.Repeat("x", UpdateContentContentMax)
	if _, err := EncodeUpdateContentRequest(42, atBound); err != nil {
		t.Fatalf("content at the bound refused: %v", err)
	}
	if _, err := EncodeUpdateContentRequest(42, atBound+"x"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("content past the bound encoded: %v", err)
	}
	if _, err := EncodeUpdateContentRequest(42, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty content encoded: %v", err)
	}
}

func TestRejectMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "reject")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeRejectRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeRejectRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeRejectRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeRejectReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeRejectReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeRejectReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	if _, err := EncodeRejectRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero memory encoded: %v", err)
	}
}

func TestRejectPolicyIsFixed(t *testing.T) {
	// Compared as bits: a differently-rounded penalty on one side would move
	// confidence by a different amount than the other while every envelope
	// still matched.
	if math.Float64frombits(RejectPenaltyBits) != 0.1 ||
		math.Float64frombits(RejectFloorBits) != 0.0 {
		t.Fatalf("policy = (%v, %v)", math.Float64frombits(RejectPenaltyBits),
			math.Float64frombits(RejectFloorBits))
	}
}

func TestHasScopeTypeMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "has_scope_type")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeHasScopeTypeRequest(operation.Request.MemoryID, operation.Request.ScopeType)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, scopeType, err := DecodeHasScopeTypeRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || scopeType != operation.Request.ScopeType {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, scopeType, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeHasScopeTypeRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeHasScopeTypeReply(vector.Present)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		present, err := DecodeHasScopeTypeReply(got)
		if err != nil || present != vector.Present {
			t.Fatalf("decode = (%d, %v)", present, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		present, err := DecodeHasScopeTypeReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || present != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, present, err)
		}
	}
	if _, err := EncodeHasScopeTypeReply(HasScopeTypeMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("non-Boolean flag encoded: %v", err)
	}
	if _, err := EncodeHasScopeTypeRequest(42, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty scope kind encoded: %v", err)
	}
}

func TestValidAtMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "valid_at")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeValidAtRequest(operation.Request.MemoryID, operation.Request.AsOf)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, asOf, err := DecodeValidAtRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID || asOf != operation.Request.AsOf {
		t.Fatalf("positive request = (%d, %q, %v)", memoryID, asOf, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, err := DecodeValidAtRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeValidAtReply(vector.Result, vector.InForce)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, inForce, err := DecodeValidAtReply(got)
		if err != nil || result != vector.Result || inForce != vector.InForce {
			t.Fatalf("decode = (%d, %d, %v)", result, inForce, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, inForce, err := DecodeValidAtReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || inForce != 0 {
			t.Fatalf("negative reply %s = (%d, %d, %v)", vector.Mutation, result, inForce, err)
		}
	}
	// "Could not evaluate" carries no verdict, so it must not be encodable
	// alongside one -- otherwise a caller could read a verdict that was never
	// reached.
	if _, err := EncodeValidAtReply(ResultInvalidState, 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("unevaluated reply carried a verdict: %v", err)
	}
	if _, err := EncodeValidAtRequest(42, ""); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("empty instant encoded: %v", err)
	}
	if _, err := EncodeValidAtRequest(42, "2026-08-18\x0012:00:00"); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("instant with an embedded NUL encoded: %v", err)
	}
}

func TestLinkDeleteMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "link_delete")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeLinkDeleteRequest(operation.Request.LinkID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	linkID, err := DecodeLinkDeleteRequest(wantRequest)
	if err != nil || linkID != operation.Request.LinkID {
		t.Fatalf("positive request = (%d, %v)", linkID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeLinkDeleteRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeLinkDeleteReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeLinkDeleteReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeLinkDeleteReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	if _, err := EncodeLinkDeleteRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero link encoded: %v", err)
	}
}

func TestTouchMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "touch")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeTouchRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeTouchRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeTouchRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeTouchReply()
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		if err := DecodeTouchReply(got); err != nil {
			t.Fatalf("decode: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeTouchReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	if _, err := EncodeTouchRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero memory encoded: %v", err)
	}
}

func TestDeleteRowMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "delete_row")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeDeleteRowRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeDeleteRowRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeDeleteRowRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeDeleteRowReply(vector.DeletedRows)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		removed, err := DecodeDeleteRowReply(got)
		if err != nil || removed != vector.DeletedRows {
			t.Fatalf("decode = (%d, %v)", removed, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		removed, err := DecodeDeleteRowReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || removed != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, removed, err)
		}
	}
	// A primary-key delete removes at most one row on this side too.
	if _, err := EncodeDeleteRowReply(DeleteRowMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("count past the single-row bound encoded: %v", err)
	}
	if _, err := EncodeDeleteRowRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero memory encoded: %v", err)
	}
}

func TestHasWorkspaceTagMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "has_workspace_tag")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeHasWorkspaceTagRequest(operation.Request.MemoryID)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, err := DecodeHasWorkspaceTagRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID {
		t.Fatalf("positive request = (%d, %v)", memoryID, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, err := DecodeHasWorkspaceTagRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeHasWorkspaceTagReply(vector.Tagged)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		tagged, err := DecodeHasWorkspaceTagReply(got)
		if err != nil || tagged != vector.Tagged {
			t.Fatalf("decode = (%d, %v)", tagged, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		tagged, err := DecodeHasWorkspaceTagReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || tagged != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, tagged, err)
		}
	}
	// The probe is LIMIT 1, so the flag stays Boolean on this side too.
	if _, err := EncodeHasWorkspaceTagReply(HasWorkspaceTagMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("non-Boolean flag encoded: %v", err)
	}
	if _, err := EncodeHasWorkspaceTagRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero memory encoded: %v", err)
	}
}

func TestDemoteIDPolicyIsFixed(t *testing.T) {
	// Compared as bits, not as floats: a constant that rounds differently on
	// one side would decay rows by a different factor than the other.
	if math.Float64frombits(DemoteIDMultiplierBits) != 0.9 ||
		math.Float64frombits(DemoteIDMinimumConfidenceBits) != 0.3 {
		t.Fatalf("policy = (%v, %v)", math.Float64frombits(DemoteIDMultiplierBits),
			math.Float64frombits(DemoteIDMinimumConfidenceBits))
	}
	// A primary-key equality can touch at most one row.
	if _, err := EncodeDemoteIDReply(DemoteIDCountMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("count past the single-row bound encoded: %v", err)
	}
	if _, err := EncodeDemoteIDRequest(0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero memory encoded: %v", err)
	}
}

func TestLifecycleSweepExpiredPolicyIsFixed(t *testing.T) {
	// Both states and the reason are compiled in on each side; a drift would
	// have the two implementations archiving different rows, or labelling them
	// with a reason the other never writes.
	if LifecycleSweepExpiredSourceState != "pending" ||
		LifecycleSweepExpiredTargetState != "archived" ||
		LifecycleSweepExpiredReason != "pending_ttl_expired" {
		t.Fatalf("policy = (%q, %q, %q)", LifecycleSweepExpiredSourceState,
			LifecycleSweepExpiredTargetState, LifecycleSweepExpiredReason)
	}
	if _, err := EncodeLifecycleSweepExpiredReply(LifecycleSweepExpiredCountMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("count past the bound encoded: %v", err)
	}
}

func TestPruneOrphanedL0PolicyIsFixed(t *testing.T) {
	// The tier and window are compiled-in policy shared with the C header; a
	// drift here would let one side sweep a different set of rows.
	if PruneOrphanedL0Tier != "L0" || PruneOrphanedL0MaxAge != "-7 days" {
		t.Fatalf("policy = (%q, %q)", PruneOrphanedL0Tier, PruneOrphanedL0MaxAge)
	}
	if _, err := EncodePruneOrphanedL0Reply(PruneOrphanedL0CountMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("count past the bound encoded: %v", err)
	}
}

func TestTotalCountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "total_count")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeTotalCountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeTotalCountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeTotalCountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeTotalCountReply(vector.Count)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeTotalCountReply(got)
		if err != nil || count != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeTotalCountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestSessionL2CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "session_l2_count")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeSessionL2CountRequest(operation.Request.SourceSession)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	if session, err := DecodeSessionL2CountRequest(wantRequest); err != nil || session != operation.Request.SourceSession {
		t.Fatalf("positive request = (%q, %v)", session, err)
	}
	for _, vector := range operation.Request.Negative {
		session, err := DecodeSessionL2CountRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || session != "" {
			t.Fatalf("negative request %s = (%q, %v)", vector.Mutation, session, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeSessionL2CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeSessionL2CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeSessionL2CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestKeyExistsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "key_exists")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeKeyExistsRequest(operation.Request.Key)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	if key, err := DecodeKeyExistsRequest(wantRequest); err != nil || key != operation.Request.Key {
		t.Fatalf("positive request = (%q, %v)", key, err)
	}
	for _, vector := range operation.Request.Negative {
		key, err := DecodeKeyExistsRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || key != "" {
			t.Fatalf("negative request %s = (%q, %v)", vector.Mutation, key, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeKeyExistsReply(vector.Exists)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		exists, err := DecodeKeyExistsReply(got)
		if err != nil || exists != vector.Exists {
			t.Fatalf("decode = (%d, %v)", exists, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		exists, err := DecodeKeyExistsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || exists != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, exists, err)
		}
	}
}

func TestFindIDByKeyKindMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "find_id_by_key_kind")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeFindIDByKeyKindRequest(operation.Request.Key, operation.Request.Kind)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	key, kind, err := DecodeFindIDByKeyKindRequest(wantRequest)
	if err != nil || key != operation.Request.Key || kind != operation.Request.Kind {
		t.Fatalf("positive request = (%q, %q, %v)", key, kind, err)
	}
	for _, vector := range operation.Request.Negative {
		key, kind, err := DecodeFindIDByKeyKindRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || key != "" || kind != "" {
			t.Fatalf("negative request %s = (%q, %q, %v)", vector.Mutation, key, kind, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeFindIDByKeyKindReply(vector.Found, vector.ID)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		found, id, err := DecodeFindIDByKeyKindReply(got)
		if err != nil || found != vector.Found || id != vector.ID {
			t.Fatalf("decode = (%d, %d, %v)", found, id, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		found, id, err := DecodeFindIDByKeyKindReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || found != 0 || id != 0 {
			t.Fatalf("negative reply %s = (%d, %d, %v)", vector.Mutation, found, id, err)
		}
	}
}

func TestKeyExistsInTierPairMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "key_exists_in_tier_pair")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeKeyExistsInTierPairRequest(
		operation.Request.Key, operation.Request.TierA, operation.Request.TierB)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	key, tierA, tierB, err := DecodeKeyExistsInTierPairRequest(wantRequest)
	if err != nil || key != operation.Request.Key || tierA != operation.Request.TierA ||
		tierB != operation.Request.TierB {
		t.Fatalf("positive request = (%q, %q, %q, %v)", key, tierA, tierB, err)
	}
	for _, vector := range operation.Request.Negative {
		key, tierA, tierB, err := DecodeKeyExistsInTierPairRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || key != "" || tierA != "" || tierB != "" {
			t.Fatalf("negative request %s = (%q, %q, %q, %v)",
				vector.Mutation, key, tierA, tierB, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeKeyExistsInTierPairReply(vector.Exists)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		exists, err := DecodeKeyExistsInTierPairReply(got)
		if err != nil || exists != vector.Exists {
			t.Fatalf("decode = (%d, %v)", exists, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		exists, err := DecodeKeyExistsInTierPairReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || exists != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, exists, err)
		}
	}
}

func TestEffectivenessUpdateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "effectiveness_update")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeEffectivenessUpdateRequest(
		operation.Request.MemoryID, operation.Request.HasValue,
		math.Float64frombits(operation.Request.ValueBits))
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	memoryID, hasValue, value, err := DecodeEffectivenessUpdateRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID ||
		hasValue != operation.Request.HasValue || math.Float64bits(value) != operation.Request.ValueBits {
		t.Fatalf("positive request = (%d, %d, %x, %v)",
			memoryID, hasValue, math.Float64bits(value), err)
	}
	for _, vector := range operation.Request.Negative {
		memoryID, hasValue, value, err := DecodeEffectivenessUpdateRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || memoryID != 0 || hasValue != 0 || value != 0 {
			t.Fatalf("negative request %s = (%d, %d, %v, %v)",
				vector.Mutation, memoryID, hasValue, value, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEffectivenessUpdateReply(vector.Result)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, err := DecodeEffectivenessUpdateReply(got)
		if err != nil || result != vector.Result {
			t.Fatalf("decode = (%d, %v)", result, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, err := DecodeEffectivenessUpdateReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, result, err)
		}
	}
}

func TestRetentionEnforceMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "retention_enforce")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeRetentionEnforceRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeRetentionEnforceRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeRetentionEnforceRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeRetentionEnforceReply(vector.DeletedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		deletedCount, err := DecodeRetentionEnforceReply(got)
		if err != nil || deletedCount != vector.DeletedCount {
			t.Fatalf("decode = (%d, %v)", deletedCount, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		deletedCount, err := DecodeRetentionEnforceReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || deletedCount != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, deletedCount, err)
		}
	}
}

func TestEffectivenessDemoteMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "effectiveness_demote")]
	if operation.Request.ThresholdBits != EffectivenessDemoteThresholdBits ||
		math.Float64bits(0.3) != EffectivenessDemoteThresholdBits {
		t.Fatalf("threshold bits = %x, generated = %x",
			operation.Request.ThresholdBits, EffectivenessDemoteThresholdBits)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEffectivenessDemoteRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEffectivenessDemoteRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEffectivenessDemoteRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEffectivenessDemoteReply(vector.DemotedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		demotedCount, err := DecodeEffectivenessDemoteReply(got)
		if err != nil || demotedCount != vector.DemotedCount {
			t.Fatalf("decode = (%d, %v)", demotedCount, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		demotedCount, err := DecodeEffectivenessDemoteReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || demotedCount != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, demotedCount, err)
		}
	}
}

func TestEffectivenessStatsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "effectiveness_stats")]
	if operation.Request.LowThresholdBits != EffectivenessStatsLowThresholdBits ||
		math.Float64bits(0.3) != EffectivenessStatsLowThresholdBits {
		t.Fatalf("low threshold bits = %x, generated = %x",
			operation.Request.LowThresholdBits, EffectivenessStatsLowThresholdBits)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEffectivenessStatsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEffectivenessStatsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEffectivenessStatsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		want := EffectivenessStats{
			AvgEffectiveness:      math.Float64frombits(vector.AvgEffectivenessBits),
			LowEffectivenessCount: vector.LowEffectivenessCount,
			HighImpactCount:       vector.HighImpactCount,
		}
		got, err := EncodeEffectivenessStatsReply(want)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		stats, err := DecodeEffectivenessStatsReply(got)
		if err != nil || stats != want {
			t.Fatalf("decode = (%+v, %v)", stats, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		stats, err := DecodeEffectivenessStatsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || stats != (EffectivenessStats{}) {
			t.Fatalf("negative reply %s = (%+v, %v)", vector.Mutation, stats, err)
		}
	}
	// The average is a probability, so neither bound may be encodable past its edge.
	for _, average := range []float64{-0.5, 1.5, math.NaN(), math.Inf(1)} {
		if _, err := EncodeEffectivenessStatsReply(EffectivenessStats{AvgEffectiveness: average}); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("average %v encoded: %v", average, err)
		}
	}
}

func TestL2MemoryIDsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "l2_memory_ids")]
	if operation.Request.MaximumIDs != L2MemoryIDsMax {
		t.Fatalf("maximum ids = %d, generated = %d", operation.Request.MaximumIDs, L2MemoryIDsMax)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeL2MemoryIDsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeL2MemoryIDsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeL2MemoryIDsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeL2MemoryIDsReply(vector.MemoryIDs)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		memoryIDs, err := DecodeL2MemoryIDsReply(got)
		if err != nil || len(memoryIDs) != len(vector.MemoryIDs) {
			t.Fatalf("decode = (%v, %v)", memoryIDs, err)
		}
		for index, id := range memoryIDs {
			if id != vector.MemoryIDs[index] {
				t.Fatalf("decode[%d] = %d, want %d", index, id, vector.MemoryIDs[index])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		memoryIDs, err := DecodeL2MemoryIDsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || memoryIDs != nil {
			t.Fatalf("negative reply %s = (%v, %v)", vector.Mutation, memoryIDs, err)
		}
	}
	// The declared bound is a ceiling, and identifiers stay positive.
	if _, err := EncodeL2MemoryIDsReply(make([]uint64, L2MemoryIDsMax+1)); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("over-bound list encoded: %v", err)
	}
	if _, err := EncodeL2MemoryIDsReply([]uint64{0}); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero identifier encoded: %v", err)
	}
}

func TestHealthRecordMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "health_record")]
	if operation.Request.ConflictWindowDays != HealthRecordConflictWindowDays {
		t.Fatalf("conflict window = %d, generated = %d",
			operation.Request.ConflictWindowDays, HealthRecordConflictWindowDays)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	got, err := EncodeHealthRecordRequest(operation.Request.Promotions,
		operation.Request.Demotions, operation.Request.Expirations)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	promotions, demotions, expirations, err := DecodeHealthRecordRequest(wantRequest)
	if err != nil || promotions != operation.Request.Promotions ||
		demotions != operation.Request.Demotions ||
		expirations != operation.Request.Expirations {
		t.Fatalf("decode = (%d, %d, %d, %v)", promotions, demotions, expirations, err)
	}
	for _, vector := range operation.Request.Negative {
		if _, _, _, err := DecodeHealthRecordRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for range operation.Reply.Positive {
		reply, err := EncodeHealthRecordReply()
		if err != nil || string(reply) != string(decodeHex(t, operation.Reply.Positive[0].Hex)) {
			t.Fatalf("positive reply = (%x, %v)", reply, err)
		}
		if err := DecodeHealthRecordReply(reply); err != nil {
			t.Fatalf("decode reply: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeHealthRecordReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	// Each counter is bounded independently.
	for index := range 3 {
		counters := []uint32{0, 0, 0}
		counters[index] = HealthRecordCounterMax + 1
		if _, err := EncodeHealthRecordRequest(counters[0], counters[1], counters[2]); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("counter %d past its bound encoded: %v", index, err)
		}
	}
}

func TestHealthRetentionMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "health_retention")]
	if operation.Request.SnapshotRetentionDays != HealthRetentionSnapshotDays ||
		operation.Request.ContradictionRetentionDays != HealthRetentionContradictionDays {
		t.Fatalf("retention policy = (%d, %d), generated = (%d, %d)",
			operation.Request.SnapshotRetentionDays, operation.Request.ContradictionRetentionDays,
			HealthRetentionSnapshotDays, HealthRetentionContradictionDays)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeHealthRetentionRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeHealthRetentionRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeHealthRetentionRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeHealthRetentionReply(vector.SnapshotsDeleted, vector.ContradictionsDeleted)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		snapshots, contradictions, err := DecodeHealthRetentionReply(got)
		if err != nil || snapshots != vector.SnapshotsDeleted ||
			contradictions != vector.ContradictionsDeleted {
			t.Fatalf("decode = (%d, %d, %v)", snapshots, contradictions, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		snapshots, contradictions, err := DecodeHealthRetentionReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || snapshots != 0 || contradictions != 0 {
			t.Fatalf("negative reply %s = (%d, %d, %v)", vector.Mutation, snapshots, contradictions, err)
		}
	}
	// Each half is bounded independently.
	if _, err := EncodeHealthRetentionReply(HealthRetentionMax+1, 0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("snapshot count past its bound encoded: %v", err)
	}
	if _, err := EncodeHealthRetentionReply(0, HealthRetentionMax+1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("contradiction count past its bound encoded: %v", err)
	}
}

func TestHealthCountersMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "health_counters")]
	if operation.Request.PromoteUseCount != HealthCountersPromoteUseCount ||
		operation.Request.PromoteConfidenceBits != HealthCountersPromoteConfidenceBits ||
		math.Float64bits(0.9) != HealthCountersPromoteConfidenceBits {
		t.Fatalf("promotion policy = (%d, %x), generated = (%d, %x)",
			operation.Request.PromoteUseCount, operation.Request.PromoteConfidenceBits,
			HealthCountersPromoteUseCount, HealthCountersPromoteConfidenceBits)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeHealthCountersRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeHealthCountersRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeHealthCountersRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		want := HealthCounters{
			Cycles:              vector.Counters["cycles"],
			TotalContradictions: vector.Counters["total_contradictions"],
			TotalPromotions:     vector.Counters["total_promotions"],
			TotalDemotions:      vector.Counters["total_demotions"],
			TotalExpirations:    vector.Counters["total_expirations"],
			NewMemories:         vector.Counters["new_memories"],
			L1Eligible:          vector.Counters["l1_eligible"],
			L2Total:             vector.Counters["l2_total"],
			L2Stale30Days:       vector.Counters["l2_stale_30_days"],
		}
		got, err := EncodeHealthCountersReply(want)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		counters, err := DecodeHealthCountersReply(got)
		if err != nil || counters != want {
			t.Fatalf("decode = (%+v, %v)", counters, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		counters, err := DecodeHealthCountersReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || counters != (HealthCounters{}) {
			t.Fatalf("negative reply %s = (%+v, %v)", vector.Mutation, counters, err)
		}
	}
	// Each counter is bounded independently, wherever it sits on the wire.
	for index := range HealthCountersFields {
		var counters HealthCounters
		values := [HealthCountersFields]*uint32{
			&counters.Cycles, &counters.TotalContradictions, &counters.TotalPromotions,
			&counters.TotalDemotions, &counters.TotalExpirations, &counters.NewMemories,
			&counters.L1Eligible, &counters.L2Total, &counters.L2Stale30Days,
		}
		*values[index] = HealthCountersMax + 1
		if _, err := EncodeHealthCountersReply(counters); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("counter %d past its bound encoded: %v", index, err)
		}
	}
}

func TestStatsCountsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "stats_counts")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeStatsCountsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeStatsCountsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeStatsCountsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		if len(vector.TierCounts) != StatsCountsTiers || len(vector.KindCounts) != StatsCountsKinds {
			t.Fatalf("vector buckets = (%d, %d), generated = (%d, %d)",
				len(vector.TierCounts), len(vector.KindCounts), StatsCountsTiers, StatsCountsKinds)
		}
		var want MemoryStats
		copy(want.TierCounts[:], vector.TierCounts)
		copy(want.KindCounts[:], vector.KindCounts)
		want.Total = vector.Total
		want.Conflicts = vector.Conflicts
		got, err := EncodeStatsCountsReply(want)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		stats, err := DecodeStatsCountsReply(got)
		if err != nil || stats != want {
			t.Fatalf("decode = (%+v, %v)", stats, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		stats, err := DecodeStatsCountsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || stats != (MemoryStats{}) {
			t.Fatalf("negative reply %s = (%+v, %v)", vector.Mutation, stats, err)
		}
	}
	// Every bucket is bounded, including the last kind a short mapping would drop.
	var overflow MemoryStats
	overflow.KindCounts[StatsCountsKinds-1] = StatsCountsMax + 1
	if _, err := EncodeStatsCountsReply(overflow); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("last kind bucket past its bound encoded: %v", err)
	}
	overflow = MemoryStats{Conflicts: StatsCountsMax + 1}
	if _, err := EncodeStatsCountsReply(overflow); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("conflict count past its bound encoded: %v", err)
	}
}

func TestExpireMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "expire")]
	if operation.Request.StaleL1Tier != ExpireStaleTier ||
		operation.Request.MaximumKinds != ExpireKindsMax {
		t.Fatalf("expiry policy = (%q, %d), generated = (%q, %d)",
			operation.Request.StaleL1Tier, operation.Request.MaximumKinds,
			ExpireStaleTier, ExpireKindsMax)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeExpireRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeExpireRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeExpireRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeExpireReply(vector.Level0Deleted, vector.StaleLevel1Deleted)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		level0, stale, err := DecodeExpireReply(got)
		if err != nil || level0 != vector.Level0Deleted || stale != vector.StaleLevel1Deleted {
			t.Fatalf("decode = (%d, %d, %v)", level0, stale, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		level0, stale, err := DecodeExpireReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || level0 != 0 || stale != 0 {
			t.Fatalf("negative reply %s = (%d, %d, %v)", vector.Mutation, level0, stale, err)
		}
	}
	// Each stage is bounded independently.
	if _, err := EncodeExpireReply(ExpireMax+1, 0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("L0 count past its bound encoded: %v", err)
	}
	if _, err := EncodeExpireReply(0, ExpireMax+1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("stale L1 count past its bound encoded: %v", err)
	}
}

func TestDemoteMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "demote")]
	if operation.Request.DemoteTier != DemoteTier ||
		operation.Request.MaximumKinds != DemoteKindsMax {
		t.Fatalf("demotion policy = (%q, %d), generated = (%q, %d)",
			operation.Request.DemoteTier, operation.Request.MaximumKinds,
			DemoteTier, DemoteKindsMax)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeDemoteRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeDemoteRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeDemoteRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeDemoteReply(vector.DemotedCount, vector.CascadedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		demoted, cascaded, err := DecodeDemoteReply(got)
		if err != nil || demoted != vector.DemotedCount || cascaded != vector.CascadedCount {
			t.Fatalf("decode = (%d, %d, %v)", demoted, cascaded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		demoted, cascaded, err := DecodeDemoteReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || demoted != 0 || cascaded != 0 {
			t.Fatalf("negative reply %s = (%d, %d, %v)", vector.Mutation, demoted, cascaded, err)
		}
	}
	// The cascade only runs when something was demoted.
	if _, err := EncodeDemoteReply(0, 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("cascade without demotion encoded: %v", err)
	}
	if _, err := EncodeDemoteReply(DemoteMax+1, 0); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("demoted count past its bound encoded: %v", err)
	}
	if _, err := EncodeDemoteReply(1, DemoteMax+1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("cascaded count past its bound encoded: %v", err)
	}
}

func TestPromoteStableMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "promote_stable")]
	request := operation.Request
	if request.SourceTier != PromoteStableSourceTier || request.TargetTier != PromoteStableTargetTier ||
		request.ConfidenceBits != PromoteStableConfidenceBits ||
		request.UseCount != PromoteStableUseCount || request.StableDays != PromoteStableDays ||
		math.Float64bits(0.95) != PromoteStableConfidenceBits {
		t.Fatalf("stability policy = (%q, %q, %x, %d, %d), generated = (%q, %q, %x, %d, %d)",
			request.SourceTier, request.TargetTier, request.ConfidenceBits, request.UseCount,
			request.StableDays, PromoteStableSourceTier, PromoteStableTargetTier,
			PromoteStableConfidenceBits, PromoteStableUseCount, PromoteStableDays)
	}
	if len(request.Kinds) != 2 || request.Kinds[0] != "fact" || request.Kinds[1] != "preference" {
		t.Fatalf("promotable kinds = %v", request.Kinds)
	}
	wantRequest := decodeHex(t, request.Positive)
	if got := EncodePromoteStableRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodePromoteStableRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range request.Negative {
		if err := DecodePromoteStableRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodePromoteStableReply(vector.PromotedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		promoted, err := DecodePromoteStableReply(got)
		if err != nil || promoted != vector.PromotedCount {
			t.Fatalf("decode = (%d, %v)", promoted, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		promoted, err := DecodePromoteStableReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || promoted != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, promoted, err)
		}
	}
	if _, err := EncodePromoteStableReply(PromoteStableMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("promoted count past its bound encoded: %v", err)
	}
}

func TestReclassifyDirectivesMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "reclassify_directives")]
	request := operation.Request
	if request.SourceTier != ReclassifyDirectivesSourceTier ||
		request.TargetTier != ReclassifyDirectivesTargetTier ||
		request.GatedKind != ReclassifyDirectivesGatedKind {
		t.Fatalf("directive policy = (%q, %q, %q), generated = (%q, %q, %q)",
			request.SourceTier, request.TargetTier, request.GatedKind,
			ReclassifyDirectivesSourceTier, ReclassifyDirectivesTargetTier,
			ReclassifyDirectivesGatedKind)
	}
	// Both gate settings round-trip: the gated request and the open one.
	for _, probe := range []struct {
		gate uint32
		hex  string
	}{{request.RequireApproval, request.Positive}, {0, request.OpenPositive}} {
		want := decodeHex(t, probe.hex)
		got, err := EncodeReclassifyDirectivesRequest(probe.gate)
		if err != nil || string(got) != string(want) {
			t.Fatalf("request gate=%d = (%x, %v), want %x", probe.gate, got, err, want)
		}
		gate, err := DecodeReclassifyDirectivesRequest(want)
		if err != nil || gate != probe.gate {
			t.Fatalf("decode gate = (%d, %v), want %d", gate, err, probe.gate)
		}
	}
	for _, vector := range request.Negative {
		if _, err := DecodeReclassifyDirectivesRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeReclassifyDirectivesReply(vector.ReclassifiedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		reclassified, err := DecodeReclassifyDirectivesReply(got)
		if err != nil || reclassified != vector.ReclassifiedCount {
			t.Fatalf("decode = (%d, %v)", reclassified, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		reclassified, err := DecodeReclassifyDirectivesReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || reclassified != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, reclassified, err)
		}
	}
	// The gate is a boolean.
	if _, err := EncodeReclassifyDirectivesRequest(ReclassifyDirectivesGateMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("gate past its bound encoded: %v", err)
	}
	if _, err := EncodeReclassifyDirectivesReply(ReclassifyDirectivesMax + 1); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("reclassified count past its bound encoded: %v", err)
	}
}

func TestRecordL4ApprovalMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "record_l4_approval")]
	request := operation.Request
	if request.TargetTier != RecordL4ApprovalTier {
		t.Fatalf("approved tier = %q, generated = %q", request.TargetTier, RecordL4ApprovalTier)
	}
	wantRequest := decodeHex(t, request.Positive)
	got, err := EncodeRecordL4ApprovalRequest(request.MemoryID, request.Approver, request.Note)
	if err != nil || string(got) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", got, err, wantRequest)
	}
	memoryID, approver, note, err := DecodeRecordL4ApprovalRequest(wantRequest)
	if err != nil || memoryID != request.MemoryID || approver != request.Approver ||
		note != request.Note {
		t.Fatalf("decode = (%d, %q, %q, %v)", memoryID, approver, note, err)
	}
	// An empty note is legal and round-trips.
	wantBare := decodeHex(t, request.BarePositive)
	bare, err := EncodeRecordL4ApprovalRequest(request.MemoryID, request.Approver, "")
	if err != nil || string(bare) != string(wantBare) {
		t.Fatalf("bare request = (%x, %v), want %x", bare, err, wantBare)
	}
	if _, _, note, err := DecodeRecordL4ApprovalRequest(wantBare); err != nil || note != "" {
		t.Fatalf("bare decode note = (%q, %v)", note, err)
	}
	for _, vector := range request.Negative {
		if _, _, _, err := DecodeRecordL4ApprovalRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for range operation.Reply.Positive {
		reply, err := EncodeRecordL4ApprovalReply()
		if err != nil || string(reply) != string(decodeHex(t, operation.Reply.Positive[0].Hex)) {
			t.Fatalf("positive reply = (%x, %v)", reply, err)
		}
		if err := DecodeRecordL4ApprovalReply(reply); err != nil {
			t.Fatalf("decode reply: %v", err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		if err := DecodeRecordL4ApprovalReply(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative reply %s: %v", vector.Mutation, err)
		}
	}
	// The approver is required, both bounds hold, and NULs are refused.
	for _, probe := range []struct {
		name     string
		id       uint64
		approver string
		note     string
	}{
		{"zero id", 0, "operator", ""},
		{"empty approver", 42, "", ""},
		{"approver too long", 42, strings.Repeat("a", RecordL4ApprovalApproverMax+1), ""},
		{"note too long", 42, "operator", strings.Repeat("n", RecordL4ApprovalNoteMax+1)},
		{"approver NUL", 42, "oper\x00tor", ""},
		{"note NUL", 42, "operator", "re\x00viewed"},
	} {
		if _, err := EncodeRecordL4ApprovalRequest(probe.id, probe.approver, probe.note); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("%s encoded: %v", probe.name, err)
		}
	}
}

func TestEmbedderServingIDMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "embedder_serving_id")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEmbedderServingIDRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEmbedderServingIDRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEmbedderServingIDRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEmbedderServingIDReply(vector.Result, vector.ServingID)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, servingID, err := DecodeEmbedderServingIDReply(got)
		if err != nil || result != vector.Result || servingID != vector.ServingID {
			t.Fatalf("decode = (%d, %q, %v)", result, servingID, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, servingID, err := DecodeEmbedderServingIDReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || servingID != "" {
			t.Fatalf("negative reply %s = (%d, %q, %v)", vector.Mutation, result, servingID, err)
		}
	}
}

func TestDimensionResetMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "dimension_reset")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	gotRequest, err := EncodeDimensionResetRequest(384, 0, 1)
	if err != nil || string(gotRequest) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", gotRequest, err, wantRequest)
	}
	target, force, dryRun, err := DecodeDimensionResetRequest(gotRequest)
	if err != nil || target != 384 || force != 0 || dryRun != 1 {
		t.Fatalf("decoded request = (%d, %d, %d, %v)", target, force, dryRun, err)
	}
	for _, vector := range operation.Request.Negative {
		target, force, dryRun, err := DecodeDimensionResetRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || target != 0 || force != 0 || dryRun != 0 {
			t.Fatalf("negative request %s = (%d, %d, %d, %v)",
				vector.Mutation, target, force, dryRun, err)
		}
	}
	expected := DimensionReset{
		RecordedDimension: 768, TargetDimension: 384, TablesDiscovered: 6,
		RowsCleared: 1234,
	}
	for _, vector := range operation.Reply.Positive {
		status := expected
		if vector.Result == ResultInvalidState {
			status = DimensionReset{}
		}
		got, err := EncodeDimensionResetReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeDimensionResetReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeDimensionResetReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (DimensionReset{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestReembedClearMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "reembed_clear")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeReembedClearRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeReembedClearRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeReembedClearReply(vector.Result)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, err := DecodeReembedClearReply(got)
		if err != nil || result != vector.Result {
			t.Fatalf("decode = (%d, %v)", result, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, err := DecodeReembedClearReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, result, err)
		}
	}
}

func TestReembedClearMaintenanceMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "reembed_clear_maintenance")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	gotRequest, err := EncodeReembedClearMaintenanceRequest(0)
	if err != nil || string(gotRequest) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", gotRequest, err, wantRequest)
	}
	force, err := DecodeReembedClearMaintenanceRequest(gotRequest)
	if err != nil || force != 0 {
		t.Fatalf("decoded force = (%d, %v)", force, err)
	}
	for _, vector := range operation.Request.Negative {
		force, err := DecodeReembedClearMaintenanceRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || force != 0 {
			t.Fatalf("negative request %s = (%d, %v)", vector.Mutation, force, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := ReembedClearMaintenance{
			WasInProgress: vector.WasInProgress, RecordedDimension: vector.RecordedDim,
			RunningDimension: vector.RunningDim,
		}
		got, err := EncodeReembedClearMaintenanceReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeReembedClearMaintenanceReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeReembedClearMaintenanceReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 ||
			status != (ReembedClearMaintenance{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestReembedStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "reembed_status")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeReembedStatusRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeReembedStatusRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := ReembedStatus{vector.TargetDim, vector.StartedEpoch}
		got, err := EncodeReembedStatusReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeReembedStatusReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeReembedStatusReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (ReembedStatus{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestPostgresStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "postgres_status")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodePostgresStatusRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodePostgresStatusRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := PostgresStatus{vector.Available, vector.Active, vector.Maximum, vector.IsReplica, vector.ReplicaLag}
		got, err := EncodePostgresStatusReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodePostgresStatusReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodePostgresStatusReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (PostgresStatus{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestEmbeddingRefusalsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "embedding_refusals")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEmbeddingRefusalsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEmbeddingRefusalsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := EmbeddingRefusals{vector.RefusedCount, vector.LastOffered}
		got, err := EncodeEmbeddingRefusalsReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeEmbeddingRefusalsReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeEmbeddingRefusalsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (EmbeddingRefusals{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestPoolStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "pool_status")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodePoolStatusRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodePoolStatusRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		t.Run("request_"+vector.Mutation, func(t *testing.T) {
			if err := DecodePoolStatusRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("negative request error = %v", err)
			}
		})
	}
	for _, vector := range operation.Reply.Positive {
		vector := vector
		t.Run("reply_"+vector.Hex, func(t *testing.T) {
			status := PoolStatus{vector.Size, vector.InUse, vector.Waiters, vector.LeaseGrants,
				vector.LeaseTimeouts, vector.Stuck, vector.Poisoned}
			got, err := EncodePoolStatusReply(vector.Result, status)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			want := decodeHex(t, vector.Hex)
			if string(got) != string(want) {
				t.Fatalf("reply = %x, want %x", got, want)
			}
			result, decoded, err := DecodePoolStatusReply(want)
			if err != nil || result != vector.Result || decoded != status {
				t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
			}
		})
	}
	for _, vector := range operation.Reply.Negative {
		t.Run("reply_"+vector.Mutation, func(t *testing.T) {
			result, status, err := DecodePoolStatusReply(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (PoolStatus{}) {
				t.Fatalf("negative reply = (%d, %+v, %v)", result, status, err)
			}
		})
	}
}

func TestEmbeddingDimensionMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[operationIndex(t, "embedding_dimension")]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEmbeddingDimensionRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEmbeddingDimensionRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		t.Run("request_"+vector.Mutation, func(t *testing.T) {
			if err := DecodeEmbeddingDimensionRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("negative request error = %v", err)
			}
		})
	}
	for _, vector := range operation.Reply.Positive {
		vector := vector
		t.Run("reply_"+vector.Hex, func(t *testing.T) {
			got, err := EncodeEmbeddingDimensionReply(vector.Result, vector.Dimension)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			want := decodeHex(t, vector.Hex)
			if string(got) != string(want) {
				t.Fatalf("reply = %x, want %x", got, want)
			}
			result, dimension, err := DecodeEmbeddingDimensionReply(want)
			if err != nil || result != vector.Result || dimension != vector.Dimension {
				t.Fatalf("decode = (%d, %d, %v)", result, dimension, err)
			}
		})
	}
	for _, vector := range operation.Reply.Negative {
		t.Run("reply_"+vector.Mutation, func(t *testing.T) {
			result, dimension, err := DecodeEmbeddingDimensionReply(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || dimension != 0 {
				t.Fatalf("negative reply = (%d, %d, %v)", result, dimension, err)
			}
		})
	}
}

func TestGeneratedIdentityMatchesSharedCatalog(t *testing.T) {
	baseline := loadWireBaseline(t)
	if baseline.CatalogSHA256 != ContractSHA256 {
		t.Fatalf("catalog fingerprint = %q, generated Go = %q", baseline.CatalogSHA256, ContractSHA256)
	}
	if baseline.WireVersion != WireVersion {
		t.Fatalf("wire version = %d, generated Go = %d", baseline.WireVersion, WireVersion)
	}
	if EventHealth != 11521 || StageHealth != 1 || OperationHealth != 1 {
		t.Fatalf("health identity = (%d, %d, %d)", EventHealth, StageHealth, OperationHealth)
	}
}

func TestHealthRequestMatchesEverySharedCVector(t *testing.T) {
	request := loadWireBaseline(t).Operations[operationIndex(t, "health")].Request
	want := decodeHex(t, request.Positive)
	if got := EncodeHealthRequest(); string(got) != string(want) {
		t.Fatalf("encoded request = %x, want %x", got, want)
	}
	if err := DecodeHealthRequest(want); err != nil {
		t.Fatalf("positive C request rejected: %v", err)
	}
	for _, vector := range request.Negative {
		t.Run(vector.Mutation, func(t *testing.T) {
			if err := DecodeHealthRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedHealth) {
				t.Fatalf("negative C request error = %v, want ErrMalformedHealth", err)
			}
		})
	}
}

func evidenceForFlags(flags uint32) HealthEvidence {
	return HealthEvidence{
		SchemaOK:   flags&HealthFlagSchema != 0,
		HavePGTrgm: flags&HealthFlagPGTrgm != 0,
		KBTablesOK: flags&HealthFlagKBTables != 0,
	}
}

func TestHealthReplyMatchesEverySharedCVector(t *testing.T) {
	reply := loadWireBaseline(t).Operations[operationIndex(t, "health")].Reply
	if len(reply.Positive) != 8 {
		t.Fatalf("positive flag partitions = %d, want 8", len(reply.Positive))
	}
	for _, vector := range reply.Positive {
		t.Run(string(rune('0'+vector.Flags)), func(t *testing.T) {
			want := decodeHex(t, vector.Hex)
			evidence := evidenceForFlags(vector.Flags)
			if got := EncodeHealthResponse(evidence); string(got) != string(want) {
				t.Fatalf("encoded response = %x, want %x", got, want)
			}
			decoded, err := DecodeHealthResponse(want)
			if err != nil || decoded != evidence {
				t.Fatalf("decoded response = (%+v, %v), want (%+v, nil)", decoded, err, evidence)
			}
		})
	}
	for _, vector := range reply.Negative {
		t.Run(vector.Mutation, func(t *testing.T) {
			if _, err := DecodeHealthResponse(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedHealth) {
				t.Fatalf("negative C response error = %v, want ErrMalformedHealth", err)
			}
		})
	}
}

func TestHealthDecodersRejectNilAndDoNotExposePartialEvidence(t *testing.T) {
	if err := DecodeHealthRequest(nil); !errors.Is(err, ErrMalformedHealth) {
		t.Fatalf("nil request error = %v", err)
	}
	evidence, err := DecodeHealthResponse(nil)
	if !errors.Is(err, ErrMalformedHealth) || evidence != (HealthEvidence{}) {
		t.Fatalf("nil response = (%+v, %v)", evidence, err)
	}
}
