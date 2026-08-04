package panel

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const DefaultDeadlineMS = 600000

// Seat is one convened reviewer. Persona is its review lens; Selector is an
// operator's positive pin, empty meaning ordinary eligibility routing.
// Participant and Ordinal are filled in when the panel actually convenes.
type Seat struct {
	Persona     string
	Selector    string
	Participant string
	Ordinal     int
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

type Store struct {
	dir string
}

func NewStore(dir string) (*Store, error) {
	if dir == "" {
		return nil, errors.New("roundtable directory is required")
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	return &Store{dir: abs}, nil
}

// Resolve is the only seat-specification path, and it fails closed. A panel is
// review authority: convening one the operator never configured is worse than
// not reviewing at all, because the unconfigured shape is invisible in the
// result. The caller must name a saved roundtable; an unnamed or unloadable one
// is an error, never an implicit panel. Generic delegation owns agent routing
// for every unpinned seat.
func (s *Store) Resolve(requested string, lenses []string, pins map[string]string) (Panel, error) {
	name := strings.TrimSpace(requested)
	if name == "" {
		return Panel{}, errors.New("no roundtable named: a roundtable review must name a saved roundtable")
	}
	p, err := s.load(name)
	if err != nil {
		return Panel{}, err
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

func lensAt(lenses []string, i int) string {
	if len(lenses) == 0 {
		return "reviewer"
	}
	return lenses[i%len(lenses)]
}
