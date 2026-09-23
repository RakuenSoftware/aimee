package memory

import (
	"encoding/json"
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
		observed := sourceReleaseCall(t, s, args)
		if observed["durable"] != false || observed["status"] != "ok" {
			t.Fatal(observed)
		}
		var event providerReceiptEvent
		if json.Unmarshal([]byte(observed["observation_detail"].(string)), &event) != nil {
			t.Fatal(observed)
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
