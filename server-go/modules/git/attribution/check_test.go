package attribution

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"testing"
)

const base = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
const head = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

func encoded(value any) []byte { b, _ := json.Marshal(value); return b }
func issue(number int, app any) map[string]any {
	return map[string]any{"number": number, "title": "Change", "body": "Details", "performed_via_github_app": app, "pull_request": map[string]any{}}
}
func humanCommit(sha string) map[string]any {
	who := map[string]any{"name": "Operator", "email": "operator@example.com"}
	return map[string]any{"sha": sha, "commit": map[string]any{"message": "Change", "author": who, "committer": who}}
}
func fixture() map[string][]byte {
	return map[string][]byte{
		"pulls/3054":  encoded(map[string]any{"number": 3054, "base": map[string]string{"sha": base}, "head": map[string]string{"sha": head}}),
		"issues/3054": encoded(issue(3054, nil)),
		"compare/" + base + "..." + head + "?per_page=100&page=1":                    encoded(map[string]any{"total_commits": 1, "commits": []any{humanCommit(head)}}),
		"commits/" + head + "/pulls?per_page=100&page=1":                             []byte(`[{"number":3054}]`),
		"commits/" + head + "/check-runs?filter=all&per_page=100&page=1":             []byte(`{"total_count":0,"check_runs":[]}`),
		"issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page=1": encoded([]any{issue(3054, nil)}),
	}
}
func checker(t *testing.T, data map[string][]byte) *Checker {
	t.Helper()
	return &Checker{Get: func(_ context.Context, path string) ([]byte, error) {
		if b, ok := data[path]; ok {
			return b, nil
		}
		return nil, fmt.Errorf("unexpected/unavailable API endpoint: %s", path)
	}}
}

func TestCleanAdmissionPinsFullHead(t *testing.T) {
	got, err := checker(t, fixture()).CheckPull(context.Background(), 3054, "")
	if got != head || err != nil {
		t.Fatalf("got %s, %v", got, err)
	}
}

func TestEmptyCommitAndReplacementCannotEraseClosedOriginal(t *testing.T) {
	data := fixture()
	ancestor := "df8b24de887491a0c0d7882f7eb5bf1dd990eee8"
	data["compare/"+base+"..."+head+"?per_page=100&page=1"] = encoded(map[string]any{"total_commits": 2, "commits": []any{humanCommit(ancestor), humanCommit(head)}})
	// Model the actual API after #3054 merged: the original #3051 is omitted.
	data["commits/"+ancestor+"/pulls?per_page=100&page=1"] = []byte(`[{"number":3054}]`)
	data["commits/"+ancestor+"/check-runs?filter=all&per_page=100&page=1"] = []byte(`{"total_count":0,"check_runs":[]}`)
	data["issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page=1"] = encoded([]any{issue(3051, map[string]any{"id": 1236702, "slug": "claude"}), issue(3054, nil)})
	data["pulls/3051"] = []byte(`{"number":3051,"commits":1,"merge_commit_sha":null,"base":{"sha":"cccccccccccccccccccccccccccccccccccccccc"},"head":{"sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}`)
	data["compare/"+strings.Repeat("c", 40)+"..."+base+"?per_page=100&page=1"] = encoded(map[string]any{"total_commits": 1, "commits": []any{humanCommit(ancestor)}})
	_, err := checker(t, data).CheckPull(context.Background(), 3054, head)
	if err == nil || !strings.Contains(err.Error(), "rejected PR #3051") {
		t.Fatalf("replacement escaped: %v", err)
	}
}

func TestOldFailureOnAncestorSurvivesSuccessOnReplacement(t *testing.T) {
	data := fixture()
	runs := []any{}
	for i := 1; i <= 100; i++ {
		runs = append(runs, map[string]any{"id": i, "name": "no-coauthor-trailers", "status": "completed", "conclusion": "success"})
	}
	key := "commits/" + head + "/check-runs?filter=all&per_page=100&page="
	data[key+"1"] = encoded(map[string]any{"total_count": 101, "check_runs": runs})
	data[key+"2"] = []byte(`{"total_count":101,"check_runs":[{"id":101,"name":"no-coauthor-trailers","status":"completed","conclusion":"failure"}]}`)
	_, err := checker(t, data).CheckPull(context.Background(), 3054, head)
	if err == nil || !strings.Contains(err.Error(), "recorded attribution rejection") {
		t.Fatalf("failure was laundered: %v", err)
	}
}

