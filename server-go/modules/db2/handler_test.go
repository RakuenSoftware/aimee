package db2

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

type wireBaseline struct {
	Operations []struct {
		Name    string `json:"name"`
		Request struct {
			Positive string `json:"positive"`
			Negative []struct {
				Hex      string `json:"hex"`
				Mutation string `json:"mutation"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []struct {
				Flags uint32 `json:"flags"`
				Hex   string `json:"hex"`
			} `json:"positive"`
		} `json:"reply"`
	} `json:"operations"`
}

type fakeHealthRow struct {
	values db2contract.HealthEvidence
	err    error
}

func (row fakeHealthRow) Scan(dest ...any) error {
	if row.err != nil {
		return row.err
	}
	if len(dest) != 3 {
		return errors.New("unexpected scan width")
	}
	*dest[0].(*bool) = row.values.SchemaOK
	*dest[1].(*bool) = row.values.HavePGTrgm
	*dest[2].(*bool) = row.values.KBTablesOK
	return nil
}

func loadHealthBaseline(t *testing.T) wireBaseline {
	t.Helper()
	path := filepath.Join("..", "..", "..", "tests", "baselines", "modules", "db2-wire-v1.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var baseline wireBaseline
	if err := json.Unmarshal(raw, &baseline); err != nil {
		t.Fatalf("decode %s: %v", path, err)
	}
	// The nine this test replays, in order, at the FRONT of the baseline. It used
	// to demand the baseline hold exactly nine, which made every later operation
	// added to db2 fail here -- a test for the health wire refusing to run because
	// something unrelated was appended alongside it.
	if len(baseline.Operations) < 9 || baseline.Operations[0].Name != "health" ||
		baseline.Operations[1].Name != "embedding_dimension" ||
		baseline.Operations[2].Name != "pool_status" ||
		baseline.Operations[3].Name != "embedding_refusals" ||
		baseline.Operations[4].Name != "postgres_status" ||
		baseline.Operations[5].Name != "reembed_status" ||
		baseline.Operations[6].Name != "reembed_clear" ||
		baseline.Operations[7].Name != "reembed_clear_maintenance" ||
		baseline.Operations[8].Name != "embedder_serving_id" {
		t.Fatalf("unexpected operation baseline: %+v", baseline.Operations)
	}
	return baseline
}

func decodeHex(t *testing.T, value string) []byte {
	t.Helper()
	decoded, err := hex.DecodeString(value)
	if err != nil {
		t.Fatalf("decode %q: %v", value, err)
	}
	return decoded
}

func evidenceForFlags(flags uint32) db2contract.HealthEvidence {
	return db2contract.HealthEvidence{
		SchemaOK:   flags&db2contract.HealthFlagSchema != 0,
		HavePGTrgm: flags&db2contract.HealthFlagPGTrgm != 0,
		KBTablesOK: flags&db2contract.HealthFlagKBTables != 0,
	}
}

func TestHealthHandlerReplaysSharedCBaseline(t *testing.T) {
	operation := loadHealthBaseline(t).Operations[0]
	request := decodeHex(t, operation.Request.Positive)
	for _, vector := range operation.Reply.Positive {
		vector := vector
		t.Run(vector.Hex, func(t *testing.T) {
			calls := 0
			handler := NewHandler(func(ctx context.Context, query string, args ...any) HealthRow {
				calls++
				if ctx == nil || query != healthQuery || len(args) != 0 {
					t.Fatalf("query call = (%v, %q, %v)", ctx, query, args)
				}
				if deadline, ok := ctx.Deadline(); !ok || time.Until(deadline) <= 0 ||
					time.Until(deadline) > healthProbeTimeout {
					t.Fatalf("probe deadline = %v, ok=%v", deadline, ok)
				}
				return fakeHealthRow{values: evidenceForFlags(vector.Flags)}
			})
			response, status := handler(
				bus.ModuleInvocation{StageID: db2contract.StageHealth}, request,
			)
			if status != bus.ModuleStatusOK || calls != 1 {
				t.Fatalf("status/calls = (%v, %d)", status, calls)
			}
			want := decodeHex(t, vector.Hex)
			if string(response) != string(want) {
				t.Fatalf("response = %x, want %x", response, want)
			}
		})
	}
}

func TestHealthHandlerRejectsEverySharedMalformedRequest(t *testing.T) {
	operation := loadHealthBaseline(t).Operations[0]
	for _, vector := range operation.Request.Negative {
		vector := vector
		t.Run(vector.Mutation, func(t *testing.T) {
			calls := 0
			handler := NewHandler(func(context.Context, string, ...any) HealthRow {
				calls++
				return fakeHealthRow{}
			})
			response, status := handler(
				bus.ModuleInvocation{StageID: db2contract.StageHealth}, decodeHex(t, vector.Hex),
			)
			if status != bus.ModuleStatusInvalidRequest || response != nil || calls != 0 {
				t.Fatalf("result = (%x, %v, calls=%d)", response, status, calls)
			}
		})
	}
}

func TestHealthHandlerRejectsWrongStageAndMissingProvider(t *testing.T) {
	request := db2contract.EncodeHealthRequest()
	response, status := NewHandler(nil)(
		bus.ModuleInvocation{StageID: db2contract.StageHealth + 1}, request,
	)
	if response != nil || status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong stage = (%x, %v)", response, status)
	}
	response, status = NewHandler(nil)(
		bus.ModuleInvocation{StageID: db2contract.StageHealth}, request,
	)
	if response != nil || status != bus.ModuleStatusCapabilityAbsent {
		t.Fatalf("missing provider = (%x, %v)", response, status)
	}
}

func TestHealthHandlerHonorsExpiredDeadlineBeforeQuery(t *testing.T) {
	calls := 0
	handler := NewHandler(func(context.Context, string, ...any) HealthRow {
		calls++
		return fakeHealthRow{}
	})
	response, status := handler(
		bus.ModuleInvocation{StageID: db2contract.StageHealth, DeadlineNS: 1},
		db2contract.EncodeHealthRequest(),
	)
	if response != nil || status != bus.ModuleStatusCancelled || calls != 0 {
		t.Fatalf("expired = (%x, %v, calls=%d)", response, status, calls)
	}
}

func TestHealthHandlerContainsQueryFailures(t *testing.T) {
	driverDetail := "password=secret SQLSTATE 28P01"
	handler := NewHandler(func(context.Context, string, ...any) HealthRow {
		return fakeHealthRow{err: errors.New(driverDetail)}
	})
	response, status := handler(
		bus.ModuleInvocation{StageID: db2contract.StageHealth}, db2contract.EncodeHealthRequest(),
	)
	if response != nil || status != bus.ModuleStatusInternal {
		t.Fatalf("query failure = (%x, %v)", response, status)
	}
	if strings.Contains(string(response), driverDetail) || strings.Contains(string(response), "28P01") {
		t.Fatalf("driver detail leaked in response %q", response)
	}

	response, status = NewHandler(func(context.Context, string, ...any) HealthRow {
		return nil
	})(bus.ModuleInvocation{StageID: db2contract.StageHealth}, db2contract.EncodeHealthRequest())
	if response != nil || status != bus.ModuleStatusInternal {
		t.Fatalf("nil row = (%x, %v)", response, status)
	}
}

func TestHealthQueryRetainsDB2OwnedReadinessEvidence(t *testing.T) {
	for _, required := range []string{"memories", "pg_trgm", "kb_documents", "kb_async_jobs"} {
		if !strings.Contains(healthQuery, required) {
			t.Fatalf("health query omits %q", required)
		}
	}
}
