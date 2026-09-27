package economizer

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
	"testing"
)

func taskCostPairFixture() TaskCostPairRequest {
	corpus := []TaskCostCorpusItem{{Task: "task", TaskDigest: strings.Repeat("1", 64), VerifierDigest: strings.Repeat("2", 64)}}
	raw, _ := json.Marshal(corpus)
	digest := sha256.Sum256(raw)
	q := 1000000
	arm := TaskCostArm{Cost: taskCostFixture(), Completion: "completed", QualityPPM: &q, QualityEvidence: "verifier-report:1"}
	return TaskCostPairRequest{SchemaVersion: 1, CorpusDigest: hex.EncodeToString(digest[:]), Corpus: corpus, Baseline: []TaskCostArm{arm}, Candidate: []TaskCostArm{arm}}
}
func TestTaskCostPairsPreserveCoverageAndRefuseCherryPicking(t *testing.T) {
	r := taskCostPairFixture()
	out, err := PairTaskCosts(r)
	if err != nil || out.Pairs != 1 || out.IncompleteCosts != 1 || out.KnownDifference != "0" || !strings.HasPrefix(out.Gate, "unqualified") {
		t.Fatalf("%+v %v", out, err)
	}
	r.Candidate[0].Completion = "failed"
	q := 200000
	r.Candidate[0].QualityPPM = &q
	out, err = PairTaskCosts(r)
	if err != nil || out.QualityRegressions != 1 {
		t.Fatal(out, err)
	}
	raw, _ := json.Marshal(r)
	if _, status := NewHandler()(bus.ModuleInvocation{StageID: StageTaskCost}, raw); status != bus.ModuleStatusOK {
		t.Fatal("paired wire unavailable", status)
	}
	for _, name := range []string{"missing", "wrong task", "changed corpus", "duplicate"} {
		t.Run(name, func(t *testing.T) {
			r := taskCostPairFixture()
			switch name {
			case "missing":
				r.Candidate = nil
			case "wrong task":
				r.Candidate[0].Cost.Task = "other"
			case "changed corpus":
				r.Corpus[0].TaskDigest = strings.Repeat("3", 64)
			case "duplicate":
				r.Candidate = append(r.Candidate, r.Candidate[0])
			}
			if _, err := PairTaskCosts(r); err == nil {
				t.Fatal("accepted cherry-picked population")
			}
		})
	}
}