func TestNoHistoricalSHAOrIntegrationExemption(t *testing.T) {
	for _, sha := range []string{head, "9f327867cd62a14e50e9a41af9b871fc7792e4bd", "003255217a10a8559e47bf0b6fedeb2c0faa6cce"} {
		data := fixture()
		entry := humanCommit(sha)
		entry["commit"].(map[string]any)["message"] = "Change\n\nCo-authored-by: Anyone <anyone@example.com>"
		data["compare/"+base+"..."+head+"?per_page=100&page=1"] = encoded(map[string]any{"total_commits": 1, "commits": []any{entry}})
		if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil {
			t.Fatalf("exempted %s", sha)
		}
	}
}

func TestIssueMetadataFailsClosedAndRejectsAllAttributionForms(t *testing.T) {
	for _, app := range []any{map[string]any{"id": 1236702, "slug": "renamed"}, map[string]any{"id": 1, "slug": "claude-code"}, map[string]any{"id": 2, "slug": "unknown-new-app"}, "claude", map[string]any{"slug": "claude"}} {
		if err := CheckIssue(encoded(issue(1, app)), 1); err == nil {
			t.Fatalf("accepted app %v", app)
		}
	}
	for _, body := range []string{"Co-Authored-By: A <a@a>", " > **Co-authored-by: A <a@a>", "_Generated by [Claude Code](https://example.com)_", "Generated by Codex", "Codex-Session: https://example.com/s", "https://example.com/code/session_1", "🤖 Generated with [Claude Code](https://claude.com/claude-code)", "- Generated with Claude Code", "See https://claude.ai/code/session_0123 for the run"} {
		entry := issue(1, nil)
		entry["body"] = body
		if err := CheckIssue(encoded(entry), 1); err == nil {
			t.Fatalf("accepted %s", body)
		}
	}
	for _, field := range []string{"title", "body", "performed_via_github_app"} {
		entry := issue(1, nil)
		delete(entry, field)
		if err := CheckIssue(encoded(entry), 1); err == nil {
			t.Fatalf("accepted missing %s", field)
		}
	}
	entry := issue(1, nil)
	entry["body"] = "Reject Generated by [Claude Code] footers.\nhttps://claude.ai/docs"
	if err := CheckIssue(encoded(entry), 1); err != nil {
		t.Fatal(err)
	}
	entry["body"] = nil
	if err := CheckIssue(encoded(entry), 1); err != nil {
		t.Fatal(err)
	}
}

func TestEveryEvidenceReadIsRequired(t *testing.T) {
	for path := range fixture() {
		t.Run(path, func(t *testing.T) {
			data := fixture()
			delete(data, path)
			if _, err := checker(t, data).CheckPull(context.Background(), 3054, head); err == nil {
				t.Fatal("unavailable evidence passed")
			}
			data[path] = []byte(`null`)
			if _, err := checker(t, data).CheckPull(context.Background(), 3054, head); err == nil {
				t.Fatal("null evidence passed")
			}
		})
	}
}

func TestRawAssistantIdentityAndMissingCommitFieldsAreRejected(t *testing.T) {
	for _, name := range []string{"Claude", "Codex", "copilot[bot]", "aimee-wfe", ""} {
		entry := humanCommit(head)
		entry["commit"].(map[string]any)["committer"] = map[string]any{"name": name, "email": "x@example.com"}
		var c commit
		_ = json.Unmarshal(encoded(entry), &c)
		if err := c.check(); err == nil {
			t.Fatalf("accepted committer %s", name)
		}
	}
}

