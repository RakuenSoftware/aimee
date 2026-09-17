package memory

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestFactWriteDecisionCombinesOntologyAndSecretBoundary(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range []struct {
		relation   string
		head, tail NodeKind
		verdict    FactVerdict
		allowed    bool
	}{
		{"works_for", NodePerson, NodeOrg, FactAccept, true},
		{"works_for", NodeFile, NodeOrg, FactRejectKind, false},
		{"", NodePerson, NodeOther, FactBadArg, false},
		{"new_attribute", NodePerson, NodeOther, FactNovel, true},
		{"api_key", NodePerson, NodeOther, FactNovel, false},
		{"private_key", NodePerson, NodeOther, FactNovel, false},
	} {
		t.Run(test.relation, func(t *testing.T) {
			got, err := client.CheckFactWrite(context.Background(), 73, FactWriteRequest{test.head, test.relation, test.tail})
			if err != nil || got != (FactWriteDecision{test.verdict, test.allowed}) {
				t.Fatalf("decision=%+v err=%v", got, err)
			}
		})
	}
}

func TestFactWriteDecisionKeepsHistoricalGateAnswers(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, test := range gateMatrixCases(t) {
		if len(test.relType) > relTypeMax {
			continue
		}
		got, err := client.CheckFactWrite(context.Background(), 73, FactWriteRequest{test.head, test.relType, test.tail})
		if err != nil || got.Verdict != test.want {
			t.Fatalf("gate(%q)=%+v err=%v want=%v", test.relType, got, err, test.want)
		}
	}
}

func TestFactWriteRefusesMissingOrContradictoryOwnerDecision(t *testing.T) {
	for _, reply := range []string{
		`{"records":null}`, `{"records":null,"fact_write":null}`, `{"records":null,"fact_write":{"verdict":0}}`,
		`{"records":null,"fact_write":{"commit_allowed":true}}`,
		`{"records":null,"fact_write":{"verdict":9,"commit_allowed":false}}`,
		`{"records":null,"fact_write":{"verdict":1,"commit_allowed":true}}`,
	} {
		t.Run(reply, func(t *testing.T) {
			client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
				return []byte(reply), nil
			}), time.Second)
			if _, err := client.CheckFactWrite(context.Background(), 0, FactWriteRequest{NodePerson, "works_for", NodeOrg}); !errors.Is(err, ErrClientResponse) {
				t.Fatalf("accepted incomplete owner response: %v", err)
			}
		})
	}
}

func TestFactWriteFailureHasNoLocalAcceptance(t *testing.T) {
	for _, failure := range []error{context.DeadlineExceeded, context.Canceled, &bus.ModuleCallStatusError{Status: bus.ModuleStatusCapabilityAbsent}} {
		calls := 0
		client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
			calls++
			return nil, failure
		}), time.Second)
		got, err := client.CheckFactWrite(context.Background(), 0, FactWriteRequest{NodePerson, "works_for", NodeOrg})
		if !errors.Is(err, failure) || got.CommitAllowed || calls != 1 {
			t.Fatalf("decision=%+v err=%v calls=%d", got, err, calls)
		}
		_, err = client.CheckFactWrite(context.Background(), 0, FactWriteRequest{NodePerson, strings.Repeat("x", relTypeMax+1), NodeOther})
		if !errors.Is(err, ErrClientRequest) || calls != 1 {
			t.Fatalf("oversize request dispatched: %v calls=%d", err, calls)
		}
	}
}
