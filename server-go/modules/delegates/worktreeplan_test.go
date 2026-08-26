package delegates

import "testing"

// A write delegate needs somewhere of its own so the supervisor can inspect and
// merge its edits deliberately.
func TestPlanWorktreeWriteRoleIsIsolated(t *testing.T) {
	// The caller composes this from the role AND its prompt rule; the module is
	// told the answer. Roles named for readability.
	for _, role := range []string{"code", "refactor", "implement"} {
		plan, err := PlanWorktree(true, "deleg-7")
		if err != nil {
			t.Fatalf("%s: %v", role, err)
		}
		if !plan.Isolated {
			t.Errorf("%s: a write role did not get its own worktree", role)
		}
		if plan.ReadOnlyMount {
			t.Errorf("%s: a write role got a read-only mount", role)
		}
		if plan.WorkName != "deleg-7" {
			t.Errorf("%s: work name = %q", role, plan.WorkName)
		}
	}
}

// Read-only is enforced by the mount; it does not make sharing another
// session's checkout safe.
func TestPlanWorktreeReadOnlyRoleGetsDedicatedTree(t *testing.T) {
	for _, role := range []string{"review", "validate", "diagnose", "summarize"} {
		plan, err := PlanWorktree(false, "deleg-7")
		if err != nil {
			t.Fatalf("%s: %v", role, err)
		}
		if !plan.Isolated {
			t.Errorf("%s: a read-only role did not get its own worktree", role)
		}
		if !plan.ReadOnlyMount {
			t.Errorf("%s: a read-only role did not require a read-only mount", role)
		}
		if plan.WorkName != "deleg-7" {
			t.Errorf("%s: work name = %q", role, plan.WorkName)
		}
	}
}

// The name reaches a branch name and a mount path, so a slash would nest a
// namespace, a space or a quote would split an argument, and a leading dash
// would read as a flag.
func TestPlanWorktreeRejectsUnsafeDelegateIDs(t *testing.T) {
	hostile := []string{
		"", "  ", "-flag", "has space", "has/slash", "has'quote", `has"quote`,
		"has;semi", "$(id)", "has..dots", "has\nnewline",
	}
	for _, id := range hostile {
		if _, err := PlanWorktree(true, id); err == nil {
			t.Errorf("accepted unsafe delegate id %q", id)
		}
	}
	for _, id := range []string{"deleg7", "deleg-7", "deleg_7", "a", "D7"} {
		if _, err := PlanWorktree(true, id); err != nil {
			t.Errorf("rejected safe delegate id %q: %v", id, err)
		}
	}
}

func TestPlanWorktreeReadOnlyRejectsUnsafeDelegateID(t *testing.T) {
	if _, err := PlanWorktree(false, "$(id); rm -rf /"); err == nil {
		t.Fatal("a read-only worktree accepted an unsafe id")
	}
}

// The plan's read-only decision and the sandbox's mount modes are the same
// decision, and must not drift apart.
func TestSandboxRequestForMatchesThePlan(t *testing.T) {
	writePlan, _ := PlanWorktree(true, "deleg-7")
	req := SandboxRequestFor(writePlan, "/srv/repo",
		"/srv/repo/.aimee/worktrees/w1/main", "/srv/repo/.git/worktrees/w1", true,
		"/run/aimee/aimee-http.sock", "/run/aimee/aimee-http.sock", "")
	spec, err := BuildSandboxSpec(req)
	if err != nil {
		t.Fatalf("write spec: %v", err)
	}
	if spec.ReadOnly {
		t.Error("a write plan produced a read-only sandbox")
	}
	if _, ok := findMount(spec, "/srv/repo/.git/worktrees/w1"); !ok {
		t.Error("an isolated delegate did not get its git dir")
	}

	readPlan, _ := PlanWorktree(false, "deleg-8")
	readWT := "/srv/repo/.aimee/worktrees/w2/main"
	req = SandboxRequestFor(readPlan, "/srv/repo", readWT, "", true,
		"/run/aimee/aimee-http.sock", "/run/aimee/aimee-http.sock", "")
	spec, err = BuildSandboxSpec(req)
	if err != nil {
		t.Fatalf("read spec: %v", err)
	}
	if !spec.ReadOnly {
		t.Error("a read-only plan produced a writable sandbox")
	}
	if _, ok := findMount(spec, readWT); !ok {
		t.Error("a read-only delegate did not get its dedicated worktree mount")
	}
	for _, m := range spec.Mounts {
		if m.Kind == SandboxWorkspace && !m.ReadOnly {
			t.Errorf("read-only plan produced a writable workspace mount: %+v", m)
		}
	}
}

// A read-only delegate retains the returned git dir so the sandbox can mount
// its common object store without exposing the parent checkout.
func TestSandboxRequestForReadOnlyRetainsTheGitDir(t *testing.T) {
	readPlan, _ := PlanWorktree(false, "deleg-8")
	req := SandboxRequestFor(readPlan, "/srv/repo", "/srv/repo",
		"/srv/repo/.git", true, "", "", "")
	if req.GitDir != "/srv/repo/.git" {
		t.Errorf("git dir = %q, want the workspace-provided git identity", req.GitDir)
	}
}