func TestMovedHeadAndBaseAreRejected(t *testing.T) {
	data := fixture()
	c := checker(t, data)
	if _, err := c.CheckPull(context.Background(), 3054, base); err == nil {
		t.Fatal("accepted stale expected head")
	}
	c = checker(t, data)
	get := c.Get
	reads := 0
	c.Get = func(ctx context.Context, path string) ([]byte, error) {
		if path == "pulls/3054" {
			reads++
			if reads == 2 {
				return encoded(map[string]any{"number": 3054, "base": map[string]string{"sha": head}, "head": map[string]string{"sha": head}}), nil
			}
		}
		return get(ctx, path)
	}
	if _, err := c.CheckPull(context.Background(), 3054, head); err == nil {
		t.Fatal("accepted moved base")
	}
}

func TestPRTextIsRecheckedAfterProvenanceTraversal(t *testing.T) {
	c := checker(t, fixture())
	get := c.Get
	reads := 0
	c.Get = func(ctx context.Context, path string) ([]byte, error) {
		if path == "issues/3054" {
			reads++
			if reads == 2 {
				changed := issue(3054, nil)
				changed["body"] = "Co-authored-by: A <a@a>"
				return encoded(changed), nil
			}
		}
		return get(ctx, path)
	}
	if _, err := c.CheckPull(context.Background(), 3054, head); err == nil || !strings.Contains(err.Error(), "title/body") {
		t.Fatalf("late attribution edit passed: %v", err)
	}
}

func TestIncomingCommitPaginationCannotHideLastTrailer(t *testing.T) {
	data := fixture()
	entries := []any{}
	for i := 1; i <= 100; i++ {
		sha := fmt.Sprintf("%040x", i)
		entries = append(entries, humanCommit(sha))
		data["commits/"+sha+"/pulls?per_page=100&page=1"] = []byte(`[]`)
		data["commits/"+sha+"/check-runs?filter=all&per_page=100&page=1"] = []byte(`{"total_count":0,"check_runs":[]}`)
	}
	path := "compare/" + base + "..." + head + "?per_page=100&page="
	data[path+"1"] = encoded(map[string]any{"total_commits": 101, "commits": entries})
	bad := humanCommit(head)
	bad["commit"].(map[string]any)["message"] = "Fix\nCo-authored-by: A <a@a>"
	data[path+"2"] = encoded(map[string]any{"total_commits": 101, "commits": []any{bad}})
	if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil || !strings.Contains(err.Error(), "trailer") {
		t.Fatalf("last commit escaped: %v", err)
	}
	delete(data, path+"2")
	if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil {
		t.Fatal("missing commit page passed")
	}
}

func TestClosedOriginalOnLaterCatalogPageCannotBeHidden(t *testing.T) {
	data := fixture()
	issues := []any{}
	for i := 1; i <= 100; i++ {
		issues = append(issues, issue(i, nil))
	}
	path := "issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page="
	data[path+"1"] = encoded(issues)
	data[path+"2"] = encoded([]any{issue(3051, map[string]any{"id": 1236702, "slug": "claude"})})
	data["pulls/3051"] = []byte(`{"number":3051,"commits":1,"merge_commit_sha":null,"base":{"sha":"cccccccccccccccccccccccccccccccccccccccc"},"head":{"sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}`)
	data["compare/"+strings.Repeat("c", 40)+"..."+base+"?per_page=100&page=1"] = encoded(map[string]any{"total_commits": 1, "commits": []any{humanCommit(head)}})
	if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil || !strings.Contains(err.Error(), "rejected PR #3051") {
		t.Fatalf("later original escaped: %v", err)
	}
	delete(data, path+"2")
	if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil {
		t.Fatal("missing catalog page passed")
	}
}

