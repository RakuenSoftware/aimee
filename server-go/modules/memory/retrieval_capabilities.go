package memory

import (
	"context"
	"sync"
)

// Request-local execution evidence. A declared arm is not proof of a ready
// index, complete coverage, or a successful execution. Mixed nested calls retain
// separate observations instead of letting the last call hide a fallback.
type retrievalArmObservation struct {
	State          string `json:"state"`
	Reason         string `json:"reason"`
	Candidates     int    `json:"candidates"`
	Quota          int    `json:"candidate_quota"`
	IndexReadiness string `json:"index_readiness"`
	IndexVersion   string `json:"index_version,omitempty"`
	Calls          int    `json:"calls"`
}
type retrievalCapabilities struct {
	mu            sync.Mutex
	PolicyDigest  string                               `json:"policy_digest"`
	SchemaVersion int                                  `json:"schema_version"`
	Endpoint      string                               `json:"endpoint"`
	Placement     Placement                            `json:"placement"`
	TemporalMode  string                               `json:"temporal_mode"`
	FusionPolicy  string                               `json:"fusion_policy"`
	Fallback      string                               `json:"fallback"`
	DeclaredArms  map[string]bool                      `json:"declared_arms"`
	Arms          map[string][]retrievalArmObservation `json:"arm_observations"`
	Truncated     bool                                 `json:"observations_truncated"`
}
type retrievalCapabilitiesKey struct{}

func withRetrievalCapabilities(ctx context.Context, placement Placement, req DataRequest) context.Context {
	switch req.Operation {
	case "search", "recall", "visible-search", "adaptive-search", "server-search", "recall-bundle", "assemble-context", "context-block", "context-ingress", "diagnose", "ask", "assertion-search", "typed-context", "hybrid-context":
	default:
		return ctx
	}
	mode := "current"
	if req.Assertions != nil && (req.Assertions.Historical || req.Assertions.ValidAt != "" || req.Assertions.BelievedAt != "") {
		mode = "explicit_temporal_assertions"
	}
	c := &retrievalCapabilities{PolicyDigest: rankingArtifactDigest(false), SchemaVersion: 1, Endpoint: req.Operation, Placement: placement, TemporalMode: mode, FusionPolicy: "staged-rrf60-independent-pools-v1", Fallback: "eligible_available_arms_only; no_cross_placement_fallback", DeclaredArms: map[string]bool{"lexical": true, "dense": true, "graph": placement == PlacementKB, "code": placement == PlacementKB && req.Operation != "assertion-search" && req.Operation != "typed-context"}, Arms: map[string][]retrievalArmObservation{}}
	if req.Operation == "assertion-search" || req.Operation == "typed-context" {
		c.FusionPolicy = "assertion_rrf_k60_v1"
	}
	if req.Operation == "recall-bundle" {
		c.DeclaredArms["code"] = false
		if placement == PlacementKB {
			c.DeclaredArms["dense"] = false
		}
	}
	for arm, supported := range c.DeclaredArms {
		state, reason := "not_executed", "endpoint_did_not_execute_arm"
		if !supported {
			state, reason = "unsupported", "placement_or_endpoint_has_no_arm"
		}
		c.Arms[arm] = []retrievalArmObservation{{State: state, Reason: reason, IndexReadiness: "unknown"}}
	}
	return context.WithValue(ctx, retrievalCapabilitiesKey{}, c)
}
func recordRetrievalArm(ctx context.Context, arm string, observation retrievalArmObservation) {
	c, _ := ctx.Value(retrievalCapabilitiesKey{}).(*retrievalCapabilities)
	if c == nil {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	prior, ok := c.Arms[arm]
	if !ok {
		return
	}
	if len(prior) == 1 && prior[0].Calls == 0 {
		prior = nil
	}
	observation.Calls = 1
	if observation.IndexReadiness == "" {
		observation.IndexReadiness = "unknown"
	}
	for i, old := range prior {
		old.Calls = 1
		if old == observation {
			prior[i].Calls++
			c.Arms[arm] = prior
			return
		}
	}
	if len(prior) >= 8 {
		c.Truncated = true
		return
	}
	c.Arms[arm] = append(prior, observation)
}
func observedRetrievalCapabilities(ctx context.Context) *retrievalCapabilities {
	c, _ := ctx.Value(retrievalCapabilitiesKey{}).(*retrievalCapabilities)
	if c == nil {
		return nil
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	out := &retrievalCapabilities{PolicyDigest: c.PolicyDigest, SchemaVersion: c.SchemaVersion, Endpoint: c.Endpoint, Placement: c.Placement, TemporalMode: c.TemporalMode, FusionPolicy: c.FusionPolicy, Fallback: c.Fallback, Truncated: c.Truncated, DeclaredArms: map[string]bool{}, Arms: map[string][]retrievalArmObservation{}}
	for k, v := range c.DeclaredArms {
		out.DeclaredArms[k] = v
	}
	for k, v := range c.Arms {
		out.Arms[k] = append([]retrievalArmObservation(nil), v...)
	}
	return out
}
