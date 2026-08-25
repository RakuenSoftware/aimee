package delegates

// Where a coordinated run's patches stand, and which ones a human can safely
// look at next.
//
// This is read-only integration policy. It never merges anything; it decides
// what each packet's state IS -- planned, running, returned, reviewable,
// accepted, failed, or needs_supervisor -- so a supervisor can see at a glance
// which packets are ready and which are waiting on them.
//
// "Reviewable" is the load-bearing one. A packet earns it only by clearing
// every check: a believable handoff, no edits outside its ownership, a base
// that matches the integration base, focused tests that actually passed, and no
// file overlap with a packet already declared reviewable. Overlap is checked
// against the packets ALREADY reviewable rather than against everything, so two
// packets touching one file leave the first reviewable and send only the second
// to a human -- otherwise a single collision would stall both.

const (
	patchMaxFiles     = 32
	patchMaxTasks     = 64
	patchStateLen     = 32
	patchNoteLen      = 256
	patchFileLen      = 256
	patchNextCmdLen   = 64
	patchDefaultNext  = "./aimee git verify"
	patchReviewSchema = "delegate_review_v1"
	patchResultSchema = "delegate_result_v1"
)

// PatchTask is one coordinated task, as the caller knows it.
type PatchTask struct {
	ID     int
	StepID int
	Status string
	Files  string // owned_files JSON
	Result string // the delegate's result JSON
	Error  string
}

// PatchTaskReport mirrors delegate_patch_task_report_t.
type PatchTaskReport struct {
	TaskID                int
	StepID                int
	TaskStatus            string
	PatchState            string
	HandoffStatus         string
	HandoffValid          bool
	ChangedFilesCount     int
	PassedTests           int
	OutsideOwnershipCount int
	OverlapTaskID         int
	StaleBase             bool
	SupervisorActions     int
	Note                  string
}

// PatchReport mirrors delegate_patch_report_t.
type PatchReport struct {
	ImplementationPackets     int
	Planned                   int
	Running                   int
	Returned                  int
	Verified                  int
	Reviewable                int
	Accepted                  int
	Failed                    int
	NeedsSupervisor           int
	InvalidHandoffs           int
	OutsideOwnershipTouches   int
	PatchOverlaps             int
	StaleWorktrees            int
	FocusedTestsPassed        int
	ReviewerPackets           int
	ReviewerBlockingFindings  int
	ReviewerOwnerPacketRoutes int
	ReviewerStatus            string
	RecommendedNextCommand    string
	Tasks                     []PatchTaskReport
}

func isSchema(v *jsonValue, name string) bool {
	s := v.get("schema_version")
	return s.isString() && s.str == name
}

func isHandoffOrReview(v *jsonValue) bool {
	return isSchema(v, patchResultSchema) || isSchema(v, patchReviewSchema)
}

// patchHandoffText finds the handoff inside a result, which may BE one or carry
// one as a `response` string or object.
func patchHandoffText(root *jsonValue, raw string) string {
	if isHandoffOrReview(root) {
		return raw
	}
	response := root.get("response")
	if response.isString() && response.str != "" {
		return response.str
	}
	if response.isObject() {
		return printJSON(response)
	}
	return ""
}

func patchHandoffObject(root *jsonValue, text string) *jsonValue {
	if isHandoffOrReview(root) {
		return root
	}
	if text == "" {
		return nil
	}
	if parsed, ok := parseJSONPrefix(text); ok && isHandoffOrReview(parsed) {
		return parsed
	}
	return nil
}

func stringList(v *jsonValue) []string {
	if v == nil || v.kind != jsonArray {
		return nil
	}
	out := make([]string, 0, len(v.items))
	for _, item := range v.items {
		if !item.isString() || item.str == "" {
			continue
		}
		if len(out) >= patchMaxFiles {
			break
		}
		out = append(out, item.str)
	}
	return out
}

func listsOverlap(a, b []string) bool {
	for _, x := range a {
		for _, y := range b {
			if x == y {
				return true
			}
		}
	}
	return false
}

