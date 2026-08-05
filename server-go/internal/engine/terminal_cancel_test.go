package engine

import (
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/modules/delegates/plane"

	_ "modernc.org/sqlite"
)

func TestCancelTerminalJobsRecoversCommitToCancelCrashWindow(t *testing.T) {
	dbPath := filepath.Join(t.TempDir(), "db.sqlite")
	store, err := db1.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	const workItemID = "wi_crash_recovery"
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{ID: workItemID, Repo: "repo", ProposalPath: workItemID, WorkflowName: "build", StartStage: "impl"}); err != nil {
		t.Fatal(err)
	}
	request := DelegateRequest{Role: "code", Persona: "engineer", Prompt: "implement", WorkItemID: workItemID, Stage: "impl", ExecutionVersion: "v1"}
	key := plane.DelegateJobKey(request)
	if err := store.SaveWorkflowDelegateJob(t.Context(), key, workItemID, 77, "participant-77"); err != nil {
		t.Fatal(err)
	}
	const completedID = "wi_completed_mapping"
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{ID: completedID, Repo: "repo", ProposalPath: completedID, WorkflowName: "build", StartStage: "impl"}); err != nil {
		t.Fatal(err)
	}
	completedKey := completedID + ":impl:v1:hash"
	if err := store.SaveWorkflowDelegateJob(t.Context(), completedKey, completedID, 78, "participant-78"); err != nil {
		t.Fatal(err)
	}
	rawDB, err := sql.Open("sqlite", "file:"+dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer rawDB.Close()
	if _, err := rawDB.ExecContext(t.Context(), `CREATE TABLE IF NOT EXISTS agent_jobs (id INTEGER PRIMARY KEY,status TEXT NOT NULL); INSERT INTO agent_jobs(id,status) VALUES(77,'running'),(78,'done')`); err != nil {
		t.Fatal(err)
	}
	// Model a server failure immediately after the terminal lifecycle commit:
	// the durable delegate mapping remains and no in-memory cancel callback ran.
	if _, err := store.StopTree(t.Context(), workItemID); err != nil {
		t.Fatal(err)
	}
	if _, err := store.StopTree(t.Context(), completedID); err != nil {
		t.Fatal(err)
	}
	var cancelledJob atomic.Int32
	// The terminal sweep still reads the Go lifecycle tables, but releasing the
	// reservation is now the plane's, so the release has to be observed there.
	var released sync.Map
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/jobs/cancel":
			var body struct {
				JobID int `json:"job_id"`
			}
			_ = json.NewDecoder(r.Body).Decode(&body)
			cancelledJob.Store(int32(body.JobID))
			_ = json.NewEncoder(w).Encode(map[string]any{"cancelled": true})
		case "/v1/delegate/reservation/forget":
			var body struct {
				ExecutionKey string `json:"execution_key"`
				JobID        int    `json:"job_id"`
			}
			_ = json.NewDecoder(r.Body).Decode(&body)
			released.Store(body.ExecutionKey, body.JobID)
			_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok", "released": true})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	client, err := NewHTTPAgentClient(AgentHTTPConfig{BaseURL: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	cancelled, err := NewTerminalJobCanceller(store, client).CancelTerminalJobs(t.Context())
	if err != nil || cancelled != 1 || cancelledJob.Load() != 77 {
		t.Fatalf("cancelled=%d job=%d err=%v", cancelled, cancelledJob.Load(), err)
	}
	// The release names the job it cancelled, so a retry that has already
	// reserved a newer job under the same key survives the compare-delete.
	if jobID, ok := released.Load(key); !ok || jobID != 77 {
		t.Fatalf("acknowledged terminal reservation was not released: job=%v held=%v", jobID, ok)
	}
	if _, ok := released.Load(completedKey); ok {
		t.Fatal("completed mapping was incorrectly released")
	}

}
