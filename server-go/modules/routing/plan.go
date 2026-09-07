package routing

import (
	"encoding/json"
	"math"
	"sort"

	"github.com/JBailes/aimee/server-go/bus"
)

const EventPlan uint32 = 6402
const StagePlan uint32 = 2

// Prices are marginal USD per million tokens. nil means unknown; a pointer to
// zero means explicitly free. Overrides apply independently to each axis.
type Prices struct {
	Input  *float64 `json:"input"`
	Output *float64 `json:"output"`
}

type PriceBand struct {
	Above int `json:"above"`
	Prices
}

type Candidate struct {
	Name       string      `json:"name"`
	Tier       int         `json:"tier"`
	Competence int         `json:"competence"`
	Prices     Prices      `json:"prices"`
	Overrides  Prices      `json:"overrides"`
	Bands      []PriceBand `json:"bands"`
	Truncated  bool        `json:"truncated"`
}

type PlanRequest struct {
	Version      int         `json:"version"`
	InputTokens  int         `json:"input_tokens"`
	OutputTokens int         `json:"output_tokens"`
	Premium      bool        `json:"premium"`
	Candidates   []Candidate `json:"candidates"`
}

// Estimate deliberately assumes uncached input: cache residency is not known
// at routing time. Pricing bands apply strictly above their context threshold.
func Estimate(c Candidate, input, output int) (float64, bool) {
	if input < 0 || output < 0 {
		return 0, false
	}
	p := c.Prices
	if c.Truncated {
		p = Prices{}
	} else {
		above := -1
		for _, b := range c.Bands {
			if b.Above >= 0 && input > b.Above && b.Above > above {
				p, above = b.Prices, b.Above
			}
		}
	}
	if c.Overrides.Input != nil {
		p.Input = c.Overrides.Input
	}
	if c.Overrides.Output != nil {
		p.Output = c.Overrides.Output
	}
	if p.Input == nil || p.Output == nil {
		return 0, false
	}
	for _, v := range []float64{*p.Input, *p.Output} {
		if v < 0 || math.IsNaN(v) || math.IsInf(v, 0) {
			return 0, false
		}
	}
	cost := (float64(input)*(*p.Input) + float64(output)*(*p.Output)) / 1e6
	return cost, !math.IsNaN(cost) && !math.IsInf(cost, 0)
}

// Plan receives an already qualified pool. The premium learning arm maximizes
// demonstrated competence, never price. Both arms minimize cost at equal
// competence; unknown prices cannot masquerade as free. With no known prices,
// the configured tier is the compatibility fallback. Names break ties stably.
func Plan(req PlanRequest) int {
	if len(req.Candidates) == 0 {
		return -1
	}
	order := make([]int, len(req.Candidates))
	costs, known := make([]float64, len(order)), make([]bool, len(order))
	for i, c := range req.Candidates {
		order[i] = i
		costs[i], known[i] = Estimate(c, req.InputTokens, req.OutputTokens)
	}
	sort.SliceStable(order, func(a, b int) bool {
		i, j := order[a], order[b]
		x, y := req.Candidates[i], req.Candidates[j]
		if req.Premium && x.Competence != y.Competence {
			return x.Competence > y.Competence
		}
		if known[i] != known[j] {
			return known[i]
		}
		if known[i] && costs[i] != costs[j] {
			return costs[i] < costs[j]
		}
		if !known[i] && x.Tier != y.Tier {
			return x.Tier < y.Tier
		}
		return x.Name < y.Name
	})
	return order[0]
}

func handlePlan(inv bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if inv.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	var req PlanRequest
	if len(request) > 128*1024 || json.Unmarshal(request, &req) != nil || req.Version != 1 ||
		req.InputTokens < 0 || req.OutputTokens < 0 || len(req.Candidates) == 0 || len(req.Candidates) > 16 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	seen := map[string]bool{}
	for _, c := range req.Candidates {
		if c.Name == "" || seen[c.Name] || c.Competence < 0 || c.Competence > 100 || len(c.Bands) > 8 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		seen[c.Name] = true
	}
	selected := Plan(req)
	if inv.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	out, _ := json.Marshal(struct {
		Selected int `json:"selected"`
	}{selected})
	return out, bus.ModuleStatusOK
}
