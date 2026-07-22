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

type Seat struct {
	Persona  string
	Selector string
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
	Selector string `json:"model"`
	Persona  string `json:"persona"`
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

// Resolve is the only seat-specification path. A named/default saved
// roundtable is exact. Only the no-preset direct fallback is capped at two
// seats. Generic delegation owns agent routing for every unpinned seat.
func (s *Store) Resolve(requested string, lenses []string, pins map[string]string) (Panel, error) {
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
		return directPanel(lenses, pins)
	}
	return resolvePreset(p, lenses, pins)
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

func resolvePreset(p preset, lenses []string, pins map[string]string) (Panel, error) {
	seats := make([]Seat, 0, len(p.Seats))
	for i, configured := range p.Seats {
		persona := strings.TrimSpace(configured.Persona)
		if persona == "" {
			persona = lensAt(lenses, i)
		}
		selector := strings.TrimSpace(configured.Selector)
		if pinned := strings.TrimSpace(pins[persona]); pinned != "" {
			selector = pinned
		}
		seats = append(seats, Seat{Persona: persona, Selector: selector})
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
	}
	return Panel{Name: p.Name, Seats: seats, MinSuccessful: minimum, Discussion: p.Discussion, DeadlineMS: deadline, Chairman: chairman, ChairmanEnabled: p.ChairmanEnabled, Acquired: true}, nil
}

func directPanel(lenses []string, pins map[string]string) (Panel, error) {
	seats := make([]Seat, 0, DirectMaxSeats)
	for i := 0; i < DirectMaxSeats; i++ {
		persona := lensAt(lenses, i)
		name := strings.TrimSpace(pins[persona])
		seats = append(seats, Seat{Persona: persona, Selector: name})
	}
	return Panel{Seats: seats, MinSuccessful: len(seats)}, nil
}

// ResolveDirect is the explicit no-roundtable path used by compatibility
// callers. Its two-seat maximum cannot be overridden by the caller.
func ResolveDirect(lenses []string, pins map[string]string) (Panel, error) {
	return directPanel(lenses, pins)
}

func lensAt(lenses []string, i int) string {
	if len(lenses) == 0 {
		return "reviewer"
	}
	return lenses[i%len(lenses)]
}
