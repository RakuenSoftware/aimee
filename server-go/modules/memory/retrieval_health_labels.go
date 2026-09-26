package memory

import (
	"errors"
	"strings"
)

// Evaluations are produced by an owner/verifier at final selection, never by a
// health API caller. An absent label is not a negative label. Candidate and
// delivery populations stay separate, and fusion names its baseline arm.
type healthCandidateLabel struct {
	ID         string `json:"id"`
	Arm        string `json:"arm"`
	Population string `json:"population"`
	Invalid    *bool  `json:"invalid"`
}
type healthRequirementLabel struct {
	ID                   string `json:"id"`
	BaselineArm          string `json:"baseline_arm"`
	BaselineSatisfied    *bool  `json:"baseline_satisfied"`
	FinalSatisfied       *bool  `json:"final_satisfied"`
	SoleSupportDisplaced *bool  `json:"sole_support_displaced"`
}
type healthSelectionLabels struct {
	Verifier     string                   `json:"verifier"`
	Candidates   []healthCandidateLabel   `json:"candidates"`
	Requirements []healthRequirementLabel `json:"requirements"`
}
type healthLabelMetrics struct {
	UnlabeledInvocations int                    `json:"unlabeled_invocations"`
	Contamination        map[string]healthRatio `json:"arm_contamination"`
	Recovery             map[string]healthRatio `json:"fusion_recovery_by_baseline_arm"`
	Displacement         healthRatio            `json:"sole_support_displacement"`
}

func healthLabelName(name string) bool {
	if name == "" || len(name) > 64 {
		return false
	}
	for _, c := range name {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}
func (r *healthLabelMetrics) add(labels *healthSelectionLabels) error {
	if labels == nil {
		r.UnlabeledInvocations++
		return nil
	}
	if strings.TrimSpace(labels.Verifier) == "" || len(labels.Verifier) > 128 || len(labels.Candidates) > 256 || len(labels.Requirements) > 16 {
		return errors.New("unbound or excessive selection labels")
	}
	if r.Contamination == nil {
		r.Contamination = map[string]healthRatio{}
	}
	if r.Recovery == nil {
		r.Recovery = map[string]healthRatio{}
	}
	candidates := map[[3]string]string{}
	for _, candidate := range labels.Candidates {
		if candidate.ID == "" || len(candidate.ID) > 256 || !healthLabelName(candidate.Arm) || candidate.Population != "candidates" && candidate.Population != "deliveries" {
			return errors.New("invalid arm label population")
		}
		key := [3]string{candidate.Population, candidate.Arm, candidate.ID}
		digest := releaseDigest(candidate)
		if old, exists := candidates[key]; exists {
			if old != digest {
				return errors.New("conflicting candidate labels")
			}
			continue
		}
		candidates[key] = digest
		group := candidate.Population + ":" + candidate.Arm
		ratio := r.Contamination[group]
		if candidate.Invalid == nil {
			ratio.Unknown++
		} else {
			ratio.Denominator++
			if *candidate.Invalid {
				ratio.Numerator++
			}
		}
		r.Contamination[group] = ratio
	}
	requirements := map[string]string{}
	for _, requirement := range labels.Requirements {
		if requirement.ID == "" || len(requirement.ID) > 256 || !healthLabelName(requirement.BaselineArm) {
			return errors.New("invalid requirement baseline")
		}
		digest := releaseDigest(requirement)
		if old, exists := requirements[requirement.ID]; exists {
			if old != digest {
				return errors.New("conflicting requirement labels")
			}
			continue
		}
		requirements[requirement.ID] = digest
		if requirement.SoleSupportDisplaced == nil {
			r.Displacement.Unknown++
		} else {
			r.Displacement.Denominator++
			if *requirement.SoleSupportDisplaced {
				r.Displacement.Numerator++
			}
		}
		ratio := r.Recovery[requirement.BaselineArm]
		if requirement.BaselineSatisfied == nil {
			ratio.Unknown++
		} else if !*requirement.BaselineSatisfied {
			if requirement.FinalSatisfied == nil {
				ratio.Unknown++
			} else {
				ratio.Denominator++
				if *requirement.FinalSatisfied {
					ratio.Numerator++
				}
			}
		}
		r.Recovery[requirement.BaselineArm] = ratio
	}
	return nil
}
func (r *healthLabelMetrics) finish() {
	r.Displacement.finish()
	for key, ratio := range r.Contamination {
		ratio.finish()
		r.Contamination[key] = ratio
	}
	for key, ratio := range r.Recovery {
		ratio.finish()
		r.Recovery[key] = ratio
	}
}

// The final source commitment prevents labels for a pre-fusion or pre-trimming
// candidate set from being advertised as labels for a different delivered set.
type healthLabelsEnvelope struct {
	SourcesDigest string                 `json:"sources_digest"`
	Labels        *healthSelectionLabels `json:"labels"`
}
