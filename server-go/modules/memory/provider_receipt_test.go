package memory

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
	"testing"
	"time"
)

func receiptTestAdmission(t *testing.T, s *sourceReleaseState) commandArgs {
	t.Helper()
	args := sourceReleaseArgs(map[string]any{"request_id": "request", "principal": "operator", "project": "private"})
	refs := []typedProjectionRef{releaseTestRef()}
	ticket, err := s.prepare(args, map[string]any{"facts_projection": map[string]any{"retained_items": refs}})
	if err != nil {
		t.Fatal(err)
	}
	args["source_release_ticket"], _ = json.Marshal(ticket)
	args["operation"] = json.RawMessage(`"source-release-plan"`)
	plan := sourceReleaseCall(t, s, args)
	check := plan["request"].(map[string]any)["revalidation"].(map[string]any)["check_id"]
	args["operation"] = json.RawMessage(`"source-release-result"`)
	args["owner_response"], _ = json.Marshal(map[string]any{"status": "ok", "eligible": true, "check_id": check, "sources_digest": releaseDigest(refs)})
	if result := sourceReleaseCall(t, s, args); result["admitted"] != true {
		t.Fatal(result)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-plan"`)
	args["payload_sha256"], _ = json.Marshal(strings.Repeat("a", 64))
	args["payload_bytes"] = json.RawMessage(`"9007199254740993"`)
	args["route"] = json.RawMessage(`"openai_chat"`)
	args["model"] = json.RawMessage(`"test-model"`)
	args["turn_id"] = json.RawMessage(`"test-turn"`)
	args["producer_build"] = json.RawMessage(`"test-build"`)
	args["caller_limits_sha256"], _ = json.Marshal(strings.Repeat("c", 64))
	args["operator_limits_sha256"], _ = json.Marshal(strings.Repeat("d", 64))
	return args
}

func TestProviderReceiptBindingNeedsSingleFreshAdmission(t *testing.T) {
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	plan := sourceReleaseCall(t, s, args)
	if plan["status"] != "ok" || plan["durable"] != false || plan["requires_durable_acceptance"] != true {
		t.Fatal(plan)
	}
	var prepared, admitted providerReceiptEvent
	if json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared) != nil || json.Unmarshal([]byte(plan["admitted_detail"].(string)), &admitted) != nil {
		t.Fatal(plan)
	}
	if prepared.Stage != "prepared" || prepared.Binding == nil || admitted.Stage != "dispatch_admitted" || admitted.Binding != nil || prepared.AttemptID != admitted.AttemptID || prepared.BindingDigest != admitted.BindingDigest {
		t.Fatal(prepared, admitted)
	}
	if prepared.Binding.Retention != "commitment_only" || prepared.Binding.TokenCount != nil || prepared.Binding.PayloadBytes != "9007199254740993" || prepared.Binding.SourceCoverage != "retained_versioned_inputs" || prepared.BindingDigest != releaseDigest(prepared.Binding) {
		t.Fatal(prepared.Binding)
	}
	var sources []typedProjectionRef
	if json.Unmarshal(prepared.Binding.Sources, &sources) != nil || len(sources) != 1 || sources[0].Source.Version.RecordID != releaseTestRef().ID {
		t.Fatal(prepared.Binding)
	}
	original := prepared.BindingDigest
	for _, mutate := range []func(*providerReceiptBinding){
		func(b *providerReceiptBinding) { b.PayloadDigest = strings.Repeat("b", 64) },
		func(b *providerReceiptBinding) { b.Route = "anthropic_messages" },
		func(b *providerReceiptBinding) { b.Model = "changed" },
		func(b *providerReceiptBinding) { b.ProducerBuild = "different" },
		func(b *providerReceiptBinding) { b.CallerLimitsDigest = strings.Repeat("e", 64) },
		func(b *providerReceiptBinding) { b.OperatorLimitsDigest = strings.Repeat("f", 64) },
		func(b *providerReceiptBinding) { b.Sources = json.RawMessage(`[]`) },
	} {
		binding := *prepared.Binding
		mutate(&binding)
		if releaseDigest(binding) == original {
			t.Fatal("changed binding reused commitment")
		}
	}
	if reused := sourceReleaseCall(t, s, args); reused["status"] != "error" {
		t.Fatal("reused source admission", reused)
	}
	fresh := receiptTestAdmission(t, s)
	next := sourceReleaseCall(t, s, fresh)
	if next["status"] != "ok" || next["attempt_id"] == plan["attempt_id"] {
		t.Fatal(next)
	}
}

