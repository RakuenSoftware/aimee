package aimee

import (
	"context"
	"fmt"
)

const (
	eventTelemetry       uint32 = 11784
	stageTelemetry       uint32 = 8
	opEvalCandidateList  uint32 = 51
	evalCandidateWidth          = 16
	evalCandidateMaxRows        = 128
)

// EvalCandidate is the telemetry family's admitted-regression ledger row. The
// workflow selector needs provenance and the frozen task bytes; retaining the
// complete row here keeps its decoder aligned with the catalog's 16-cell reply.
type EvalCandidate struct {
	ID               int64
	Signature        string
	State            string
	Suite            string
	TaskName         string
	TaskJSON         string
	Origin           string
	OriginRef        string
	Occurrences      int
	DistinctSessions int
	AdmittedBy       string
	AdmittedPath     string
	RejectReason     string
	PassingWindows   int
	CreatedAt        string
	UpdatedAt        string
}

func (c *Client) EvalCandidateList(ctx context.Context, state string, max int) ([]EvalCandidate, error) {
	if max <= 0 || max > evalCandidateMaxRows {
		return nil, fmt.Errorf("eval candidate limit must be between 1 and %d", evalCandidateMaxRows)
	}
	status, fields, err := c.callFieldsAt(ctx, eventTelemetry, stageTelemetry,
		opEvalCandidateList, []string{state, Itoa(max)})
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "eval_candidate_list", Status: status}
	}
	rows, err := Rows(fields, evalCandidateWidth)
	if err != nil {
		return nil, err
	}
	out := make([]EvalCandidate, 0, len(rows))
	for _, row := range rows {
		id, err := Atoi64(row[0])
		if err != nil {
			return nil, err
		}
		occurrences, err := Atoi(row[8])
		if err != nil {
			return nil, err
		}
		distinct, err := Atoi(row[9])
		if err != nil {
			return nil, err
		}
		windows, err := Atoi(row[13])
		if err != nil {
			return nil, err
		}
		out = append(out, EvalCandidate{
			ID: id, Signature: row[1], State: row[2], Suite: row[3], TaskName: row[4],
			TaskJSON: row[5], Origin: row[6], OriginRef: row[7], Occurrences: occurrences,
			DistinctSessions: distinct, AdmittedBy: row[10], AdmittedPath: row[11],
			RejectReason: row[12], PassingWindows: windows, CreatedAt: row[14], UpdatedAt: row[15],
		})
	}
	return out, nil
}
