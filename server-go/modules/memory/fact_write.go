package memory

import (
	memorycontract "github.com/JBailes/aimee/server-go/memory"
)

type FactWriteRequest = memorycontract.FactWriteRequest
type FactWriteDecision = memorycontract.FactWriteDecision

func DecideFactWrite(request FactWriteRequest) FactWriteDecision {
	verdict := GateCheck(request.Head, request.Relation, request.Tail)
	return FactWriteDecision{
		Verdict: verdict,
		CommitAllowed: (verdict == FactAccept || verdict == FactNovel) &&
			RelSensitivityOf(request.Relation) != SensSecret,
	}
}