func TestIncompleteRejectedPRInventoryIsNotPermissionToSkip(t *testing.T) {
	for _, reply := range []string{`{"number":3051}`, `{"number":3051,"commits":3000}`, `{"number":3051,"commits":2}`} {
		data := fixture()
		data["issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page=1"] = encoded([]any{issue(3051, map[string]any{"id": 1236702, "slug": "claude"})})
		data["pulls/3051"] = []byte(reply)
		data["pulls/3051/commits?per_page=100&page=1"] = []byte(`[]`)
		if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil {
			t.Fatalf("incomplete inventory passed: %s", reply)
		}
	}
}

func TestIntermediateSquashCannotEraseOriginalOpeningAttribution(t *testing.T) {
	data := fixture()
	data["issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page=1"] = encoded([]any{issue(3051, map[string]any{"id": 1236702, "slug": "claude"})})
	data["pulls/3051"] = encoded(map[string]any{"number": 3051, "commits": 1, "merge_commit_sha": head, "head": map[string]string{"sha": base}, "base": map[string]string{"sha": strings.Repeat("c", 40)}})
	if err := checker(t, data).CheckRange(context.Background(), base, head); err == nil || !strings.Contains(err.Error(), "merge result belongs to rejected PR #3051") {
		t.Fatalf("squashed provenance escaped: %v", err)
	}
}

func TestRejectedPRWith322CommitsUsesCompleteComparison(t *testing.T) {
	data := map[string][]byte{}
	originalBase, originalHead := strings.Repeat("c", 40), strings.Repeat("d", 40)
	data["pulls/2997"] = encoded(map[string]any{"number": 2997, "commits": 322, "merge_commit_sha": nil, "base": map[string]string{"sha": originalBase}, "head": map[string]string{"sha": originalHead}})
	path := "compare/" + originalBase + "..." + originalHead + "?per_page=100&page="
	for page := 1; page <= 4; page++ {
		entries := []any{}
		for i := (page-1)*100 + 1; i <= page*100 && i <= 322; i++ {
			entries = append(entries, map[string]string{"sha": fmt.Sprintf("%040x", i)})
		}
		data[path+fmt.Sprint(page)] = encoded(map[string]any{"total_commits": 322, "commits": entries})
	}
	reason := fmt.Errorf("opening app attribution")
	if err := checker(t, data).rejectedPull(context.Background(), 2997, map[string]bool{head: true}, reason); err != nil {
		t.Fatalf("complete unrelated inventory failed: %v", err)
	}
	data[path+"4"] = encoded(map[string]any{"total_commits": 322, "commits": append([]any{map[string]string{"sha": head}}, func() []any {
		entries := []any{}
		for i := 302; i <= 322; i++ {
			entries = append(entries, map[string]string{"sha": fmt.Sprintf("%040x", i)})
		}
		return entries
	}()...)})
	if err := checker(t, data).rejectedPull(context.Background(), 2997, map[string]bool{head: true}, reason); err == nil || !strings.Contains(err.Error(), "rejected PR #2997") {
		t.Fatalf("commit past 250 escaped: %v", err)
	}
}

