package ir

import (
	"strconv"
	"strings"
)

// DelegateLearningBinding is custody metadata written only by a host append.
// Schema validation does not grant that custody or assert task correctness.
type DelegateLearningBinding struct {
	Version   int    `json:"version"`
	JobID     string `json:"job_id"`
	ScopeKind string `json:"scope_kind"`
	ScopeID   string `json:"scope_id"`
	Agent     string `json:"agent"`
	Provider  string `json:"provider"`
	Model     string `json:"model"`
}
type ChildExecutionPayload struct {
	Session         string                   `json:"session_id"`
	Turn            string                   `json:"turn_id"`
	Role            string                   `json:"role"`
	Status          string                   `json:"status"`
	LearningBinding *DelegateLearningBinding `json:"learning_binding,omitempty"`
}

func ValidDelegateLearningBinding(b *DelegateLearningBinding) bool {
	if b == nil {
		return true
	}
	job, err := strconv.ParseInt(b.JobID, 10, 64)
	if err != nil || job <= 0 || job > 2147483647 || strconv.FormatInt(job, 10) != b.JobID || b.Version != 1 || b.ScopeKind != "project" {
		return false
	}
	for i, s := range []string{b.ScopeID, b.Agent, b.Provider, b.Model} {
		if s == "" || len(s) > []int{256, 63, 127, 255}[i] || strings.ContainsRune(s, 0) {
			return false
		}
	}
	return true
}
