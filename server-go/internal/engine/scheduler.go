package engine

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

type Scheduler struct {
	db                *db1.Store
	engine            *Engine
	concurrency       int
	concurrencySource func() int
	policySource      func() RunPolicy
	pollEvery         time.Duration
	log               *slog.Logger

	mu      sync.Mutex
	running map[string]struct{}
	cancels map[string]context.CancelFunc
	wake    chan struct{}
}

func (s *Scheduler) SetConcurrencySource(source func() int) {
	s.mu.Lock()
	s.concurrencySource = source
	s.mu.Unlock()
	s.Notify()
}
func (s *Scheduler) SetPolicySource(source func() RunPolicy) {
	s.mu.Lock()
	s.policySource = source
	s.mu.Unlock()
	s.Notify()
}

type RunPolicy struct {
	MaxTurns       int
	MaxWall        time.Duration
	AutoResumeWall bool
	MaxResumes     int
	StaleAbandon   time.Duration
}

func NewScheduler(db *db1.Store, engine *Engine, concurrency int, logger *slog.Logger) *Scheduler {
	if concurrency < 1 {
		concurrency = 1
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &Scheduler{db: db, engine: engine, concurrency: concurrency,
		pollEvery: time.Second, log: logger, running: make(map[string]struct{}),
		cancels: make(map[string]context.CancelFunc), wake: make(chan struct{}, 1)}
}

// Cancel stops the currently executing turn for a work item. The database
// lifecycle transition remains the source of truth; cancellation only prevents
// an already-dispatched turn from crossing a later side-effect boundary.
func (s *Scheduler) Cancel(workItemID string) {
	s.mu.Lock()
	cancel := s.cancels[workItemID]
	s.mu.Unlock()
	if cancel != nil {
		cancel()
	}
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
	s.mu.Lock()
	policySource := s.policySource
	concurrencySource := s.concurrencySource
	concurrency := s.concurrency
	s.mu.Unlock()
	if policySource != nil {
		policy := policySource()
		if policy.AutoResumeWall {
			if resumed, err := s.db.ResumeWallCaps(ctx, policy.MaxResumes); err != nil {
				s.log.Error("auto-resume wall caps", "error", err)
			} else if resumed > 0 {
				s.log.Info("auto-resumed wall-cap workflows", "count", resumed)
			}
		}
		if policy.StaleAbandon > 0 {
			if abandoned, err := s.db.AbandonExhaustedWallCaps(ctx, policy.MaxResumes, policy.StaleAbandon); err != nil {
				s.log.Error("abandon exhausted stale wall caps", "error", err)
			} else if abandoned > 0 {
				s.log.Info("abandoned exhausted stale wall caps", "count", abandoned)
			}
		}
	}
	for reason, backoff := range map[string]time.Duration{"runner_unavailable": 5 * time.Second, "ci_pending": 15 * time.Second, "merge_pending": 15 * time.Second, "panel_unreachable": 60 * time.Second} {
		if resumed, err := s.db.ResumeTransient(ctx, reason, backoff); err != nil {
			s.log.Error("resume transient workflows", "reason", reason, "error", err)
		} else if resumed > 0 {
			s.log.Info("resumed transient workflows", "reason", reason, "count", resumed)
		}
	}
	if resumed, err := s.db.ResumeReadyParents(ctx); err != nil {
		s.log.Error("resume completed fan-in parents", "error", err)
	} else if resumed > 0 {
		s.log.Info("resumed completed fan-in parents", "count", resumed)
	}
	if concurrencySource != nil {
		if live := concurrencySource(); live > 0 {
			concurrency = live
		}
	}
	s.mu.Lock()
	available := concurrency - len(s.running)
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
	runCtx, runCancel := context.WithCancel(ctx)
	s.mu.Lock()
	s.cancels[workItemID] = runCancel
	s.mu.Unlock()
	defer func() {
		runCancel()
		s.mu.Lock()
		delete(s.running, workItemID)
		delete(s.cancels, workItemID)
		s.mu.Unlock()
		s.Notify() // fill the freed slot immediately; do not wait for the poll.
	}()
	started := time.Now()
	for {
		s.mu.Lock()
		policySource := s.policySource
		s.mu.Unlock()
		policy := RunPolicy{}
		if policySource != nil {
			policy = policySource()
		}
		if policy.MaxTurns > 0 {
			turns, err := s.db.ExecutedTurnCount(runCtx, workItemID)
			if err != nil {
				s.log.Error("count workflow turns", "work_item", workItemID, "error", err)
				return
			}
			if turns >= policy.MaxTurns {
				item, err := s.db.WorkItem(ctx, workItemID)
				if err == nil && item.State == "active" && item.PauseReason == "" {
					_ = s.db.Park(ctx, workItemID, item.Stage, "turn_cap", 0)
				}
				return
			}
		}
		remaining := policy.MaxWall - time.Since(started)
		if policy.MaxWall > 0 && remaining <= 0 {
			item, err := s.db.WorkItem(context.WithoutCancel(ctx), workItemID)
			if err == nil && item.State == "active" && item.PauseReason == "" {
				_ = s.db.Park(context.WithoutCancel(ctx), workItemID, item.Stage, "wall_cap", 0)
			}
			return
		}
		stepCtx := runCtx
		cancel := func() {}
		if policy.MaxWall > 0 {
			stepCtx, cancel = context.WithTimeout(runCtx, remaining)
		}
		result, err := s.engine.Advance(stepCtx, workItemID)
		cancel()
		if err != nil {
			if errors.Is(err, context.Canceled) {
				return
			}
			s.log.Error("advance workflow", "work_item", workItemID, "error", err)
			return
		}
		if result.Terminal || result.Parked || !result.Ran {
			return
		}
	}
}