func addUnique(list []string, add []string) []string {
	for _, path := range add {
		if path == "" || len(list) >= patchMaxFiles {
			break
		}
		found := false
		for _, have := range list {
			if have == path {
				found = true
				break
			}
		}
		if !found {
			list = append(list, path)
		}
	}
	return list
}

func supervisorActionCount(handoff *jsonValue) int {
	actions := handoff.get("supervisor_actions")
	if actions == nil || actions.kind != jsonArray {
		return 0
	}
	return len(actions.items)
}

// firstString returns the first of the given keys that holds a string, so the
// several spellings delegates use for the same fact are all understood.
func firstString(v *jsonValue, keys ...string) (string, bool) {
	for _, k := range keys {
		if item := v.get(k); item.isString() {
			return item.str, true
		}
	}
	return "", false
}

// handoffBaseIsStale asks whether the delegate worked from a base other than
// the integration base. An explicit flag wins; otherwise the two commits are
// compared, and only when BOTH are known -- an unknown base is not evidence of
// staleness.
func handoffBaseIsStale(handoff *jsonValue) bool {
	if !handoff.isObject() {
		return false
	}
	stale := handoff.get("stale_base")
	if stale == nil {
		stale = handoff.get("worktree_stale")
	}
	if stale != nil && stale.kind == jsonBool && stale.boolean {
		return true
	}
	base, haveBase := firstString(handoff, "base_commit", "delegate_base_commit", "base_sha")
	integration, haveIntegration := firstString(handoff,
		"integration_base_commit", "integration_head", "integration_base_sha")
	return haveBase && haveIntegration && base != integration
}

// reviewerBlockingCount counts findings that are not explicitly minor. A
// finding with no severity counts as blocking: unlabelled is not the same as
// harmless, and the conservative reading sends it to a human.
func reviewerBlockingCount(findings *jsonValue) int {
	if findings == nil || findings.kind != jsonArray {
		return 0
	}
	count := 0
	for _, finding := range findings.items {
		severity := finding.get("severity")
		if !severity.isString() {
			count++
			continue
		}
		if severity.str != "note" && severity.str != "low" {
			count++
		}
	}
	return count
}

// reviewerFindingRoutes counts findings that name the packet responsible, which
// is what lets a finding be routed back rather than landing on the supervisor.
func reviewerFindingRoutes(findings *jsonValue) int {
	if findings == nil || findings.kind != jsonArray {
		return 0
	}
	count := 0
	for _, finding := range findings.items {
		if owner := finding.get("owner_packet"); owner.isString() && owner.str != "" {
			count++
		}
	}
	return count
}

func (r *PatchReport) addState(state string) {
	switch state {
	case "planned":
		r.Planned++
	case "running":
		r.Running++
	case "returned":
		r.Returned++
	case "verified":
		r.Verified++
	case "reviewable":
		r.Reviewable++
	case "accepted":
		r.Accepted++
	case "failed":
		r.Failed++
	case "needs_supervisor":
		r.NeedsSupervisor++
	}
	// "reviewer" is deliberately absent: a read-only review packet is not an
	// implementation packet and is counted on its own.
}

func setTaskState(tr *PatchTaskReport, r *PatchReport, state, note string) {
	tr.PatchState = state
	if note != "" {
		tr.Note = note
	}
	r.addState(state)
}

func (r *PatchReport) processReviewer(handoff *jsonValue) {
	r.ReviewerPackets++
	if status := handoff.get("status"); status.isString() && status.str != "" {
		r.ReviewerStatus = status.str
	} else {
		r.ReviewerStatus = "needs_supervisor"
	}
	findings := handoff.get("findings")
	if r.ReviewerStatus == "block" {
		r.ReviewerBlockingFindings += reviewerBlockingCount(findings)
	}
	r.ReviewerOwnerPacketRoutes += reviewerFindingRoutes(findings)
}

func orElse(primary, fallback string) string {
	if primary != "" {
		return primary
	}
	return fallback
}

