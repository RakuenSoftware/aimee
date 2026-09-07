package memory

import "sync/atomic"

var graphFusionEnabled atomic.Bool

func setGraphFusion(state string) bool {
	enabled := state == "on"
	graphFusionEnabled.Store(enabled)
	return enabled
}

func clearGraphFusion() { graphFusionEnabled.Store(false) }

func isGraphFusionEnabled() bool { return graphFusionEnabled.Load() }
