package memory

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
)

type retrievalPolicyArtifact struct {
	SchemaVersion       int             `json:"schema_version"`
	Version             string          `json:"version"`
	Fusion              string          `json:"fusion"`
	NativePrior         rankPriorPolicy `json:"native_prior"`
	AssertionPrior      string          `json:"assertion_prior"`
	AssertionJointBound float64         `json:"assertion_joint_bound"`
	GraphPrior          string          `json:"graph_prior"`
	GraphJointBound     float64         `json:"graph_joint_bound"`
	Selection           string          `json:"selection"`
	Routing             string          `json:"routing"`
	Exposure            string          `json:"exposure"`
}

func rankingArtifact(selection bool) retrievalPolicyArtifact {
	artifact := retrievalPolicyArtifact{SchemaVersion: 1, Version: "fair-hybrid-bounded-priors-v1", Fusion: "independent-eligible-pools-staged-rrf60", NativePrior: nativeRankingPriorPolicy(), AssertionPrior: assertionLexicalPriorPolicy, AssertionJointBound: assertionLexicalPriorBound, GraphPrior: graphRankingPriorPolicy, GraphJointBound: graphRankingPriorBound, Selection: "baseline", Routing: "fixed_declared_arms_no_learned_early_stop", Exposure: "disabled"}
	if selection {
		artifact.Version = "fair-hybrid-diversity-v2"
		artifact.Selection = typedSelectionPolicyVersion
	}
	return artifact
}
func rankingArtifactDigest(selection bool) string {
	raw, _ := json.Marshal(rankingArtifact(selection))
	return fmt.Sprintf("sha256:%x", sha256.Sum256(raw))
}
