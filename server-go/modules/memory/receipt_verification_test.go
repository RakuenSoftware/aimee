package memory

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"strings"
	"testing"
)

func verificationPrepared(t *testing.T, body []byte) string {
	t.Helper()
	s := &sourceReleaseState{}
	args := receiptTestAdmission(t, s)
	digest := sha256.Sum256(body)
	args["payload_sha256"], _ = json.Marshal(hex.EncodeToString(digest[:]))
	args["payload_bytes"], _ = json.Marshal(strconv.Itoa(len(body)))
	return sourceReleaseCall(t, s, args)["prepared_detail"].(string)
}
func mustReceiptJSON(t *testing.T, v any) []byte {
	t.Helper()
	raw, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}
func TestReceiptVerificationIndependentEvidence(t *testing.T) {
	body := []byte("binary\x00payload λ")
	prepared := verificationPrepared(t, body)
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		client := clientForHandler(t, NewHandler(nil, WithDataStore(placement, nil)))
		call := func(raw string, payload any, supplied bool) map[string]any {
			args := map[string]any{"prepared_receipt": json.RawMessage(raw)}
			if supplied {
				args["payload_base64"] = payload
			}
			return runPublicCommand(t, client, "verify_receipt", string(mustReceiptJSON(t, args)))
		}
		for _, supplied := range []bool{false, true} {
			result := call(prepared, base64.StdEncoding.EncodeToString(body), supplied)
			if result["status"] != "ok" {
				t.Fatal(result)
			}
			e := result["evidence"].(map[string]any)
			want := "unavailable"
			if supplied {
				want = "matched"
			}
			if e["schema"] != "valid" || e["binding_commitment"] != "matched" || e["source_commitment"] != "matched" || e["payload_correspondence"] != want {
				t.Fatal(e)
			}
			for key, value := range map[string]string{"authenticated_producer": "unavailable", "source_version_available": "not_checked", "decision_replayed": "unavailable", "chain_included": "not_checked", "externally_compared": "not_checked", "effect_confirmed": "unavailable"} {
				if e[key] != value {
					t.Fatal(key, e)
				}
			}
			if strings.Contains(string(mustReceiptJSON(t, result)), "binary") || result["verified"] != nil {
				t.Fatal("raw content or blanket assurance", result)
			}
		}
		result := call(prepared, base64.StdEncoding.EncodeToString([]byte("changed")), true)
		if result["evidence"].(map[string]any)["payload_correspondence"] != "mismatch" {
			t.Fatal(result)
		}
		for _, payload := range []any{nil, 123, "YWJj\n", "YR==", "YQ", "%%%%", strings.Repeat("A", base64.StdEncoding.EncodedLen(receiptVerificationPayloadMax)+4)} {
			if result := call(prepared, payload, true); result["status"] != "error" {
				t.Fatal("accepted invalid payload", result)
			}
		}
		var object map[string]any
		if json.Unmarshal([]byte(prepared), &object) != nil {
			t.Fatal("decode")
		}
		reordered, _ := json.MarshalIndent(object, "", "  ")
		if result := call(string(reordered), "", false); result["status"] != "ok" || result["evidence"].(map[string]any)["binding_commitment"] != "matched" {
			t.Fatal(result)
		}
	}
}

func TestReceiptVerificationRejectsAmbiguousSchema(t *testing.T) {
	prepared := verificationPrepared(t, []byte{})
	for name, raw := range map[string]string{
		"duplicate":           strings.Replace(prepared, `"schema_version":1`, `"schema_version":1,"schema_version":1`, 1),
		"nested_duplicate":    strings.Replace(prepared, `"model":"test-model"`, `"model":"test-model","model":"test-model"`, 1),
		"case_alias":          strings.Replace(prepared, `"model":`, `"Model":`, 1),
		"unknown":             strings.Replace(prepared, `"model":`, `"extra":true,"model":`, 1),
		"missing_null":        strings.Replace(prepared, `,"token_count":null`, "", 1),
		"null_string":         strings.Replace(prepared, `"model":"test-model"`, `"model":null`, 1),
		"source_unknown":      strings.Replace(prepared, `"channel":`, `"extra":1,"channel":`, 1),
		"source_missing":      strings.Replace(prepared, `"sources":[`, `"sources":null,"discard":[`, 1),
		"wrong_stage":         strings.Replace(prepared, `"stage":"prepared"`, `"stage":"acknowledged"`, 1),
		"trailing":            prepared + `{}`,
		"oversize":            strings.Repeat(" ", providerReceiptDetailMax) + prepared,
		"future_schema":       strings.Replace(prepared, `"schema_version":1`, `"schema_version":2`, 1),
		"noncanonical_number": strings.Replace(prepared, `"schema_version":1`, `"schema_version":1.0`, 1),
	} {
		t.Run(name, func(t *testing.T) {
			if _, ok := decodePreparedReceipt([]byte(raw)); ok {
				t.Fatal("accepted ambiguous or unsupported schema")
			}
		})
	}
}

func TestReceiptVerificationDetectsChangedCommitmentsWithoutAuthenticatingForgery(t *testing.T) {
	prepared := verificationPrepared(t, []byte("body"))
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, nil)))
	var event providerReceiptEvent
	if json.Unmarshal([]byte(prepared), &event) != nil {
		t.Fatal("decode")
	}
	for _, mutate := range []func(*providerReceiptBinding){
		func(b *providerReceiptBinding) { b.Model = "changed" },
		func(b *providerReceiptBinding) { b.PayloadBytes = "9007199254740993" },
		func(b *providerReceiptBinding) { b.ProducerBuild = "changed" },
		func(b *providerReceiptBinding) { b.CallerLimitsDigest = strings.Repeat("1", 64) },
		func(b *providerReceiptBinding) { b.SourcesDigest = strings.Repeat("2", 64) },
	} {
		changed := event
		binding := *event.Binding
		changed.Binding = &binding
		mutate(&binding)
		args := map[string]any{"prepared_receipt": changed}
		result := runPublicCommand(t, client, "verify_receipt", string(mustReceiptJSON(t, args)))
		if result["status"] != "ok" || result["evidence"].(map[string]any)["binding_commitment"] != "mismatch" {
			t.Fatal(result)
		}
		changed.BindingDigest = releaseDigest(changed.Binding)
		args["prepared_receipt"] = changed
		result = runPublicCommand(t, client, "verify_receipt", string(mustReceiptJSON(t, args)))
		e := result["evidence"].(map[string]any)
		if e["binding_commitment"] != "matched" || e["authenticated_producer"] != "unavailable" || e["chain_included"] != "not_checked" {
			t.Fatal(result)
		}
	}
}
