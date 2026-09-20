package economizer

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestRequestBudgetAdmission(t *testing.T) {
	for _, tc := range []struct {
		name, limits string
		size         uint64
		want         uint16
	}{
		{"exact", `{"schema_version":1,"max_request_bytes":3}`, 3, RequestBudgetAdmitted},
		{"overflow", `{"schema_version":1,"max_request_bytes":3}`, 4, RequestBudgetOverflow},
		{"zero_empty", `{"schema_version":1,"max_request_bytes":0}`, 0, RequestBudgetAdmitted},
		{"zero_nonempty", `{"schema_version":1,"max_request_bytes":0}`, 1, RequestBudgetOverflow},
		{"large_exact", `{"schema_version":1,"max_request_bytes":9007199254740993}`, 9007199254740993, RequestBudgetAdmitted},
		{"large_overflow", `{"schema_version":1,"max_request_bytes":9007199254740993}`, 9007199254740994, RequestBudgetOverflow},
		{"max_uint", `{"schema_version":1,"max_request_bytes":18446744073709551615}`, ^uint64(0), RequestBudgetAdmitted},
		{"tokens", `{"schema_version":1,"max_request_bytes":100,"max_request_tokens":0}`, 1, RequestBudgetTokensUnavailable},
		{"response_reserve", `{"schema_version":1,"reserved_response_tokens":1}`, 1, RequestBudgetTokensUnavailable},
		{"tool_reserve", `{"schema_version":1,"reserved_tool_tokens":1}`, 1, RequestBudgetTokensUnavailable},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := admitRequestBudget([]byte(tc.limits), tc.size); got != tc.want {
				t.Fatalf("got %d want %d", got, tc.want)
			}
		})
	}
	for _, limits := range []string{
		`null`, `[]`, `{}`, `{"schema_version":1}`, `{"schema_version":2,"max_request_bytes":3}`,
		`{"schema_version":1,"max_request_bytes":null}`, `{"schema_version":1,"max_request_bytes":-1}`,
		`{"schema_version":1,"max_request_bytes":1.5}`, `{"schema_version":1,"max_request_bytes":"3"}`,
		`{"schema_version":1,"max_request_bytes":18446744073709551616}`,
		`{"schema_version":1,"max_request_bytes":3,"max_request_bytes":4}`,
		`{"schema_version":1,"max_request_bytes":3,"max_request_byt\u0065s":4}`,
		`{"schema_version":1,"MAX_REQUEST_BYTES":3}`,
		`{"schema_version":1,"max_request_bytes":3,"Max_Request_Bytes":4}`,
		`{"schema_version":1,"max_request_bytes":3,"unknown":1}`,
		`{"schema_version":1,"max_request_bytes":3} {}`,
		`{"schema_version":1,"max_request_bytes":3,"max_request_tokens":null}`,
		string([]byte{'{', '"', 0xff, '"', ':', '1', '}'}),
	} {
		if got := admitRequestBudget([]byte(limits), 1); got != RequestBudgetInvalid {
			t.Errorf("accepted invalid limits %q: %d", limits, got)
		}
	}
}

func budgetFrame(route uint16, size uint64, limits string) []byte {
	frame := make([]byte, requestBudgetHeader+len(limits))
	binary.LittleEndian.PutUint32(frame, requestBudgetMagic)
	binary.LittleEndian.PutUint16(frame[4:], auxWireVersion)
	binary.LittleEndian.PutUint16(frame[6:], route)
	binary.LittleEndian.PutUint64(frame[8:], size)
	digest := sha256.Sum256([]byte("provider body"))
	copy(frame[16:48], digest[:])
	binary.LittleEndian.PutUint32(frame[48:], uint32(len(limits)))
	copy(frame[52:], limits)
	return frame
}

func policyBudgetFrame(route uint16, size uint64, limits, policy string) []byte {
	v1 := budgetFrame(route, size, limits)
	frame := make([]byte, requestBudgetPolicyHeader+len(limits)+len(policy))
	copy(frame, v1[:requestBudgetHeader])
	binary.LittleEndian.PutUint16(frame[4:], 2)
	binary.LittleEndian.PutUint32(frame[52:], uint32(len(policy)))
	copy(frame[56:], limits)
	copy(frame[56+len(limits):], policy)
	return frame
}

