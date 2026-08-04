package engine

import (
	"os"
	"testing"
)

// TestMain gives this package a commit identity.
//
// These tests are hermetic in every other respect -- their own bare repos,
// forges and stub agents -- but gitIdentityArgs() reads AIMEE_GIT_AUTHOR_NAME
// and AIMEE_GIT_AUTHOR_EMAIL from the ambient environment, and several tests
// commit. Without them the failures were:
//
//	TestShippedWorkflowDefinitionsRunWithNativeEngine/build-triggered -- the
//	  slice child commits on its own branch at `impl`, so it parked
//	  "runner_unavailable: no git identity configured" and retried until the
//	  25s deadline
//	TestCommitChangesDropsCoreDumpAndRejectsGiantBlob -- same message, directly
//	TestRefreshPullRequestBaseUsesCurrentRemoteTipBeforeHandoff -- git itself
//	  refused the merge with "Committer identity unknown"
//	TestNativeSchedulerDrivesConfiguredBuildThroughSliceToFinalPR -- the slice
//	  path again
//
// So the package passed only on machines that happened to export those two
// variables and failed for everyone else. Setting them here makes the result
// depend on the code under test rather than on the developer's shell. Values
// are deliberately non-routable; nothing here reaches a real remote.
func TestMain(m *testing.M) {
	if os.Getenv("AIMEE_GIT_AUTHOR_NAME") == "" {
		os.Setenv("AIMEE_GIT_AUTHOR_NAME", "aimee-test")
	}
	if os.Getenv("AIMEE_GIT_AUTHOR_EMAIL") == "" {
		os.Setenv("AIMEE_GIT_AUTHOR_EMAIL", "aimee-test@example.invalid")
	}
	// git itself also needs an identity for merges it performs directly.
	if os.Getenv("GIT_AUTHOR_NAME") == "" {
		os.Setenv("GIT_AUTHOR_NAME", "aimee-test")
		os.Setenv("GIT_AUTHOR_EMAIL", "aimee-test@example.invalid")
		os.Setenv("GIT_COMMITTER_NAME", "aimee-test")
		os.Setenv("GIT_COMMITTER_EMAIL", "aimee-test@example.invalid")
	}
	os.Exit(m.Run())
}
