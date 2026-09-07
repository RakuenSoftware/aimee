package db2

import (
	"context"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
)

// The packaged C replay prepares the real DB2 schema. These migrated memory
// operations run through the Go wire handler in a rolled-back transaction.
func TestMemoryPostgresReplay(t *testing.T) {
	url := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL after the packaged DB2 replay")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	backend := NewPGMemoryBackend(MemorySeams{
		QueryRow: func(ctx context.Context, sql string, args ...any) HealthRow { return tx.QueryRow(ctx, sql, args...) },
		Query:    func(ctx context.Context, sql string, args ...any) (Rows, error) { return tx.Query(ctx, sql, args...) },
		Exec: func(ctx context.Context, sql string, args ...any) (int64, error) {
			tag, err := tx.Exec(ctx, sql, args...)
			return tag.RowsAffected(), err
		},
	})
	handler := NewMemoryHandler(backend)
	call := func(request []byte) []byte {
		t.Helper()
		reply, status := handler(memoryInvocation(), request)
		if status != bus.ModuleStatusOK {
			t.Fatalf("request %x: status %v", request, status)
		}
		return reply
	}
	encode := func(request []byte, err error) []byte {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		return request
	}
	for _, request := range [][]byte{
		contract.EncodeLevel3CountRequest(), contract.EncodeLevel2CountRequest(),
		contract.EncodeOrphanedL0CountRequest(), contract.EncodeTotalCountRequest(),
		encode(contract.EncodeSessionL2CountRequest("fresh-session-with-no-l2")),
		encode(contract.EncodeKeyExistsRequest("fresh-key-with-no-row")),
		encode(contract.EncodeFindIDByKeyKindRequest("fresh-key-with-no-row", "task")),
		encode(contract.EncodeKeyExistsInTierPairRequest("fresh-key-with-no-row", "L3", "L4")),
		encode(contract.EncodeEffectivenessUpdateRequest(42, 1, 0.75)),
		encode(contract.EncodeEffectivenessUpdateRequest(42, 0, 0)),
		contract.EncodeRetentionEnforceRequest(), contract.EncodeEffectivenessDemoteRequest(),
		contract.EncodeEffectivenessStatsRequest(), contract.EncodeL2MemoryIDsRequest(),
		contract.EncodeHealthRetentionRequest(), contract.EncodeStatsCountsRequest(),
		contract.EncodeExpireRequest(), contract.EncodeDemoteRequest(),
		contract.EncodePromoteStableRequest(), encode(contract.EncodeReclassifyDirectivesRequest(1)),
	} {
		call(request)
	}
	before, err := contract.DecodeHealthCountersReply(call(contract.EncodeHealthCountersRequest()))
	if err != nil {
		t.Fatal(err)
	}
	call(encode(contract.EncodeHealthRecordRequest(4, 2, 9)))
	after, err := contract.DecodeHealthCountersReply(call(contract.EncodeHealthCountersRequest()))
	if err != nil {
		t.Fatal(err)
	}
	if after.Cycles != before.Cycles+1 || after.TotalPromotions != before.TotalPromotions+4 ||
		after.TotalDemotions != before.TotalDemotions+2 || after.TotalExpirations != before.TotalExpirations+9 {
		t.Fatalf("health write/read round trip: before=%+v after=%+v", before, after)
	}
	request := encode(contract.EncodeRecordL4ApprovalRequest(9223372036854775807, "operator", "reviewed"))
	if _, status := handler(memoryInvocation(), request); status != bus.ModuleStatusInternal {
		t.Fatalf("missing memory approval must report backend failure, got %v", status)
	}
}