func TestInheritedProviderRequestAdmission(t *testing.T) {
	const policy = `{"schema_version":1,"max_request_bytes":3}`
	for _, tc := range []struct {
		name, limits, policy string
		size                 uint64
		want                 uint16
	}{
		{"no_header_exact", "", policy, 3, RequestBudgetAdmitted},
		{"no_header_overflow", "", policy, 4, RequestBudgetOverflow},
		{"inherit", `{"schema_version":1}`, policy, 4, RequestBudgetOverflow},
		{"cannot_raise", `{"schema_version":1,"max_request_bytes":999}`, policy, 4, RequestBudgetOverflow},
		{"tighten", `{"schema_version":1,"max_request_bytes":2}`, policy, 3, RequestBudgetOverflow},
		{"zero_child", `{"schema_version":1,"max_request_bytes":0}`, policy, 1, RequestBudgetOverflow},
		{"zero_operator", `{"schema_version":1,"max_request_bytes":99}`, `{"schema_version":1,"max_request_bytes":0}`, 1, RequestBudgetOverflow},
		{"empty_body_zero", "", `{"schema_version":1,"max_request_bytes":0}`, 0, RequestBudgetAdmitted},
		{"maximum_uint", `{"schema_version":1,"max_request_bytes":18446744073709551615}`, `{"schema_version":1,"max_request_bytes":9007199254740993}`, 9007199254740994, RequestBudgetOverflow},
		{"caller_tokens", `{"schema_version":1,"max_request_tokens":10}`, policy, 1, RequestBudgetTokensUnavailable},
		{"caller_null", `null`, policy, 1, RequestBudgetInvalid},
		{"caller_typo", `{"schema_version":1,"MAX_REQUEST_BYTES":1}`, policy, 1, RequestBudgetInvalid},
		{"policy_null", "", `null`, 1, RequestBudgetPolicyInvalid},
		{"policy_missing_cap", "", `{"schema_version":1}`, 1, RequestBudgetPolicyInvalid},
		{"policy_null_cap", "", `{"schema_version":1,"max_request_bytes":null}`, 1, RequestBudgetPolicyInvalid},
		{"policy_duplicate", "", `{"schema_version":1,"max_request_bytes":0,"max_request_bytes":999}`, 1, RequestBudgetPolicyInvalid},
		{"policy_schema", "", `{"schema_version":2,"max_request_bytes":999}`, 1, RequestBudgetPolicyInvalid},
		{"policy_tokens", "", `{"schema_version":1,"max_request_bytes":999,"reserved_tool_tokens":0}`, 1, RequestBudgetPolicyInvalid},
	} {
		t.Run(tc.name, func(t *testing.T) {
			for route := uint16(1); route <= 3; route++ {
				frame := policyBudgetFrame(route, tc.size, tc.limits, tc.policy)
				out, status := NewHandler()(bus.ModuleInvocation{StageID: StageRequestBudget}, frame)
				if status != bus.ModuleStatusOK || len(out) != 44 || binary.LittleEndian.Uint16(out[6:]) != tc.want {
					t.Fatalf("route %d status %v response %x want %d", route, status, out, tc.want)
				}
				digest := sha256.Sum256(frame)
				if !bytes.Equal(out[12:], digest[:]) {
					t.Fatal("decision did not bind both policy layers")
				}
			}
		})
	}
}

func TestRequestBudgetPolicyFrameRefusals(t *testing.T) {
	const policy = `{"schema_version":1,"max_request_bytes":3}`
	valid := policyBudgetFrame(1, 3, "", policy)
	frames := [][]byte{valid[:55], policyBudgetFrame(1, 3, "", ""), policyBudgetFrame(1, 3, strings.Repeat(" ", 1025), policy), policyBudgetFrame(1, 3, "", strings.Repeat(" ", 1025)), append(bytes.Clone(valid), 0)}
	for _, offset := range []int{48, 52} {
		bad := bytes.Clone(valid)
		binary.LittleEndian.PutUint32(bad[offset:], ^uint32(0))
		frames = append(frames, bad)
	}
	for _, frame := range frames {
		if _, status := NewHandler()(bus.ModuleInvocation{StageID: StageRequestBudget}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("malformed policy frame accepted: %v", status)
		}
	}
	if _, status := NewHandler()(bus.ModuleInvocation{StageID: StageRequestBudget, DeadlineNS: 1}, valid); status != bus.ModuleStatusCancelled {
		t.Fatal("expired policy admission accepted", status)
	}
}

