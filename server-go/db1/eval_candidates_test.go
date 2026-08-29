package db1

import (
	"context"
	"encoding/binary"
	"testing"
	"time"
)

type evalCandidateCaller struct {
	event, stage uint32
	request      []byte
	response     []byte
}

func (c *evalCandidateCaller) Call(_ context.Context, event, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	c.event, c.stage, c.request = event, stage, request
	return c.response, nil
}

func fieldsReply(status uint32, fields ...string) []byte {
	var out []byte
	put := func(value uint32) {
		var raw [4]byte
		binary.LittleEndian.PutUint32(raw[:], value)
		out = append(out, raw[:]...)
	}
	put(status)
	put(uint32(len(fields)))
	for _, field := range fields {
		put(uint32(len(field)))
		out = append(out, field...)
	}
	return out
}

func TestEvalCandidateListUsesTelemetryFamilyAndDecodesRows(t *testing.T) {
	caller := &evalCandidateCaller{response: fieldsReply(statusOK,
		"7", "abc", "admitted", "regressions", "task", `{"name":"task"}`,
		"agent_job", "agent_job:9", "3", "3", "auto", "/tmp/task.json", "", "2",
		"2026-08-28 00:00:00", "2026-08-28 01:00:00")}
	client, err := NewClient(caller, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	rows, err := client.EvalCandidateList(context.Background(), "admitted", 8)
	if err != nil {
		t.Fatal(err)
	}
	if caller.event != eventTelemetry || caller.stage != stageTelemetry {
		t.Fatalf("called event=%d stage=%d", caller.event, caller.stage)
	}
	if len(rows) != 1 || rows[0].ID != 7 || rows[0].State != "admitted" || rows[0].DistinctSessions != 3 {
		t.Fatalf("rows=%+v", rows)
	}
	op, requestFields, err := DecodeFields(caller.request)
	if err != nil || op != opEvalCandidateList || len(requestFields) != 2 ||
		requestFields[0] != "admitted" || requestFields[1] != "8" {
		t.Fatalf("request op=%d fields=%v err=%v", op, requestFields, err)
	}
}
