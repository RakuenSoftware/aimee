package engine

import (
	"context"
	"errors"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

type Scheduler struct {
	db                *db1.Store
	engine            *Engine
	concurrency       int
	concurrencySource func() int
	perWorkflow       int
	perWorkflowSource func() int
	policySource      func() RunPolicy
	pollEvery         time.Duration
	transientPauses   []transientPause
	cancelTerminal    func(context.Context) (int, error)
	cleanupTerminal   func(context.Context, db1.WorkItem) error
	log               *slog.Logger

	mu      sync.Mutex
	running map[string]struct{}
	cancels map[string]context.CancelFunc
	wake    chan struct{}
}

type transientPause struct {
	reason  string
	backoff time.Duration
}

// Scheduler-owned pauses describe unavailable execution machinery, not a
// lifecycle decision. Retrying them creates a new execution version, so
// cancelled or malformed durable delegate results cannot poison every future
// attempt. Work-item cost and turn caps remain the bounded safety backstops.
var schedulerTransientPauses = []transientPause{
	{reason: "runner_unavailable", backoff: 5 * time.Second},
	// Provider throttles advertise retry-after windows in the tens of seconds;
	// re-dispatching on the runner_unavailable backoff just re-trips the limit.
	{reason: "capacity_backpressure", backoff: 30 * time.Second},
	// Panel capacity is the same self-clearing resource pressure, but remains a
	// distinct workflow state so it cannot be confused with provider reachability.
	{reason: "panel_capacity", backoff: 30 * time.Second},
	{reason: "panel_capacity_deadline", backoff: 30 * time.Second},
	{reason: "panel_deadline", backoff: 60 * time.Second},
	{reason: "ci_pending", backoff: 15 * time.Second},
	{reason: "merge_pending", backoff: 15 * time.Second},
	{reason: "panel_unreachable", backoff: 60 * time.Second},
	{reason: "roundtable_discussion", backoff: 60 * time.Second},
	{reason: "roundtable_chairman", backoff: 60 * time.Second},
	// A generated slice rechecks the durable state of its declared predecessors.
	// This is scheduler-owned waiting, not an operator decision; once the prior
	// slice merges and reaches accepted the dependent slice becomes runnable.
	{reason: "dependency_pending", backoff: 30 * time.Second},
}

func (s *Scheduler) SetConcurrencySource(source func() int) {
	s.mu.Lock()
	s.concurrencySource = source
	s.mu.Unlock()
	s.Notify()
}

// SetPerWorkflowSource installs a live override for the per-workflow concurrency
// cap: at most this many work items belonging to the same root workflow run may
// execute at once. It bounds how much of the global agent budget a single
// workflow can consume, so one fan-out cannot starve every other run.
func (s *Scheduler) SetPerWorkflowSource(source func() int) {
	s.mu.Lock()
	s.perWorkflowSource = source
	s.mu.Unlock()
	s.Notify()
}
func (s *Scheduler) SetPolicySource(source func() RunPolicy) {
	s.mu.Lock()
	s.policySource = source
	s.mu.Unlock()
	s.Notify()
}

func (s *Scheduler) SetTerminalCancellation(cancel func(context.Context) (int, error)) {
	s.mu.Lock()
	s.cancelTerminal = cancel
	s.mu.Unlock()
	s.Notify()
}

// SetTerminalCleanup installs the managed-worktree cleanup boundary. Terminal
// rows remain as workflow history, but their checkouts must not accumulate
// forever after the final transition.
func (s *Scheduler) SetTerminalCleanup(cleanup func(context.Context, db1.WorkItem) error) {
	s.mu.Lock()
	s.cleanupTerminal = cleanup
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
	return &Scheduler{db: db, engine: engine, concurrency: concurrency, perWorkflow: 1,
		pollEvery: time.Second, transientPauses: append([]transientPause(nil), schedulerTransientPauses...),
		log: logger, running: make(map[string]struct{}),
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
	perWorkflowSource := s.perWorkflowSource
	cancelTerminal := s.cancelTerminal
	cleanupTerminal := s.cleanupTerminal
	concurrency := s.concurrency
	perWorkflow := s.perWorkflow
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
	if stopped, err := s.db.ReconcileOrphanedDescendants(ctx); err != nil {
		s.log.Error("reconcile orphaned workflow descendants", "error", err)
	} else if len(stopped) > 0 {
		for _, workItemID := range stopped {
			s.Cancel(workItemID)
		}
		s.log.Warn("stopped orphaned workflow descendants", "count", len(stopped))
	}
	if cancelTerminal != nil {
		if cancelled, err := cancelTerminal(ctx); err != nil {
			s.log.Error("cancel terminal workflow delegate jobs", "cancelled", cancelled, "error", err)
		} else if cancelled > 0 {
			s.log.Info("cancelled terminal workflow delegate jobs", "count", cancelled)
		}
	}
	for _, pause := range s.transientPauses {
		if resumed, err := s.db.ResumeTransient(ctx, pause.reason, pause.backoff); err != nil {
			s.log.Error("resume transient workflows", "reason", pause.reason, "error", err)
		} else if resumed > 0 {
			s.log.Info("resumed transient workflows", "reason", pause.reason, "count", resumed)
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
	if perWorkflowSource != nil {
		if live := perWorkflowSource(); live > 0 {
			perWorkflow = live
		}
	}
	if perWorkflow < 1 {
		perWorkflow = 1
	}
	items, err := s.db.WorkItems(ctx)
	if err != nil {
		s.log.Error("list workflow items", "error", err)
		return
	}
	if cleanupTerminal != nil {
		for _, item := range items {
			terminal := item.State == "accepted" || item.State == "rejected" || item.State == "stopped"
			if !terminal || item.Worktree == "" {
				continue
			}
			s.mu.Lock()
			_, running := s.running[item.ID]
			s.mu.Unlock()
			if running {
				continue
			}
			if err := cleanupTerminal(ctx, item); err != nil {
				s.log.Error("clean terminal workflow worktree", "work_item", item.ID, "error", err)
				continue
			}
			if err := s.db.SetWorktree(ctx, item.ID, ""); err != nil {
				s.log.Error("clear terminal workflow worktree", "work_item", item.ID, "error", err)
			}
		}
	}
	s.mu.Lock()
	available := concurrency - len(s.running)
	s.mu.Unlock()
	if available <= 0 {
		return
	}
	// WorkItems is newest-first. Traverse oldest-first so continuously arriving
	// work cannot starve an older runnable item.
	for i := len(items) - 1; i >= 0 && available > 0; i-- {
		item := items[i]
		if item.State != "active" || item.PauseReason != "" {
			continue
		}
		root := rootWorkflowID(item.ID)
		s.mu.Lock()
		if _, exists := s.running[item.ID]; exists {
			s.mu.Unlock()
			continue
		}
		// Per-workflow cap: never let one root workflow occupy more than
		// perWorkflow slots at once, even when the global budget has room. The
		// count includes items dispatched earlier in this same pass, since they
		// were already added to s.running above.
		if s.runningForRootLocked(root) >= perWorkflow {
			s.mu.Unlock()
			continue
		}
		s.running[item.ID] = struct{}{}
		s.mu.Unlock()
		available--
		go s.drive(ctx, item.ID)
	}
}

// rootWorkflowID returns the root work-item ID shared by every item in a single
// workflow run. Children are named "<root>.s<split>.g<gen>.<slice>", so the root
// is the prefix up to the first "." (a root ID like "wi_<hash>" has none).
func rootWorkflowID(id string) string {
	if dot := strings.IndexByte(id, '.'); dot >= 0 {
		return id[:dot]
	}
	return id
}

// runningForRootLocked counts currently-dispatched work items belonging to the
// given root workflow. Caller must hold s.mu.
func (s *Scheduler) runningForRootLocked(root string) int {
	n := 0
	for id := range s.running {
		if rootWorkflowID(id) == root {
			n++
		}
	}
	return n
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