func TestProviderReceiptObservationPreservesTransportUncertainty(t *testing.T) {
	for _, status := range []int{-1, 200, 429, 503} {
		s := &sourceReleaseState{}
		args := receiptTestAdmission(t, s)
		plan := sourceReleaseCall(t, s, args)
		args["operation"] = json.RawMessage(`"provider-receipt-observe"`)
		args["attempt_id"], _ = json.Marshal(plan["attempt_id"])
		args["http_status"], _ = json.Marshal(status)
		args["response_sha256"], _ = json.Marshal(strings.Repeat("b", 64))
		args["response_bytes"] = json.RawMessage(`"2"`)
		representation := "host_buffered_response_string"
		if status == 200 || status == -1 {
			representation = "provider_stream_bytes"
		}
		args["response_representation"], _ = json.Marshal(representation)
		observed := sourceReleaseCall(t, s, args)
		if observed["durable"] != false || observed["status"] != "ok" {
			t.Fatal(observed)
		}
		var event providerReceiptEvent
		if json.Unmarshal([]byte(observed["observation_detail"].(string)), &event) != nil {
			t.Fatal(observed)
		}
		if event.ResponseRepresentation != representation {
			t.Fatal(event)
		}
		if status == -1 {
			if event.Stage != "outcome_unknown" || event.Reason != "transport_outcome_unresolved" || event.HTTPStatus != 0 {
				t.Fatal(event)
			}
		} else if event.Stage != "acknowledged" || event.HTTPStatus != status {
			t.Fatal(event)
		}
		duplicate := sourceReleaseCall(t, s, args)
		if duplicate["observation_detail"] != observed["observation_detail"] {
			t.Fatal("non-idempotent observation")
		}
		args["response_bytes"] = json.RawMessage(`"3"`)
		if changed := sourceReleaseCall(t, s, args); changed["status"] != "error" {
			t.Fatal("changed observation accepted")
		}
		args["principal"] = json.RawMessage(`"other"`)
		if changed := sourceReleaseCall(t, s, args); changed["status"] != "error" {
			t.Fatal("foreign observation accepted")
		}
		s.expire(time.Now().Add(sourceReleaseTTL + time.Second))
		if len(s.receipts) != 0 || s.receiptBytes != 0 {
			t.Fatal("receipt state did not expire")
		}
		if lost := sourceReleaseCall(t, s, args); lost["status"] != "error" {
			t.Fatal("expired observation invented acknowledgement")
		}
	}
}

func TestProviderReceiptRejectsMalformedAndOversizedBindings(t *testing.T) {
	for key, values := range map[string][]string{"payload_sha256": {`"short"`, `null`}, "payload_bytes": {`"01"`, `"-1"`, `1`, `"9223372036854775808"`}, "route": {`"unknown"`, `null`}, "principal": {`"foreign"`}} {
		for _, value := range values {
			s := &sourceReleaseState{}
			args := receiptTestAdmission(t, s)
			args[key] = json.RawMessage(value)
			if result := sourceReleaseCall(t, s, args); result["status"] != "error" || len(s.receipts) != 0 {
				t.Fatal(key, value, result)
			}
		}
	}
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	ticket := args.stringOr("source_release_ticket", "")
	s.entries[ticket].sources = json.RawMessage(`"` + strings.Repeat("x", providerReceiptDetailMax) + `"`)
	if result := sourceReleaseCall(t, s, args); result["status"] != "error" || len(s.receipts) != 0 {
		t.Fatal("oversized durable row silently truncated", result)
	}
	s = &sourceReleaseState{}
	args = receiptTestAdmission(t, s)
	s.receiptBytes = sourceReleaseMaxBytes
	if result := sourceReleaseCall(t, s, args); result["status"] != "error" {
		t.Fatal("capacity exhaustion ignored", result)
	}
}