// BuildPatchReport summarises where a coordinated run's patches stand.
func BuildPatchReport(tasks []PatchTask) PatchReport {
	report := PatchReport{
		ReviewerStatus:         "not_run",
		RecommendedNextCommand: patchDefaultNext,
	}
	if len(tasks) == 0 {
		return report
	}

	// Files already claimed by a reviewable packet, and who claimed them.
	var reviewableFiles [][]string
	var reviewableTaskIDs []int

	for _, task := range tasks {
		if len(report.Tasks) >= patchMaxTasks {
			break
		}
		report.Tasks = append(report.Tasks, PatchTaskReport{
			TaskID:     task.ID,
			StepID:     task.StepID,
			TaskStatus: task.Status,
		})
		tr := &report.Tasks[len(report.Tasks)-1]

		switch task.Status {
		case "pending":
			report.ImplementationPackets++
			setTaskState(tr, &report, "planned", "packet has not launched")
			continue
		case "claimed", "running":
			report.ImplementationPackets++
			setTaskState(tr, &report, "running", "delegate is active")
			continue
		case "failed":
			report.ImplementationPackets++
			setTaskState(tr, &report, "failed", orElse(task.Error, "delegate failed"))
			continue
		}

		var root *jsonValue
		if task.Result != "" {
			if parsed, ok := parseJSONPrefix(task.Result); ok {
				root = parsed
			}
		}
		text := patchHandoffText(root, task.Result)
		handoff := patchHandoffObject(root, text)

		if isSchema(handoff, patchReviewSchema) {
			report.processReviewer(handoff)
			setTaskState(tr, &report, "reviewer", "read-only reviewer result")
			continue
		}

		report.ImplementationPackets++
		verdict, ok := ValidateHandoff(text, task.Files, true)
		if task.Status != "done" || text == "" || !ok {
			report.InvalidHandoffs++
			tr.HandoffStatus = statusNeedsReview
			note := "missing or invalid structured handoff"
			if task.Status == "done" && text != "" {
				note = orElse(verdict.Error, note)
			}
			setTaskState(tr, &report, "needs_supervisor", note)
			continue
		}

		tr.HandoffValid = verdict.Valid
		tr.HandoffStatus = verdict.Status
		tr.ChangedFilesCount = verdict.ChangedFilesCount
		tr.PassedTests = verdict.PassedTests
		tr.OutsideOwnershipCount = verdict.OutsideOwnershipCount
		tr.SupervisorActions = supervisorActionCount(handoff)
		tr.StaleBase = handoffBaseIsStale(handoff)
		report.FocusedTestsPassed += verdict.PassedTests
		report.OutsideOwnershipTouches += verdict.OutsideOwnershipCount
		if tr.StaleBase {
			report.StaleWorktrees++
		}
		if verdict.PassedTests > 0 {
			report.Verified++
		}

		changedFiles := stringList(handoff.get("changed_files"))

		switch {
		case verdict.Status == "failed":
			setTaskState(tr, &report, "failed", orElse(verdict.Error, "delegate reported failed"))
		case verdict.Status == "blocked":
			setTaskState(tr, &report, "needs_supervisor",
				orElse(verdict.Error, "delegate reported blocked"))
		case verdict.OutsideOwnershipCount > 0:
			setTaskState(tr, &report, "needs_supervisor",
				orElse(verdict.Error, "changed files outside owned_files"))
		case tr.StaleBase:
			setTaskState(tr, &report, "needs_supervisor",
				"delegate base differs from integration base")
		case verdict.PassedTests <= 0:
			setTaskState(tr, &report, "returned",
				orElse(verdict.Error, "focused verification not reported"))
		default:
			overlap := 0
			for i, claimed := range reviewableFiles {
				if listsOverlap(changedFiles, claimed) {
					overlap = reviewableTaskIDs[i]
					break
				}
			}
			if overlap > 0 {
				tr.OverlapTaskID = overlap
				report.PatchOverlaps++
				setTaskState(tr, &report, "needs_supervisor",
					"changed files overlap another packet")
				break
			}
			reviewableFiles = append(reviewableFiles, addUnique(nil, changedFiles))
			reviewableTaskIDs = append(reviewableTaskIDs, task.ID)
			setTaskState(tr, &report, "reviewable", "ownership and verification checks passed")
		}
	}
	return report
}
