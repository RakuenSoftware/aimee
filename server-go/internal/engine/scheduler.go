package engine

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

type Scheduler struct {
	db          *db1.Store
	engine      *Engine
	concurrency int
	pollEvery   time.Duration
	log         *slog.Logger

	mu      sync.Mutex
	running map[string]struct{}
	wake    chan struct{}
}

func NewScheduler(db *db1.Store, engine *Engine, concurrency int, logger *slog.Logger) *Scheduler {
	if concurrency < 1 {
		concurrency = 1
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &Scheduler{db: db, engine: engine, concurrency: concurrency,
		pollEvery: time.Second, log: logger, running: make(map[string]struct{}), wake: make(chan struct{}, 1)}
}

func (s *Scheduler) Notify() {
	select {
	case s.wake <- struct{}{}:
	default:
	}
}

func (s *Scheduler) Run(ctx context.Context) {
	ticker := time.NewTicker(s.pollEvery)
	defer ticker.Stop()
	for {
		s.fill(ctx)
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		case <-s.wake:
		}
	}
}

func (s *Scheduler) fill(ctx context.Context) {
	if resumed, err := s.db.ResumeTransient(ctx, "runner_unavailable", 5*time.Second); err != nil {
		s.log.Error("resume transient workflows", "error", err)
	} else if resumed > 0 {
		s.log.Info("resumed workflows after runner recovery backoff", "count", resumed)
	}
	s.mu.Lock()
	available := s.concurrency - len(s.running)
	s.mu.Unlock()
	if available <= 0 {
		return
	}
	items, err := s.db.WorkItems(ctx)
	if err != nil {
		s.log.Error("list workflow items", "error", err)
		return
	}
	// WorkItems is newest-first. Traverse oldest-first so continuously arriving
	// work cannot starve an older runnable item.
	for i := len(items) - 1; i >= 0 && available > 0; i-- {
		item := items[i]
		if item.State != "active" || item.PauseReason != "" {
			continue
		}
		s.mu.Lock()
		if _, exists := s.running[item.ID]; exists {
			s.mu.Unlock()
			continue
		}
		s.running[item.ID] = struct{}{}
		s.mu.Unlock()
		available--
		go s.drive(ctx, item.ID)
	}
}

func (s *Scheduler) drive(ctx context.Context, workItemID string) {
	defer func() {
		s.mu.Lock()
		delete(s.running, workItemID)
		s.mu.Unlock()
		s.Notify() // fill the freed slot immediately; do not wait for the poll.
	}()
	for {
		result, err := s.engine.Advance(ctx, workItemID)
		if err != nil {
			s.log.Error("advance workflow", "work_item", workItemID, "error", err)
			return
		}
		if result.Terminal || result.Parked || !result.Ran {
			return
		}
	}
}
