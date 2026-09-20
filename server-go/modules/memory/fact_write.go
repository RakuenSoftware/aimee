package memory

import (
	"context"
	"encoding/json"
)

// FactWriteRequest preserves the gate's relation spelling and bounded wire
// contract. General data-operation relation normalization must not change it.
type FactWriteRequest struct {
	Head     NodeKind `json:"head"`
	Relation string   `json:"relation"`
	Tail     NodeKind `json:"tail"`
}

// FactWriteDecision keeps the ontology answer separate from commit eligibility:
// observe-only callers still need the former for a relation withheld from KB.
// The Go owner makes both decisions in one invocation, with no native PII
// classifier or second policy round trip between validation and commit.
type FactWriteDecision struct {
	Verdict       FactVerdict `json:"verdict"`
	CommitAllowed bool        `json:"commit_allowed"`
}

func (d *FactWriteDecision) UnmarshalJSON(body []byte) error {
	var fields struct {
		Verdict       *FactVerdict `json:"verdict"`
		CommitAllowed *bool        `json:"commit_allowed"`
	}
	if err := json.Unmarshal(body, &fields); err != nil {
		return err
	}
	if fields.Verdict == nil || fields.CommitAllowed == nil {
		return ErrClientResponse
	}
	d.Verdict, d.CommitAllowed = *fields.Verdict, *fields.CommitAllowed
	return nil
}

func DecideFactWrite(request FactWriteRequest) FactWriteDecision {
	verdict := GateCheck(request.Head, request.Relation, request.Tail)
	return FactWriteDecision{
		Verdict: verdict,
		CommitAllowed: (verdict == FactAccept || verdict == FactNovel) &&
			RelSensitivityOf(request.Relation) != SensSecret,
	}
}

func (c *Client) CheckFactWrite(ctx context.Context, trace uint64, request FactWriteRequest) (FactWriteDecision, error) {
	if len(request.Relation) > relTypeMax {
		return FactWriteDecision{}, ErrClientRequest
	}
	response, err := c.Data(ctx, trace, DataRequest{Operation: "fact-write-decision", FactWrite: &request})
	if err != nil {
		return FactWriteDecision{}, err
	}
	if response.FactWrite == nil || response.FactWrite.Verdict > FactBadArg ||
		(response.FactWrite.CommitAllowed && response.FactWrite.Verdict != FactAccept && response.FactWrite.Verdict != FactNovel) {
		return FactWriteDecision{}, ErrClientResponse
	}
	return *response.FactWrite, nil
}