func TestRequestBudgetStageCommitment(t *testing.T) {
	handler := NewHandler()
	for route := uint16(1); route <= 3; route++ {
		for _, size := range []uint64{0, 3, 4} {
			frame := budgetFrame(route, size, `{"schema_version":1,"max_request_bytes":3}`)
			out, status := handler(bus.ModuleInvocation{StageID: StageRequestBudget}, frame)
			if status != bus.ModuleStatusOK || len(out) != 44 {
				t.Fatalf("status %v response length %d", status, len(out))
			}
			want := RequestBudgetAdmitted
			if size > 3 {
				want = RequestBudgetOverflow
			}
			if binary.LittleEndian.Uint32(out) != requestBudgetMagic || binary.LittleEndian.Uint16(out[4:]) != auxWireVersion || binary.LittleEndian.Uint16(out[6:]) != want || binary.LittleEndian.Uint32(out[8:]) != 32 {
				t.Fatal("bad response envelope")
			}
			commitment := sha256.Sum256(frame)
			if !bytes.Equal(out[12:], commitment[:]) {
				t.Fatal("response not bound to request")
			}
			// Every metadata field, including same-length limits and body edits, changes the commitment.
			for _, offset := range []int{6, 8, 16, len(frame) - 2} {
				changed := bytes.Clone(frame)
				changed[offset] ^= 1
				hash := sha256.Sum256(changed)
				if bytes.Equal(out[12:], hash[:]) {
					t.Fatal("stale commitment accepted")
				}
			}
		}
	}
}

func TestRequestBudgetMalformedFrames(t *testing.T) {
	valid := budgetFrame(1, 3, `{"schema_version":1,"max_request_bytes":3}`)
	frames := [][]byte{nil, valid[:51], budgetFrame(0, 3, "{}"), budgetFrame(4, 3, "{}"), budgetFrame(1, 3, ""), budgetFrame(1, 3, strings.Repeat(" ", 1025)), append(bytes.Clone(valid), 0)}
	for _, offset := range []int{0, 4, 48} {
		bad := bytes.Clone(valid)
		bad[offset] ^= 1
		frames = append(frames, bad)
	}
	for _, frame := range frames {
		out, status := NewHandler()(bus.ModuleInvocation{StageID: StageRequestBudget}, frame)
		if status != bus.ModuleStatusInvalidRequest || out != nil {
			t.Fatalf("malformed frame admitted: %v", status)
		}
	}
}

func TestRequestBudgetCancelled(t *testing.T) {
	frame := budgetFrame(1, 3, `{"schema_version":1,"max_request_bytes":3}`)
	out, status := NewHandler()(bus.ModuleInvocation{StageID: StageRequestBudget, DeadlineNS: 1}, frame)
	if status != bus.ModuleStatusCancelled || out != nil {
		t.Fatalf("expired admission: %v", status)
	}
}

func BenchmarkRequestBudgetAdmission(b *testing.B) {
	frame := budgetFrame(1, 8192, `{"schema_version":1,"max_request_bytes":8192}`)
	handler := NewHandler()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if _, status := handler(bus.ModuleInvocation{StageID: StageRequestBudget}, frame); status != bus.ModuleStatusOK {
			b.Fatal(status)
		}
	}
}

func BenchmarkRequestBudgetOperatorAdmission(b *testing.B) {
	for _, caller := range []bool{false, true} {
		name, limits := "inherited", ""
		if caller {
			name, limits = "tightened", `{"schema_version":1,"max_request_bytes":8192}`
		}
		b.Run(name, func(b *testing.B) {
			frame := policyBudgetFrame(1, 8192, limits, `{"schema_version":1,"max_request_bytes":16384}`)
			handler := NewHandler()
			b.ReportAllocs()
			for i := 0; i < b.N; i++ {
				if _, status := handler(bus.ModuleInvocation{StageID: StageRequestBudget}, frame); status != bus.ModuleStatusOK {
					b.Fatal(status)
				}
			}
		})
	}
}
