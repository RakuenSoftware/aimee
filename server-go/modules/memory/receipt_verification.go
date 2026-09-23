package memory

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"reflect"
	"strconv"
	"time"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

// This verifies a caller-supplied commitment, not the origin or durable inclusion
// of that commitment. It reads no retained records and cannot grant source access.
const receiptVerificationPayloadMax = 512 << 10

// Decode without discarding duplicate names, precision, or nested schema changes.
// Comparing this tree with the typed re-encoding rejects case aliases, unknown
// fields, missing required fields and null-for-zero coercion. Object order and
// whitespace are immaterial; canonical field order still owns binding hashes.
func receiptJSONTree(raw []byte) (any, error) {
	if !utf8.Valid(raw) {
		return nil, errors.New("invalid UTF-8")
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.UseNumber()
	var read func(int) (any, error)
	read = func(depth int) (any, error) {
		if depth > 32 {
			return nil, errors.New("receipt nesting exceeds bound")
		}
		token, err := d.Token()
		if err != nil {
			return nil, err
		}
		switch token {
		case json.Delim('{'):
			object := map[string]any{}
			for d.More() {
				key, err := d.Token()
				if err != nil {
					return nil, err
				}
				name, ok := key.(string)
				if !ok {
					return nil, errors.New("invalid key")
				}
				if _, exists := object[name]; exists {
					return nil, errors.New("duplicate key")
				}
				value, err := read(depth + 1)
				if err != nil {
					return nil, err
				}
				object[name] = value
			}
			end, err := d.Token()
			if err != nil || end != json.Delim('}') {
				return nil, errors.New("invalid object")
			}
			return object, nil
		case json.Delim('['):
			array := []any{}
			for d.More() {
				value, err := read(depth + 1)
				if err != nil {
					return nil, err
				}
				array = append(array, value)
			}
			end, err := d.Token()
			if err != nil || end != json.Delim(']') {
				return nil, errors.New("invalid array")
			}
			return array, nil
		}
		if _, delim := token.(json.Delim); delim {
			return nil, errors.New("invalid delimiter")
		}
		return token, nil
	}
	value, err := read(0)
	if err != nil {
		return nil, err
	}
	if _, err = d.Token(); err != io.EOF {
		return nil, errors.New("trailing receipt data")
	}
	return value, nil
}

func decodePreparedReceipt(raw []byte) (*providerReceiptEvent, bool) {
	if len(raw) == 0 || len(raw) > providerReceiptDetailMax {
		return nil, false
	}
	tree, err := receiptJSONTree(raw)
	if err != nil {
		return nil, false
	}
	var event providerReceiptEvent
	if json.Unmarshal(raw, &event) != nil || event.Binding == nil {
		return nil, false
	}
	b := event.Binding
	var refs []typedProjectionRef
	if json.Unmarshal(b.Sources, &refs) != nil {
		return nil, false
	}
	sources := sourceRevalidation{SchemaVersion: 1, CheckID: b.SourceCheckID, Sources: refs}
	switch b.SourceCoverage {
	case "retained_versioned_inputs":
		if !sources.valid() {
			return nil, false
		}
	case "no_versioned_source_handle":
		if refs == nil || len(refs) != 0 || b.SourceCheckID != "" || b.Workspace != "" || b.Project != "" {
			return nil, false
		}
	default:
		return nil, false
	}
	b.Sources, _ = json.Marshal(refs)
	canonical, _ := json.Marshal(event)
	encoded, err := receiptJSONTree(canonical)
	if err != nil || !reflect.DeepEqual(tree, encoded) {
		return nil, false
	}
	_, err = time.Parse(time.RFC3339Nano, event.At)
	if err != nil || event.SchemaVersion != 1 || event.Stage != "prepared" || event.ResponseRepresentation != "" || event.HTTPStatus != 0 || event.ResponseDigest != "" || event.ResponseBytes != "" || event.Reason != "" ||
		!releaseTokenValid(event.AttemptID) || event.AttemptID != b.AttemptID || !receiptDigestValid(event.BindingDigest) ||
		b.SchemaVersion != 1 || !releaseTokenValid(b.ProducerID) || !receiptDigestValid(b.RequestBinding) ||
		b.Retention != "commitment_only" || b.CountProvenance != "host_final_provider_bytes" || b.TokenCount != nil ||
		!receiptDigestValid(b.PayloadDigest) || !receiptByteCount(b.PayloadBytes) || !receiptDigestValid(b.SourcesDigest) ||
		!receiptDigestValid(b.CallerLimitsDigest) || !receiptDigestValid(b.OperatorLimitsDigest) ||
		len(b.RequestID) > 256 || len(b.TurnID) > 128 || len(b.ProducerBuild) > 128 || len(b.Workspace) > 1024 || len(b.Project) > 1024 || len(b.Provider) > 1024 || len(b.Model) > 1024 ||
		(b.Route != "openai_chat" && b.Route != "openai_responses" && b.Route != "anthropic_messages") {
		return nil, false
	}
	return &event, true
}

func handleReceiptVerification(_ handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	var envelopeOK bool
	args, envelopeOK = commandDomainArgs(args, "memory.verify_receipt")
	if !envelopeOK {
		return commandResult(commandError("invalid_argument", "invalid receipt verification transport envelope"))
	}
	for key := range args {
		if key != "prepared_receipt" && key != "payload_base64" {
			return commandResult(commandError("invalid_argument", "receipt verification accepts prepared_receipt and optional payload_base64 only"))
		}
	}
	event, ok := decodePreparedReceipt(args["prepared_receipt"])
	if !ok {
		return commandResult(commandError("invalid_argument", "prepared receipt does not match the supported schema"))
	}
	var payload []byte
	payloadPresent := false
	if _, present := args["payload_base64"]; present {
		encoded, ok := args.stringValue("payload_base64")
		if !ok || len(encoded) > base64.StdEncoding.EncodedLen(receiptVerificationPayloadMax) {
			return commandResult(commandError("invalid_argument", "payload_base64 exceeds the verification bound or is not a string"))
		}
		var err error
		payload, err = base64.StdEncoding.Strict().DecodeString(encoded)
		if err != nil || base64.StdEncoding.EncodeToString(payload) != encoded {
			return commandResult(commandError("invalid_argument", "payload_base64 must be canonical padded base64"))
		}
		payloadPresent = true
	}
	evidence := map[string]string{
		"schema": "valid", "binding_commitment": "mismatch", "source_commitment": "mismatch",
		"authenticated_producer": "unavailable", "source_version_available": "not_checked",
		"payload_correspondence": "unavailable", "decision_replayed": "unavailable",
		"chain_included": "not_checked", "externally_compared": "not_checked", "effect_confirmed": "unavailable",
	}
	if event.BindingDigest == releaseDigest(event.Binding) {
		evidence["binding_commitment"] = "matched"
	}
	if event.Binding.SourcesDigest == releaseDigest(event.Binding.Sources) {
		evidence["source_commitment"] = "matched"
	}
	if payloadPresent {
		digest := sha256.Sum256(payload)
		evidence["payload_correspondence"] = "mismatch"
		if hex.EncodeToString(digest[:]) == event.Binding.PayloadDigest && strconv.Itoa(len(payload)) == event.Binding.PayloadBytes {
			evidence["payload_correspondence"] = "matched"
		}
	}
	return commandResult(map[string]any{"status": "ok", "schema_version": 1, "verification_scope": "caller_supplied_prepared_commitment", "retention_mode": "commitment_only", "source_coverage": event.Binding.SourceCoverage, "evidence": evidence})
}
