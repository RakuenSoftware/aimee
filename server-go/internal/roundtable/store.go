package roundtable

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const DirectMaxSeats = 2
const DefaultDeadlineMS = 360000

type Agent struct {
	Name        string
	Provider    string
	MaxParallel int
}

type Seat struct {
	Persona string
	Agent   string
	Pinned  bool
}

type Panel struct {
	Name            string
	Seats           []Seat
	MinSuccessful   int
	Discussion      bool
	DeadlineMS      int
	Chairman        string
	ChairmanEnabled bool
	Acquired        bool
}

type presetSeat struct {
	Model   string `json:"model"`
	Persona string `json:"persona"`
}

type preset struct {
	Name            string       `json:"name"`
	Seats           []presetSeat `json:"seats"`
	MinSuccessful   int          `json:"min_successful"`
	Discussion      bool         `json:"discussion"`
	DeadlineMS      int          `json:"deadline_ms"`
	Chairman        string       `json:"chairman"`
	ChairmanEnabled bool         `json:"chairman_enabled"`
}

type DefaultSource func() (string, error)

type Store struct {
	dir         string
	defaultName DefaultSource
}

func NewStore(dir string, defaultName DefaultSource) (*Store, error) {
	if dir == "" || defaultName == nil {
		return nil, errors.New("roundtable directory and default source are required")
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	return &Store{dir: abs, defaultName: defaultName}, nil
}

// Resolve is the only seat-authority path. A named/default saved roundtable is
// exact. Only the no-preset direct fallback is capped at two diverse agents.
func (s *Store) Resolve(requested string, agents []Agent, lenses []string, pins map[string]string) (Panel, error) {
	configuredDefault, err := s.defaultName()
	if err != nil {
		return Panel{}, fmt.Errorf("load roundtable.default: %w", err)
	}
	name := strings.TrimSpace(requested)
	explicit := name != ""
	configured := !explicit && strings.TrimSpace(configuredDefault) != ""
	if !explicit {
		name = strings.TrimSpace(configuredDefault)
	}
	if name == "" {
		name = "default"
	}
	p, err := s.load(name)
	if err != nil {
		if explicit || configured || !errors.Is(err, os.ErrNotExist) {
			return Panel{}, err
		}
		return directPanel(agents, lenses, pins)
	}
	return resolvePreset(p, agents, lenses, pins)
}

func (s *Store) load(name string) (preset, error) {
	if name == "" || name == "." || name == ".." || strings.ContainsAny(name, `/\\`) {
		return preset{}, fmt.Errorf("invalid roundtable name %q", name)
	}
	data, err := os.ReadFile(filepath.Join(s.dir, name+".json"))
	if err != nil {
		return preset{}, fmt.Errorf("roundtable preset %q: %w", name, err)
	}
	var p preset
	if err := json.Unmarshal(data, &p); err != nil {
		return preset{}, fmt.Errorf("decode roundtable preset %q: %w", name, err)
	}
	if len(p.Seats) == 0 {
		return preset{}, fmt.Errorf("roundtable preset %q has no seats", name)
	}
	if p.Name == "" {
		p.Name = name
	}
	return p, nil
}

func resolvePreset(p preset, agents []Agent, lenses []string, pins map[string]string) (Panel, error) {
	capacity := capacities(agents)
	providerSeats := map[string]int{}
	agentSeats := map[string]int{}
	seats := make([]Seat, 0, len(p.Seats))
	for i, configured := range p.Seats {
		persona := strings.TrimSpace(configured.Persona)
		if persona == "" {
			persona = lensAt(lenses, i)
		}
		model := strings.TrimSpace(configured.Model)
		if pinned := strings.TrimSpace(pins[persona]); pinned != "" {
			model = pinned
		}
		if model == "" || model == "$random" {
			model = pickAgent(agents, capacity, providerSeats, agentSeats)
			if model == "" {
				return Panel{}, fmt.Errorf("roundtable %q requires %d seats but only %d are available", p.Name, len(p.Seats), len(seats))
			}
		} else if capacity[model] <= 0 {
			return Panel{}, fmt.Errorf("required roundtable agent %q is unavailable or over capacity", model)
		}
		capacity[model]--
		agentSeats[model]++
		providerSeats[providerOf(agents, model)]++
		seats = append(seats, Seat{Persona: persona, Agent: model, Pinned: pins[persona] != "" || configured.Model != "" && configured.Model != "$random"})
	}
	minimum := p.MinSuccessful
	if minimum <= 0 {
		minimum = len(seats)
	}
	if minimum > len(seats) {
		return Panel{}, fmt.Errorf("roundtable %q min_successful %d exceeds its %d seats", p.Name, minimum, len(seats))
	}
	deadline := p.DeadlineMS
	if deadline <= 0 {
		deadline = DefaultDeadlineMS
	}
	chairman := strings.TrimSpace(p.Chairman)
	if p.ChairmanEnabled {
		if chairman == "" {
			return Panel{}, fmt.Errorf("roundtable %q enables its chairman without selecting an agent", p.Name)
		}
		if chairman == "$random" {
			freshCapacity := capacities(agents)
			chairman = pickAgent(agents, freshCapacity, providerSeats, agentSeats)
			if chairman == "" {
				return Panel{}, fmt.Errorf("roundtable %q has no eligible chairman", p.Name)
			}
		} else if capacities(agents)[chairman] <= 0 {
			return Panel{}, fmt.Errorf("required roundtable chairman %q is unavailable", chairman)
		}
	}
	return Panel{Name: p.Name, Seats: seats, MinSuccessful: minimum, Discussion: p.Discussion, DeadlineMS: deadline, Chairman: chairman, ChairmanEnabled: p.ChairmanEnabled, Acquired: true}, nil
}

func directPanel(agents []Agent, lenses []string, pins map[string]string) (Panel, error) {
	capacity := capacities(agents)
	providerSeats := map[string]int{}
	agentSeats := map[string]int{}
	var seats []Seat
	for len(seats) < DirectMaxSeats {
		persona := lensAt(lenses, len(seats))
		name := strings.TrimSpace(pins[persona])
		pinned := name != ""
		if !pinned {
			name = pickAgent(agents, capacity, providerSeats, agentSeats)
		}
		if name == "" {
			break
		}
		if capacity[name] <= 0 {
			return Panel{}, fmt.Errorf("required roundtable agent %q is unavailable or over capacity", name)
		}
		capacity[name]--
		agentSeats[name]++
		providerSeats[providerOf(agents, name)]++
		seats = append(seats, Seat{Persona: persona, Agent: name, Pinned: pinned})
	}
	if len(seats) == 0 {
		return Panel{}, errors.New("no enabled review-capable agents")
	}
	return Panel{Seats: seats, MinSuccessful: len(seats)}, nil
}

// ResolveDirect is the explicit no-roundtable path used by compatibility
// callers. Its two-seat maximum cannot be overridden by the caller.
func ResolveDirect(agents []Agent, lenses []string, pins map[string]string) (Panel, error) {
	return directPanel(agents, lenses, pins)
}

func capacities(agents []Agent) map[string]int {
	out := make(map[string]int, len(agents))
	for _, agent := range agents {
		if agent.Name != "" && agent.MaxParallel > 0 {
			out[agent.Name] = agent.MaxParallel
		}
	}
	return out
}

func pickAgent(agents []Agent, capacity, providerSeats, agentSeats map[string]int) string {
	// First represent every provider, then every model. Once all available models
	// are represented, reuse the least-seated model. Provider seat counts break
	// ties so two-provider tables remain balanced instead of exhausting one
	// provider before returning to the other.
	for _, agent := range agents {
		if capacity[agent.Name] > 0 && providerSeats[provider(agent)] == 0 {
			return agent.Name
		}
	}
	best := ""
	bestProviderSeats := int(^uint(0) >> 1)
	for _, agent := range agents {
		if capacity[agent.Name] <= 0 || agentSeats[agent.Name] != 0 {
			continue
		}
		count := providerSeats[provider(agent)]
		if best == "" || count < bestProviderSeats {
			best, bestProviderSeats = agent.Name, count
		}
	}
	if best != "" {
		return best
	}
	bestAgentSeats := int(^uint(0) >> 1)
	bestProviderSeats = int(^uint(0) >> 1)
	for _, agent := range agents {
		if capacity[agent.Name] <= 0 {
			continue
		}
		agentCount := agentSeats[agent.Name]
		providerCount := providerSeats[provider(agent)]
		if best == "" || agentCount < bestAgentSeats ||
			(agentCount == bestAgentSeats && providerCount < bestProviderSeats) {
			best, bestAgentSeats, bestProviderSeats = agent.Name, agentCount, providerCount
		}
	}
	return best
}

func provider(agent Agent) string {
	value := strings.ToLower(strings.TrimSpace(agent.Provider))
	if value == "" {
		return "agent:" + agent.Name
	}
	return value
}

func providerOf(agents []Agent, name string) string {
	for _, agent := range agents {
		if agent.Name == name {
			return provider(agent)
		}
	}
	return "agent:" + name
}

func lensAt(lenses []string, i int) string {
	if len(lenses) == 0 {
		return "reviewer"
	}
	return lenses[i%len(lenses)]
}
