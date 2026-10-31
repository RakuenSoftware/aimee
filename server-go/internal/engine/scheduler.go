package engine

import (
	"context"
	"errors"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/internal/workflowstore"
)

type Scheduler struct {
	db                *workflowstore.Store
	engine            *Engine
	concurrency       int
	concurrencySource func() int
	perWorkflow       int
	perWorkflowSource func() int
	policySource      func() RunPolicy
	pollEvery         time.Duration
	transientPauses   []transientPause
	cancelTerminal    func(context.Context) (int, error)
	cleanupTerminal   func(context.Context, workflowstore.WorkItem) error
	log               *slog.Logger

	mu      sync.Mutex
	running map[string]struct{}
	cancels map[string]context.CancelFunc
	wake    chan struct{}
	// lastTerminal is how many terminal runs the previous pass saw, and
	// cancelRetry when to try cancelling their delegate jobs again.
	lastTerminal int
	cancelRetry  time.Time
}

const (
	// storeRetry is how soon a pass that could not list the store tries again.
	storeRetry = 5 * time.Second
	// cancelTerminalRetry spaces retries of a delegate-job cancellation that
	// failed, or that might have left more to cancel.
	cancelTerminalRetry = 30 * time.Second
)

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
func (s *Scheduler) SetTerminalCleanup(cleanup func(context.Context, workflowstore.WorkItem) error) {
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

func NewScheduler(db *workflowstore.Store, engine *Engine, concurrency int, logger *slog.Logger) *Scheduler {
	if concurrency < 1 {
		concurrency = 1
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &Scheduler{db: db, engine: engine, concurrency: concurrency, perWorkflow: 1,
		transientPauses: append([]transientPause(nil), schedulerTransientPauses...),
		log:             logger, running: make(map[string]struct{}),
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

// Run dispatches work as it becomes runnable. It sleeps until told something
// changed -- a submission, a finished turn, a lifecycle write announced by the
// store (ouroboros ring D14) -- or until the next pause it is waiting out
// expires. An idle scheduler makes no calls at all; it used to sweep the store
// every second.
func (s *Scheduler) Run(ctx context.Context) {
	timer := time.NewTimer(time.Hour)
	stopTimer(timer)
	defer timer.Stop()
	for {
		wait := untilDue(s.fill(ctx), time.Now())
		if s.pollEvery > 0 && (wait < 0 || wait > s.pollEvery) {
			wait = s.pollEvery // a test's ceiling, never set in production
		}
		var due <-chan time.Time
		if wait >= 0 {
			timer.Reset(wait)
			due = timer.C
		}
		select {
		case <-ctx.Done():
			return
		case <-due:
		case <-s.wake:
			stopTimer(timer)
		}
	}
}

func stopTimer(timer *time.Timer) {
	if !timer.Stop() {
		select {
		case <-timer.C:
		default:
		}
	}
}

// minimumRecheck bounds how soon an overdue pause is looked at again. The
// store's timestamps have whole-second precision, so a pause can look due a
// moment before the store agrees; without a floor that moment is a spin.
const minimumRecheck = time.Second

// untilDue turns the next due time into a sleep: negative for "nothing is
// due, sleep until told", otherwise at least minimumRecheck.
func untilDue(next, now time.Time) time.Duration {
	if next.IsZero() {
		return -1
	}
	if wait := next.Sub(now); wait > minimumRecheck {
		return wait
	}
	return minimumRecheck
}

// storeTime parses the store's timestamp spelling (UTC, whole seconds).
func storeTime(stamp string) (time.Time, bool) {
	t, err := time.ParseInLocation("2006-01-02 15:04:05", stamp, time.UTC)
	return t, err == nil
}

// pendingWork is what one listing says the scheduler has to do, and when it
// next has to look again.
type pendingWork struct {
	transientDue  map[string]bool // pause reasons with at least one item past its backoff
	wallResumable bool
	wallAbandon   bool
	fanInParents  bool
	orphans       bool
	terminal      int
	next          time.Time
}

// sweepAll marks every store operation as worth running, for a listing that
// cannot show everything.
func (p *pendingWork) sweepAll(pauses []transientPause, now time.Time) {
	shortest := time.Duration(0)
	for _, pause := range pauses {
		p.transientDue[pause.reason] = true
		if shortest == 0 || pause.backoff < shortest {
			shortest = pause.backoff
		}
	}
	p.wallResumable, p.wallAbandon, p.fanInParents, p.orphans = true, true, true, true
	p.dueAt(now.Add(shortest))
}

func (p *pendingWork) dueAt(when time.Time) {
	if p.next.IsZero() || when.Before(p.next) {
		p.next = when
	}
}

// survey reads the listing once and decides which store operations would do
// anything, so a pass over a quiet store is one call rather than sixteen.
func (s *Scheduler) survey(items []workflowstore.WorkItem, policySource func() RunPolicy,
	now time.Time) pendingWork {
	work := pendingWork{transientDue: map[string]bool{}}
	backoff := make(map[string]time.Duration, len(s.transientPauses))
	for _, pause := range s.transientPauses {
		backoff[pause.reason] = pause.backoff
	}
	policy := RunPolicy{}
	if policySource != nil {
		policy = policySource()
	}
	active := make(map[string]bool, len(items))
	for _, item := range items {
		if item.State == "active" {
			active[item.ID] = true
		}
	}
	for _, item := range items {
		switch item.State {
		case "accepted", "rejected", "stopped", "abandoned":
			work.terminal++
			continue
		case "active":
		default:
			continue
		}
		if item.ParentID != "" && !active[item.ParentID] {
			work.orphans = true
		}
		updated, stamped := storeTime(item.UpdatedAt)
		switch reason := item.PauseReason; {
		case reason == "":
		case reason == "slices_running":
			work.fanInParents = true
		case reason == "wall_cap":
			if policy.AutoResumeWall && item.OverrideCount < policy.MaxResumes {
				work.wallResumable = true
			} else if policy.StaleAbandon > 0 && stamped {
				// Rounded up a second: the store compares at whole seconds.
				when := updated.Add(policy.StaleAbandon + time.Second)
				if !now.Before(when) {
					work.wallAbandon = true
				} else {
					work.dueAt(when)
				}
			}
		default:
			wait, transient := backoff[reason]
			if !transient {
				continue // an operator's or a policy's pause: only a change resumes it
			}
			when := now
			if stamped {
				when = updated.Add(wait + time.Second)
			}
			if !now.Before(when) {
				work.transientDue[reason] = true
			} else {
				work.dueAt(when)
			}
		}
	}
	return work
}

// fill does what the store's current state calls for and dispatches runnable
// items. It returns when it next has to look again on its own (zero: only when
// told).
func (s *Scheduler) fill(ctx context.Context) time.Time {
	s.mu.Lock()
	policySource := s.policySource
	concurrencySource := s.concurrencySource
	perWorkflowSource := s.perWorkflowSource
	cancelTerminal := s.cancelTerminal
	cleanupTerminal := s.cleanupTerminal
	concurrency := s.concurrency
	perWorkflow := s.perWorkflow
	lastTerminal := s.lastTerminal
	cancelRetry := s.cancelRetry
	s.mu.Unlock()

	items, err := s.db.WorkItems(ctx)
	if err != nil {
		s.log.Error("list workflow items", "error", err)
		// The store will announce when it can answer; retry in case it cannot.
		return time.Now().Add(storeRetry)
	}
	now := time.Now()
	work := s.survey(items, policySource, now)
	if len(items) >= workflowstore.ListCeiling {
		// The listing is truncated, so an item it does not show may be due.
		// Sweep everything, as before, and look again at the shortest backoff.
		work.sweepAll(s.transientPauses, now)
	}
	changed := false
	note := func(count int64) {
		if count > 0 {
			changed = true
		}
	}

	if policySource != nil {
		policy := policySource()
		if policy.AutoResumeWall && work.wallResumable {
			if resumed, err := s.db.ResumeWallCaps(ctx, policy.MaxResumes); err != nil {
				s.log.Error("auto-resume wall caps", "error", err)
			} else if resumed > 0 {
				note(resumed)
				s.log.Info("auto-resumed wall-cap workflows", "count", resumed)
			}
		}
		if policy.StaleAbandon > 0 && work.wallAbandon {
			if abandoned, err := s.db.AbandonExhaustedWallCaps(ctx, policy.MaxResumes, policy.StaleAbandon); err != nil {
				s.log.Error("abandon exhausted stale wall caps", "error", err)
			} else if abandoned > 0 {
				note(abandoned)
				s.log.Info("abandoned exhausted stale wall caps", "count", abandoned)
			}
		}
	}
	if work.orphans {
		if stopped, err := s.db.ReconcileOrphanedDescendants(ctx); err != nil {
			s.log.Error("reconcile orphaned workflow descendants", "error", err)
		} else if len(stopped) > 0 {
			changed = true
			for _, workItemID := range stopped {
				s.Cancel(workItemID)
			}
			s.log.Warn("stopped orphaned workflow descendants", "count", len(stopped))
		}
	}
	// A terminal transition can leave delegate jobs to cancel. Look when the
	// number of terminal runs grew, and keep retrying while cancellations fail.
	retryAt := time.Time{}
	if cancelTerminal != nil && (work.terminal > lastTerminal ||
		(!cancelRetry.IsZero() && !now.Before(cancelRetry))) {
		if cancelled, err := cancelTerminal(ctx); err != nil {
			s.log.Error("cancel terminal workflow delegate jobs", "cancelled", cancelled, "error", err)
			retryAt = now.Add(cancelTerminalRetry)
		} else if cancelled > 0 {
			s.log.Info("cancelled terminal workflow delegate jobs", "count", cancelled)
			retryAt = now.Add(cancelTerminalRetry) // confirm none are left
		}
	} else if !cancelRetry.IsZero() {
		retryAt = cancelRetry
	}
	s.mu.Lock()
	s.lastTerminal = work.terminal
	s.cancelRetry = retryAt
	s.mu.Unlock()
	if !retryAt.IsZero() {
		work.dueAt(retryAt)
	}
	for _, pause := range s.transientPauses {
		if !work.transientDue[pause.reason] {
			continue
		}
		if resumed, err := s.db.ResumeTransient(ctx, pause.reason, pause.backoff); err != nil {
			s.log.Error("resume transient workflows", "reason", pause.reason, "error", err)
		} else if resumed > 0 {
			note(resumed)
			s.log.Info("resumed transient workflows", "reason", pause.reason, "count", resumed)
		} else {
			// Past due by our clock, not yet by the store's: look again shortly.
			work.dueAt(now.Add(minimumRecheck))
		}
	}
	if work.fanInParents {
		if resumed, err := s.db.ResumeReadyParents(ctx); err != nil {
			s.log.Error("resume completed fan-in parents", "error", err)
		} else if resumed > 0 {
			note(resumed)
			s.log.Info("resumed completed fan-in parents", "count", resumed)
		}
	}
	if changed {
		// What was resumed or stopped is runnable or finished now: list again.
		if relisted, err := s.db.WorkItems(ctx); err == nil {
			items = relisted
		}
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
	s.dispatch(ctx, items, concurrency, perWorkflow)
	return work.next
}

// dispatch starts runnable items into the free slots.
func (s *Scheduler) dispatch(ctx context.Context, items []workflowstore.WorkItem,
	concurrency, perWorkflow int) {
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