func TestNewBranchUsesImmutableDefaultMergeBase(t *testing.T) {
	data := fixture()
	defaultHead := strings.Repeat("c", 40)
	data["commits/main"] = encoded(map[string]any{"sha": defaultHead})
	data["compare/"+defaultHead+"..."+head] = encoded(map[string]any{"merge_base_commit": map[string]string{"sha": base}})
	c := checker(t, data)
	got, err := c.PushBase(context.Background(), strings.Repeat("0", 40), head, "main", "refs/heads/new", true)
	if err != nil || got != base {
		t.Fatalf("base=%q error=%v", got, err)
	}
	if err := c.CheckRange(context.Background(), got, head); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		before, ref string
		created     bool
	}{{"bad", "refs/heads/new", true}, {strings.Repeat("0", 40), "refs/heads/main", true}, {strings.Repeat("0", 40), "refs/heads/new", false}, {strings.Repeat("0", 40), "", true}, {strings.Repeat("0", 40), "refs/tags/new", true}} {
		if _, err := c.PushBase(context.Background(), tc.before, head, "main", tc.ref, tc.created); err == nil {
			t.Fatal("unverifiable creation admitted", tc)
		}
	}
	got, err = c.PushBase(context.Background(), base, head, "", "", false)
	if err != nil || got != base {
		t.Fatalf("ordinary push changed: %q %v", got, err)
	}
	data["commits/main"] = []byte(`{"sha":"main"}`)
	if _, err := c.PushBase(context.Background(), strings.Repeat("0", 40), head, "main", "refs/heads/new", true); err == nil {
		t.Fatal("mutable default ref accepted")
	}
}
func executionFixture() map[string][]byte {
	data := fixture()
	data["commits/"+head+"/check-runs?filter=all&per_page=100&page=1"] = []byte(`{"total_count":1,"check_runs":[{"id":77,"name":"no-coauthor-trailers","status":"completed","conclusion":"failure"}]}`)
	data["check-runs/77"] = encoded(map[string]any{"id": 77, "head_sha": head, "name": "no-coauthor-trailers", "app": map[string]string{"slug": "github-actions"}})
	data["actions/jobs/77"] = encoded(map[string]any{"id": 77, "run_id": 88, "run_attempt": 1, "head_sha": head, "check_run_url": "https://api.github.com/repos/owner/repo/check-runs/77", "name": "no-coauthor-trailers", "status": "completed", "conclusion": "failure"})
	data["actions/runs/88"] = encoded(map[string]any{"id": 88, "head_sha": head, "event": "push", "path": ".github/workflows/attribution.yml", "repository": map[string]string{"full_name": "owner/repo"}})
	data["check-runs/77/annotations?per_page=100&page=1"] = []byte(`[]`)
	data["actions/jobs/77/logs"] = []byte("2026-10-27T19:23:01.8923689Z attribution denied after 0s : attribution requires full nonzero base/head SHAs\n")
	return data
}
func TestHistoricalInputFailureIsNotContentDenial(t *testing.T) {
	data := executionFixture()
	c := checker(t, data)
	c.Repository = "owner/repo"
	if err := c.CheckRange(context.Background(), base, head); err != nil {
		t.Fatal(err)
	}
	data["actions/jobs/77/logs"] = []byte("2026-10-26T16:29:16.5758459Z attribution denied after 14.557s : PR #3030 carries GitHub app attribution: chatgpt-codex-connector\n")
	if err := c.CheckRange(context.Background(), base, head); err == nil {
		t.Fatal("actual historical app denial erased")
	}
}
func TestHistoricalExecutionEvidenceFailsClosed(t *testing.T) {
	for _, key := range []string{"check-runs/77", "actions/jobs/77", "actions/runs/88", "check-runs/77/annotations?per_page=100&page=1", "actions/jobs/77/logs"} {
		t.Run(key, func(t *testing.T) {
			data := executionFixture()
			data[key] = []byte(`{}`)
			c := checker(t, data)
			c.Repository = "owner/repo"
			if err := c.CheckRange(context.Background(), base, head); err == nil {
				t.Fatal("incomplete evidence ignored")
			}
		})
	}
	for _, change := range []struct {
		key, field string
		value      any
	}{{"actions/jobs/77", "head_sha", base}, {"actions/jobs/77", "check_run_url", "https://api.github.com/repos/other/repo/check-runs/77"}, {"actions/runs/88", "event", "pull_request"}, {"check-runs/77", "app", map[string]string{"slug": "other"}}} {
		data := executionFixture()
		var obj map[string]any
		json.Unmarshal(data[change.key], &obj)
		obj[change.field] = change.value
		data[change.key] = encoded(obj)
		c := checker(t, data)
		c.Repository = "owner/repo"
		if err := c.CheckRange(context.Background(), base, head); err == nil {
			t.Fatal("mismatched identity ignored", change)
		}
	}
}
func TestVersionedExecutionVerdictRequiresExactBinding(t *testing.T) {
	v := Verdict{Version: 1, Repository: "owner/repo", HeadSHA: head, BaseSHA: base, RunID: 88, RunAttempt: 1, Event: "push", Outcome: "execution_error", Code: "api_read"}
	for _, mutate := range []func(*Verdict){func(*Verdict) {}, func(v *Verdict) { v.HeadSHA = base }, func(v *Verdict) { v.RunID = 99 }, func(v *Verdict) { v.Outcome = "denied" }, func(v *Verdict) { v.Code = "unknown" }} {
		data := executionFixture()
		copy := v
		mutate(&copy)
		data["check-runs/77/annotations?per_page=100&page=1"] = encoded([]any{map[string]any{"title": VerdictTitle, "message": string(encoded(copy))}})
		delete(data, "actions/jobs/77/logs")
		c := checker(t, data)
		c.Repository = "owner/repo"
		err := c.CheckRange(context.Background(), base, head)
		if (err == nil) != (copy == v) {
			t.Fatalf("verdict=%+v error=%v", copy, err)
		}
	}
}

