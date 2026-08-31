package delegates

import (
	"fmt"
	"strings"
)

// Turning a delegate plan into the work a coordinator job will run.
//
// The plan arrives as packets: a title, an objective, the files a packet owns,
// and a role. What comes out is one execution step and one coord task per
// implementation packet, plus the prompt each delegate is briefed with.
//
// Every decision here is about the PLAN's shape -- which packets are real work,
// whether a packet is allowed to launch, what its delegate is told. None of it
// is about storage: creating the execution plan, the coord job, and the task
// rows stays with the caller, which owns the database. This side reads a plan
// and returns rows; it never writes one.
//
// Facts the caller must supply, because this side does no I/O: for each owned
// file, whether it exists, and -- when it does not -- which tracked files share
// its basename. Both travel IN the request. See LaunchOwnedFile for why the
// caller sends candidates rather than the whole repository.

// MaxPlanSteps caps how many implementation packets one plan may launch.
// Mirrors AGENT_MAX_PLAN_STEPS (src/headers/agent_types.h).
const MaxPlanSteps = 32

// DefaultMaxConcurrent mirrors DB1_COORD_DEFAULT_PAR (src/db1/coord_jobs.h):
// what a plan runs at when the request does not say.
const DefaultMaxConcurrent = 3

// A packet as the plan describes it, with the caller's facts attached.
type LaunchPacket struct {
	ID         string
	Title      string
	Objective  string
	Role       string
	OwnedFiles []LaunchOwnedFile
	// HandoffSchema is the packet's declared result contract. An
	// implementation packet must declare delegate_result_v1 -- a delegate
	// whose result nobody can parse is work that cannot be checked.
	HandoffSchema string
}

// IsReview reports whether this packet is a reviewer rather than work.
//
// It is derived from the role, not carried as a flag, because that is what
// decides it -- a caller that set the two inconsistently would get a reviewer
// launched as an implementation packet, or work silently dropped.
//
// Reviewers own no files and produce no coord task: they are carried in the
// plan but never launched.
func (p LaunchPacket) IsReview() bool { return p.Role == "review" }

// One owned file, with the facts about it this side cannot look up.
type LaunchOwnedFile struct {
	Path string
	// Exists is the caller's answer for THIS path, from a filesystem check.
	// It is not "is it tracked by git" -- an untracked new file exists and
	// needs no repair, and conflating the two would send a delegate hunting
	// for a file that is already sitting in its worktree.
	Exists bool
	// Candidates are the repository's tracked files sharing this path's
	// basename. Only meaningful when Exists is false; the caller may leave it
	// empty otherwise.
	//
	// The caller sends candidates rather than its whole file list because the
	// list does not fit: this repository's is 208KB against a 1MiB event
	// payload, and a larger monorepo would simply not send. Selecting by
	// basename is a lookup, not a decision -- what a count of one, zero, or
	// several MEANS is decided below, and that is the part worth having in one
	// place.
	Candidates []string
}

// What the plan as a whole says.
type LaunchPlan struct {
	Schema string
	Title  string
	// MissingOwnedFiles is the planner's own list of files it could not
	// account for. A non-empty list blocks the launch: the plan is asking a
	// delegate to own something the planner could not find, and that is a
	// question for a human, not a repair this side can make.
	MissingOwnedFiles []string
	Packets           []LaunchPacket
}

// What the caller should do, once this side has read the plan.
type LaunchDecision struct {
	// Error is set when the plan must not launch. The caller reports it and
	// writes nothing.
	Error string
	// Steps are the execution-plan steps, in packet order.
	Steps []LaunchStep
	// Tasks are the coord tasks, in packet order, one per implementation
	// packet. Tasks[i] belongs with Steps[i].
	Tasks []LaunchTask
	// Repairs record every path this side rewrote, for the caller to log.
	Repairs []LaunchRepair
	// Warnings record paths left alone that may not resolve at runtime.
	Warnings []string
	// MaxConcurrent is the effective concurrency after defaulting.
	MaxConcurrent int
}

type LaunchStep struct {
	Action           string
	Precondition     string
	SuccessPredicate string
	Rollback         string
}

type LaunchTask struct {
	OwnedFiles []string
	Role       string
	Prompt     string
}

type LaunchRepair struct {
	From string
	To   string
}

