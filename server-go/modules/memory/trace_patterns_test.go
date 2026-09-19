package memory

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestTracePatterns(t *testing.T) {
	rows := []traceObservation{
		{1, "Read", "No such file"}, {1, "Read", "error"}, {1, "Read", "ok"}, {1, "Read", "FAILED"}, {1, "Search", "found"},
		{2, "Read", "ok"}, {2, "Search", "ok"},
		{3, "Read", "ok"}, {3, "Search", "ok"},
	}
	got, err := tracePatterns(rows)
	if err != nil {
		t.Fatal(err)
	}
	want := []tracePattern{
		{"anti-pattern", "Retry loop: Read called 4 times with 3 errors", "Tool 'Read' was called 4 consecutive times with 3 failures. Consider a different approach after 2 failures.", .7},
		{"procedure", "recovery:Read->Search", "When 'Read' fails, try 'Search' as a recovery step.", .7},
		{"procedure", "sequence:Read->Search", "Common pattern: 'Read' is typically followed by 'Search' (observed in 3/3 plans, 100%).", .8},
	}
	if len(got) != len(want) {
		t.Fatal(got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("%+v != %+v", got[i], want[i])
		}
	}
	for _, s := range []string{"error", "Error", "ERROR", "failed", "Failed", "FAILED", "No such file", "not found", "Permission denied", "command not found"} {
		if !traceResultError(s) {
			t.Fatal(s)
		}
	}
	for _, s := range []string{"", "ok", "ErRoR", "failure"} {
		if traceResultError(s) {
			t.Fatal(s)
		}
	}
}

func TestTracePatternBoundaries(t *testing.T) {
	cases := []struct {
		name  string
		rows  []traceObservation
		count int
	}{
		{"below retry threshold", []traceObservation{{1, "Read", "error"}, {1, "Read", "error"}}, 0},
		{"one failure", []traceObservation{{1, "Read", "error"}, {1, "Read", "ok"}, {1, "Read", "ok"}}, 0},
		{"plan boundary", []traceObservation{{1, "Read", "error"}, {1, "Read", "error"}, {2, "Read", "error"}, {3, "Search", "ok"}}, 0},
		{"same tool is not recovery", []traceObservation{{1, "Read", "error"}, {1, "Read", "ok"}}, 0},
		{"empty tool interrupts run", []traceObservation{{1, "Read", "error"}, {1, "", ""}, {1, "Read", "error"}}, 0},
		{"duplicate recovery", []traceObservation{{1, "Read", "error"}, {1, "Search", "ok"}, {2, "Read", "error"}, {2, "Search", "ok"}}, 1},
		{"plan zero legacy recovery", []traceObservation{{0, "Read", "error"}, {0, "Search", "ok"}}, 1},
		{"two of three plans", []traceObservation{{1, "Read", "ok"}, {1, "Search", "ok"}, {2, "Read", "ok"}, {2, "Search", "ok"}, {3, "Other", "ok"}}, 1},
		{"repeated pair is one plan", []traceObservation{{1, "Read", "ok"}, {1, "Search", "ok"}, {1, "Read", "ok"}, {1, "Search", "ok"}, {2, "Other", "ok"}, {3, "Other", "ok"}}, 0},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := tracePatterns(tc.rows)
			if err != nil || len(got) != tc.count {
				t.Fatal(got, err)
			}
		})
	}
	// Unlike the native detector's 128-pair scratch array, later pairs are not
	// silently dropped. Only sufficiently supported pairs become findings.
	rows := []traceObservation{}
	for plan := int64(1); plan <= 3; plan++ {
		for i := 0; i < 140; i++ {
			rows = append(rows, traceObservation{plan, string(rune(0x400 + i)), "ok"})
		}
	}
	got, err := tracePatterns(rows)
	if err != nil || len(got) != 139 {
		t.Fatal(len(got), err)
	}
}

func TestTracePatternHostBoundary(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		for _, input := range []string{`{}`, `{"traces":null}`, `{"traces":{}}`, `{"traces":[{"plan_id":-1}]}`, `{"traces":[{"plan_id":1.5}]}`, `{"traces":[{"tool_name":"bad\u0000name"}]}`} {
			args := commandArgs{}
			json.Unmarshal([]byte(input), &args)
			encoded, status := handleTracePatterns(args)
			if status != bus.ModuleStatusOK || !strings.Contains(string(encoded), `invalid_argument`) {
				t.Fatal(input, string(encoded), status)
			}
		}
		result := runHostRuntime(t, handler, `{"operation":"trace-patterns","traces":[]}`)
		if result["status"] != "ok" || len(result["patterns"].([]any)) != 0 {
			t.Fatal(result)
		}
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"trace-patterns","traces":[]}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 9}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("peer admitted", status)
		}
		// Full tool names/results survive Go transport, including error evidence
		// beyond the old native result buffer. No arguments or raw results return.
		tool := strings.Repeat("界", 100)
		rows := []traceObservation{{9007199254740993, tool, strings.Repeat("ok ", 2000) + "error"}, {9007199254740993, "Search", "ok"}}
		raw, _ := json.Marshal(map[string]any{"operation": "trace-patterns", "traces": rows})
		result = runHostRuntime(t, handler, string(raw))
		if result["status"] != "ok" || len(result["patterns"].([]any)) != 1 {
			t.Fatal(result)
		}
		pattern := result["patterns"].([]any)[0].(map[string]any)
		if pattern["key"] != "recovery:"+tool+"->Search" {
			t.Fatal(pattern)
		}
	}
}

func TestTracePatternCapacityAndRefusal(t *testing.T) {
	for _, rows := range [][]traceObservation{
		make([]traceObservation, 513),
		{{1, strings.Repeat("x", 1025), ""}},
		{{1, "Read", strings.Repeat("x", 1<<20)}},
	} {
		raw, _ := json.Marshal(rows)
		encoded, status := handleTracePatterns(commandArgs{"traces": raw})
		if status != bus.ModuleStatusOK || !strings.Contains(string(encoded), "invalid_argument") || strings.Contains(string(encoded), `"patterns"`) {
			t.Fatal(string(encoded), status)
		}
	}
	// Adjacent int64 plan IDs must not collapse to one JSON float identity.
	rows := []traceObservation{{9007199254740992, "Read", "error"}, {9007199254740993, "Search", "ok"}}
	got, err := tracePatterns(rows)
	if err != nil || len(got) != 0 {
		t.Fatal(got, err)
	}
	// A refused later finding must not return earlier acceptable findings.
	rows = []traceObservation{{1, "Read", "error"}, {1, "Search", "ok"}, {2, "token=abc", "error"}, {2, "Search", "ok"}}
	raw, _ := json.Marshal(rows)
	encoded, _ := handleTracePatterns(commandArgs{"traces": raw})
	if !strings.Contains(string(encoded), "invalid_argument") || strings.Contains(string(encoded), `"patterns"`) {
		t.Fatal(string(encoded))
	}
}