func TestExplicitHistoricalSubjectAlreadyInAcceptedBase(t *testing.T) {
	data := executionFixture()
	oldHead := strings.Repeat("c", 40)
	merge := strings.Repeat("d", 40)
	oldBase := strings.Repeat("e", 40)
	data["actions/jobs/77/logs"] = []byte("2026-10-26T16:29:16.5758459Z attribution denied after 14.557s : PR #3030 carries GitHub app attribution: chatgpt-codex-connector\n")
	data["issues/3030"] = encoded(issue(3030, map[string]any{"id": 123, "slug": "chatgpt-codex-connector"}))
	data["pulls/3030"] = encoded(map[string]any{"number": 3030, "merged": true, "state": "closed", "commits": 1, "merge_commit_sha": merge, "base": map[string]string{"sha": oldBase}, "head": map[string]string{"sha": oldHead}})
	data["issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page=1"] = encoded([]any{issue(3054, nil), issue(3030, map[string]any{"id": 123, "slug": "chatgpt-codex-connector"})})
	data["compare/"+oldBase+"..."+oldHead+"?per_page=100&page=1"] = encoded(map[string]any{"total_commits": 1, "commits": []any{map[string]string{"sha": oldHead}}})
	keys := []string{}
	for _, pair := range [][2]string{{oldHead, merge}, {merge, head}, {oldHead, base}, {merge, base}} {
		key := "compare/" + pair[0] + "..." + pair[1]
		keys = append(keys, key)
		data[key] = encoded(map[string]any{"status": "ahead", "base_commit": map[string]string{"sha": pair[0]}, "merge_base_commit": map[string]string{"sha": pair[0]}})
	}
	c := checker(t, data)
	c.Repository = "owner/repo"
	if err := c.CheckRange(context.Background(), base, head); err != nil {
		t.Fatal("clean new range still tainted", err)
	}
	// Preserve the actual denial; it was not relabelled a policy success.
	if err := CheckIssue(data["issues/3030"], 3030); err == nil {
		t.Fatal("original denial erased")
	}
	for _, key := range keys {
		saved := data[key]
		data[key] = []byte(`{"status":"diverged"}`)
		if err := c.CheckRange(context.Background(), base, head); err == nil {
			t.Fatal("incomplete ancestry admitted", key)
		}
		data[key] = saved
	}
	// Moving the denied subject into the current range must still be rejected.
	data["compare/"+base+"..."+head+"?per_page=100&page=1"] = encoded(map[string]any{"total_commits": 2, "commits": []any{humanCommit(oldHead), humanCommit(head)}})
	data["commits/"+oldHead+"/pulls?per_page=100&page=1"] = []byte(`[{"number":3030}]`)
	if err := c.CheckRange(context.Background(), base, head); err == nil {
		t.Fatal("actual incoming denied subject admitted")
	}
}
func TestVerdictRejectsAmbiguousSchema(t *testing.T) {
	v := Verdict{Version: 1, Repository: "owner/repo", HeadSHA: head, BaseSHA: base, RunID: 88, RunAttempt: 1, Event: "push", Outcome: "execution_error", Code: "api_read"}
	good := string(encoded(v))
	for _, bad := range []string{strings.TrimSuffix(good, "}") + `,"code":"api_read"}`, strings.TrimSuffix(good, "}") + `,"unknown":true}`, good + good, `{"version":1}`, strings.Replace(good, `"run_id":88`, `"run_id":"88"`, 1)} {
		if decodeVerdict(bad, new(Verdict)) {
			t.Fatal("ambiguous verdict accepted", bad)
		}
	}
}