func TestProviderReceiptResponseRepresentationCompatibility(t *testing.T) {
	for _, value := range []string{"absent", `null`, `1`, `{}`, `"unknown"`, `""`} {
		s := &sourceReleaseState{}
		args := receiptTestAdmission(t, s)
		plan := sourceReleaseCall(t, s, args)
		args["operation"] = json.RawMessage(`"provider-receipt-observe"`)
		args["attempt_id"], _ = json.Marshal(plan["attempt_id"])
		args["http_status"] = json.RawMessage(`200`)
		args["response_sha256"], _ = json.Marshal(strings.Repeat("b", 64))
		args["response_bytes"] = json.RawMessage(`"2"`)
		if value != "absent" {
			args["response_representation"] = json.RawMessage(value)
		}
		result := sourceReleaseCall(t, s, args)
		if value == "absent" {
			var event providerReceiptEvent
			if result["status"] != "ok" || json.Unmarshal([]byte(result["observation_detail"].(string)), &event) != nil || event.ResponseRepresentation != "host_buffered_response_string" {
				t.Fatal(result)
			}
		} else if result["status"] != "error" {
			t.Fatal(value, result)
		}
	}
}

func TestProviderReceiptWithoutSourceHandleIsAnExplicitGap(t *testing.T) {
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	delete(args, "source_release_ticket")
	plan := sourceReleaseCall(t, s, args)
	if plan["status"] != "ok" || plan["durable"] != false {
		t.Fatal(plan)
	}
	var event providerReceiptEvent
	if json.Unmarshal([]byte(plan["prepared_detail"].(string)), &event) != nil {
		t.Fatal(plan)
	}
	b := event.Binding
	if b.SourceCoverage != "no_versioned_source_handle" || string(b.Sources) != "[]" || b.SourceCheckID != "" || b.Workspace != "" || b.Project != "" || b.RequestBinding != releaseBinding(args) {
		t.Fatal(b)
	}
	if _, ok := decodePreparedReceipt([]byte(plan["prepared_detail"].(string))); !ok {
		t.Fatal("unversioned body commitment not verifiable")
	}
	if next := sourceReleaseCall(t, s, args); next["attempt_id"] == plan["attempt_id"] || next["status"] != "ok" {
		t.Fatal("resend reused attempt", next)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-observe"`)
	args["attempt_id"], _ = json.Marshal(plan["attempt_id"])
	args["http_status"] = json.RawMessage(`200`)
	args["response_sha256"], _ = json.Marshal(strings.Repeat("b", 64))
	args["response_bytes"] = json.RawMessage(`"2"`)
	if observed := sourceReleaseCall(t, s, args); observed["status"] != "ok" {
		t.Fatal(observed)
	}
	args["principal"] = json.RawMessage(`"other"`)
	if observed := sourceReleaseCall(t, s, args); observed["status"] != "error" {
		t.Fatal("foreign observation", observed)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-plan"`)
	for _, ticket := range []string{`null`, `false`, `{}`, `"expired-or-foreign"`} {
		args["source_release_ticket"] = json.RawMessage(ticket)
		if invalid := sourceReleaseCall(t, s, args); invalid["status"] != "error" {
			t.Fatal("downgraded invalid handle", ticket, invalid)
		}
	}
}

func TestProviderReceiptPressureReclaimsOnlyDurablyConfirmedObservations(t *testing.T) {
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	delete(args, "source_release_ticket")
	first := sourceReleaseCall(t, s, args)
	firstID := first["attempt_id"].(string)
	secondID := ""
	for i := 1; i < sourceReleaseMaxEntries; i++ {
		plan := sourceReleaseCall(t, s, args)
		if plan["status"] != "ok" {
			t.Fatal(i, plan)
		}
		if i == 1 {
			secondID = plan["attempt_id"].(string)
		}
	}
	if blocked := sourceReleaseCall(t, s, args); blocked["status"] != "error" {
		t.Fatal("unresolved admission evicted", blocked)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-stored"`)
	args["attempt_id"], _ = json.Marshal(firstID)
	args["observation_sha256"], _ = json.Marshal(strings.Repeat("a", 64))
	if stored := sourceReleaseCall(t, s, args); stored["status"] != "error" {
		t.Fatal("admission mistaken for persisted observation", stored)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-observe"`)
	args["http_status"] = json.RawMessage(`200`)
	args["response_sha256"], _ = json.Marshal(strings.Repeat("b", 64))
	args["response_bytes"] = json.RawMessage(`"2"`)
	observed := sourceReleaseCall(t, s, args)
	if observed["status"] != "ok" {
		t.Fatal(observed)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-plan"`)
	if blocked := sourceReleaseCall(t, s, args); blocked["status"] != "error" {
		t.Fatal("unpersisted observation evicted", blocked)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-stored"`)
	if stored := sourceReleaseCall(t, s, args); stored["status"] != "error" {
		t.Fatal("wrong observation accepted", stored)
	}
	args["observation_sha256"], _ = json.Marshal(releaseDigest(json.RawMessage(observed["observation_detail"].(string))))
	original := args["principal"]
	args["principal"] = json.RawMessage(`"other"`)
	if stored := sourceReleaseCall(t, s, args); stored["status"] != "error" {
		t.Fatal("foreign completion", stored)
	}
	args["principal"] = original
	for i := 0; i < 2; i++ {
		stored := sourceReleaseCall(t, s, args)
		if stored["status"] != "ok" || stored["cache_reclaimable"] != true || stored["durable"] != nil {
			t.Fatal(stored)
		}
	}
	args["operation"] = json.RawMessage(`"provider-receipt-plan"`)
	if plan := sourceReleaseCall(t, s, args); plan["status"] != "ok" || plan["attempt_id"] == firstID {
		t.Fatal(plan)
	}
	if s.receipts[firstID] != nil || s.receipts[secondID] == nil || len(s.receipts) != sourceReleaseMaxEntries {
		t.Fatal("pressure discarded unresolved state")
	}
	bytes := 0
	for _, entry := range s.receipts {
		bytes += len(entry.prepared) + len(entry.admitted) + len(entry.observation)
	}
	if s.receiptBytes != bytes || bytes > sourceReleaseMaxBytes {
		t.Fatal("cache accounting", s.receiptBytes, bytes)
	}
	args["operation"] = json.RawMessage(`"provider-receipt-observe"`)
	if expired := sourceReleaseCall(t, s, args); expired["status"] != "error" {
		t.Fatal("reclaimed entry invented new observation", expired)
	}
}

func TestProviderReceiptRecoveryStages(t *testing.T) {
	for _, stage := range []string{"prepared", "dispatch_admitted", "dispatch_started", "outcome_unknown", "acknowledged"} {
		t.Run(stage, func(t *testing.T) {
			state := &sourceReleaseState{}
			args := receiptTestAdmission(t, state)
			args["dispatch_owner"], _ = json.Marshal(strings.Repeat("a", 32))
			plan := sourceReleaseCall(t, state, args)
			var prepared providerReceiptEvent
			if json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared) != nil {
				t.Fatal(plan)
			}
			rows := []map[string]any{}
			appendEvent := func(detail string) {
				var e providerReceiptEvent
				json.Unmarshal([]byte(detail), &e)
				rows = append(rows, map[string]any{"sequence": fmt.Sprint(len(rows) + 1), "action": "memory.provider." + e.Stage, "attempt_id": e.AttemptID, "detail": detail, "row_hash": strings.Repeat("f", 64)})
			}
			appendEvent(plan["prepared_detail"].(string))
			if stage != "prepared" {
				appendEvent(plan["admitted_detail"].(string))
			}
			if stage == "dispatch_started" {
				args["operation"] = json.RawMessage(`"provider-receipt-started"`)
				args["attempt_id"], _ = json.Marshal(prepared.AttemptID)
				started := sourceReleaseCall(t, state, args)
				appendEvent(started["observation_detail"].(string))
			}
			if stage == "outcome_unknown" || stage == "acknowledged" {
				status := -1
				if stage == "acknowledged" {
					status = 200
				}
				event := providerReceiptEvent{SchemaVersion: 1, Stage: stage, AttemptID: prepared.AttemptID, BindingDigest: prepared.BindingDigest, HTTPStatus: status}
				raw, _ := json.Marshal(event)
				appendEvent(string(raw))
			}
			inspect := sourceReleaseArgs(map[string]any{"dispatch_owner": strings.Repeat("b", 32), "receipt_request_id": "request", "chain_intact": true, "ledger_events": rows})
			raw, code := inspectProviderReceipts(inspect)
			var result map[string]any
			body, decodeErr := bus.DecodeCommandResult(raw)
			if decodeErr != nil {
				t.Fatal(decodeErr)
			}
			json.Unmarshal(body, &result)
			if code != 0 || result["status"] != "ok" {
				t.Fatal(string(raw), code)
			}
			receipt := result["receipts"].([]any)[0].(map[string]any)
			want := "outcome_unknown"
			if stage == "prepared" {
				want = "prepared_without_dispatch"
			}
			if stage == "acknowledged" {
				want = stage
			}
			if receipt["state"] != want || receipt["replay"] != "unavailable_commitment_only" {
				t.Fatal(receipt)
			}
			if stage == "prepared" {
				inspect["dispatch_owner"], _ = json.Marshal(strings.Repeat("a", 32))
				raw, _ = inspectProviderReceipts(inspect)
				if !strings.Contains(string(raw), "prepared_dispatch_owner_active") {
					t.Fatal(string(raw))
				}
			}
			prepared.Binding.RendererVersion = "changed-renderer"
			changed, _ := json.Marshal(prepared)
			rows[0]["detail"] = string(changed)
			inspect["ledger_events"], _ = json.Marshal(rows)
			raw, _ = inspectProviderReceipts(inspect)
			if !strings.Contains(string(raw), `"status":"error"`) {
				t.Fatal("changed binding accepted", string(raw))
			}
		})
	}
}

func TestProviderReceiptReplayRetention(t *testing.T) {
	state := &sourceReleaseState{}
	args := receiptTestAdmission(t, state)
	payload := []byte{'a', 0, 'b'}
	digest := sha256.Sum256(payload)
	args["dispatch_owner"], _ = json.Marshal(strings.Repeat("a", 32))
	args["receipt_retention"] = json.RawMessage(`"replayable"`)
	args["payload_sha256"], _ = json.Marshal(hex.EncodeToString(digest[:]))
	args["payload_bytes"] = json.RawMessage(`"3"`)
	plan := sourceReleaseCall(t, state, args)
	if plan["replay_store"] != true {
		t.Fatal(plan)
	}
	var prepared providerReceiptEvent
	json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared)
	if _, ok := decodePreparedReceipt([]byte(plan["prepared_detail"].(string))); !ok {
		t.Fatal("replayable schema refused")
	}
	rows := []map[string]any{{"sequence": "1", "action": "memory.provider.prepared", "attempt_id": prepared.AttemptID, "detail": plan["prepared_detail"], "row_hash": strings.Repeat("f", 64)}}
	inspect := sourceReleaseArgs(map[string]any{"dispatch_owner": strings.Repeat("b", 32), "receipt_request_id": "request", "chain_intact": true, "ledger_events": rows, "include_payload": true, "replay_inputs": map[string]string{prepared.AttemptID: base64.StdEncoding.EncodeToString(payload)}})
	run := func() map[string]any {
		raw, _ := inspectProviderReceipts(inspect)
		body, err := bus.DecodeCommandResult(raw)
		var result map[string]any
		if err != nil || json.Unmarshal(body, &result) != nil {
			t.Fatal(err, string(body))
		}
		if result["status"] != "ok" {
			t.Fatal(result)
		}
		return result["receipts"].([]any)[0].(map[string]any)
	}
	result := run()
	if result["replay"] != "available_exact_payload" || result["payload_base64"] != "YQBi" {
		t.Fatal(result)
	}
	inspect["replay_inputs"], _ = json.Marshal(map[string]string{prepared.AttemptID: ""})
	result = run()
	if result["replay"] != "unavailable_inputs" || result["evidence"].(map[string]any)["chain_included"] != true {
		t.Fatal("deletion collapsed replay and inclusion", result)
	}
	inspect["forget_replay"] = json.RawMessage(`true`)
	inspect["replay_deleted"], _ = json.Marshal(map[string]bool{prepared.AttemptID: true})
	result = run()
	if result["replay"] != "unavailable_erased" {
		t.Fatal(result)
	}
}