// PlanLaunch reads a plan and returns the work it becomes.
//
// The plan is REJECTED, with nothing written, when: it is not a
// delegate_plan_v1; it carries planner-reported missing owned files; a packet
// owns no files, declares no result contract, or names a path whose basename is
// ambiguous in the repository; it yields more than MaxPlanSteps packets; or it
// yields none at all.
func PlanLaunch(plan LaunchPlan, maxConcurrent int) LaunchDecision {
	d := LaunchDecision{MaxConcurrent: maxConcurrent}
	if d.MaxConcurrent <= 0 {
		d.MaxConcurrent = DefaultMaxConcurrent
	}

	if plan.Schema != "delegate_plan_v1" {
		d.Error = "invalid delegate plan schema"
		return d
	}
	if len(plan.MissingOwnedFiles) > 0 {
		d.Error = fmt.Sprintf(
			"delegate plan has missing owned_files: %s; review or mark new files before launching",
			plan.MissingOwnedFiles[0])
		return d
	}

	for _, p := range plan.Packets {
		// A reviewer packet is carried, not launched: no step, no task.
		if p.IsReview() {
			continue
		}

		files, repairs, warnings, err := resolveOwnedFiles(p)
		if err != "" {
			d.Error = err
			return d
		}
		d.Repairs = append(d.Repairs, repairs...)
		d.Warnings = append(d.Warnings, warnings...)

		if len(files) == 0 {
			d.Error = "delegate plan packet missing owned_files"
			return d
		}
		if p.HandoffSchema != "delegate_result_v1" {
			d.Error = "delegate plan packet missing handoff_schema delegate_result_v1"
			return d
		}

		d.Steps = append(d.Steps, launchStep(p))
		d.Tasks = append(d.Tasks, LaunchTask{
			OwnedFiles: files,
			Role:       roleOrDefault(p.Role),
			Prompt:     LaunchPrompt(p, files),
		})

		if len(d.Steps) > MaxPlanSteps {
			d.Error = "delegate plan has too many implementation packets"
			return d
		}
	}

	if len(d.Steps) == 0 {
		d.Error = "delegate plan has no implementation packets"
		return d
	}
	return d
}

// resolveOwnedFiles applies the path repair to one packet's files.
//
// A path whose file exists is left alone. A path whose file does not is looked
// up by basename in the repository: exactly one match is a repair, no match is
// a warning and the path stands (the planner may legitimately name a file the
// packet is about to create), and several matches is an error -- there is no
// safe way to pick, and guessing would point a delegate at the wrong file.
func resolveOwnedFiles(p LaunchPacket) (files []string,
	repairs []LaunchRepair, warnings []string, errMsg string) {

	for _, f := range p.OwnedFiles {
		if f.Path == "" {
			continue
		}
		if f.Exists {
			files = append(files, f.Path)
			continue
		}

		base := basename(f.Path)
		if base == "" {
			return nil, nil, nil, fmt.Sprintf("packet owned_files: invalid path '%s'", f.Path)
		}

		switch matches := f.Candidates; len(matches) {
		case 1:
			repairs = append(repairs, LaunchRepair{From: f.Path, To: matches[0]})
			files = append(files, matches[0])
		case 0:
			warnings = append(warnings, fmt.Sprintf(
				"packet owned_files: '%s' not found in repository (basename '%s' has no match in tracked files)",
				f.Path, base))
			files = append(files, f.Path)
		default:
			return nil, nil, nil, fmt.Sprintf(
				"packet owned_files: '%s' not found; ambiguous basename '%s' matches: %s -- repair the path before launching",
				f.Path, base, strings.Join(matches, ", "))
		}
	}
	return files, repairs, warnings, ""
}

// LaunchPrompt is what a delegate is told about its packet.
//
// The owned-file list is stated in full. The C this replaces built it into a
// fixed 1KB buffer and silently stopped at the boundary, so a packet owning
// enough files was briefed with a truncated list while its task row still
// carried every one -- the delegate was told to modify only files it had not
// been shown. Nothing depended on the truncation; it was a buffer, not a rule.
func LaunchPrompt(p LaunchPacket, files []string) string {
	title := p.Title
	if title == "" {
		title = "delegate packet"
	}
	objective := p.Objective
	if objective == "" {
		objective = title
	}
	list := strings.Join(files, ", ")
	if list == "" {
		list = "(none)"
	}
	return fmt.Sprintf("%s\n\nObjective: %s\n\nOwned files (modify only these): %s\n\n"+
		"When done, respond with a structured handoff JSON (delegate_result_v1 schema).",
		title, objective, list)
}

func launchStep(p LaunchPacket) LaunchStep {
	action := p.Title
	if action == "" {
		action = p.ID
	}
	if action == "" {
		action = "delegate packet"
	}
	precondition := p.ID
	if precondition == "" {
		precondition = "delegate packet"
	}
	success := p.Objective
	if success == "" {
		success = "delegate packet completed"
	}
	return LaunchStep{Action: action, Precondition: precondition, SuccessPredicate: success}
}

func roleOrDefault(role string) string {
	if role == "" {
		return "execute"
	}
	return role
}

func basename(path string) string {
	if i := strings.LastIndexByte(path, '/'); i >= 0 {
		return path[i+1:]
	}
	return path
}