func TestHistoricalEvidenceTransportErrorIsNotContentVerdict(t *testing.T) {
	data := executionFixture()
	c := checker(t, data)
	c.Repository = "owner/repo"
	read := c.Get
	c.Get = func(ctx context.Context, path string) ([]byte, error) {
		if path == "check-runs/77" {
			return nil, &ExecutionError{Code: "api_read", Message: "temporary unavailable"}
		}
		return read(ctx, path)
	}
	err := c.CheckRange(context.Background(), base, head)
	var execution *ExecutionError
	if err == nil || !errors.As(err, &execution) || execution.Code != "api_read" {
		t.Fatalf("evidence read failure tainted content: %v", err)
	}
}

func TestRecordedFailureChainRetainsOriginalSubjectAndRejectsCycles(t *testing.T) {
	data := executionFixture()
	for _, pair := range [][2]string{{"check-runs/77", "check-runs/66"}, {"actions/jobs/77", "actions/jobs/66"}, {"actions/runs/88", "actions/runs/89"}} {
		data[pair[1]] = []byte(strings.ReplaceAll(strings.ReplaceAll(string(data[pair[0]]), "77", "66"), "88", "89"))
	}
	data["check-runs/66/annotations?per_page=100&page=1"] = []byte(`[]`)
	data["actions/jobs/66/logs"] = data["actions/jobs/77/logs"]
	data["actions/jobs/77/logs"] = []byte("2026-10-27T19:24:42.5655881Z attribution denied after 1.927s : commit " + head + " has a recorded attribution rejection (check 66)\n")
	data["commits/"+head+"/check-runs?filter=all&per_page=100&page=1"] = []byte(`{"total_count":2,"check_runs":[{"id":77,"name":"no-coauthor-trailers","status":"completed","conclusion":"failure"},{"id":66,"name":"no-coauthor-trailers","status":"completed","conclusion":"failure"}]}`)
	c := checker(t, data)
	c.Repository = "owner/repo"
	if err := c.CheckRange(context.Background(), base, head); err != nil {
		t.Fatal("input failure cascade became content deny", err)
	}
	data["actions/jobs/66/logs"] = []byte("2026-10-27T19:24:42.5655881Z attribution denied after 1.927s : commit " + head + " has a recorded attribution rejection (check 77)\n")
	if err := c.CheckRange(context.Background(), base, head); err == nil {
		t.Fatal("evidence cycle admitted")
	}
	data["actions/jobs/66/logs"] = []byte("2026-10-26T16:29:16.5758459Z attribution denied after 14.557s : PR #3030 carries GitHub app attribution: chatgpt-codex-connector\n")
	if err := c.CheckRange(context.Background(), base, head); err == nil {
		t.Fatal("genuine unknown-scope rejection erased by cascade")
	}
}

func TestConcurrentEvidenceNeverRelabelsObservedDenialAsExecution(t *testing.T) {
	transient := &ExecutionError{Code: "api_read", Message: "temporary unavailable"}
	denied := fmt.Errorf("incoming commit belongs to rejected PR #3030")
	for _, observed := range [][]error{{transient, denied}, {denied, transient}} {
		failures := make(chan error, len(observed))
		for _, err := range observed {
			failures <- err
		}
		close(failures)
		if got := catalogFailure(failures); got != denied {
			t.Fatalf("observed violation was hidden by execution race: %v", got)
		}
	}
	failures := make(chan error, 1)
	failures <- transient
	close(failures)
	if got := catalogFailure(failures); got != transient {
		t.Fatalf("execution-only failure changed: %v", got)
	}
}
