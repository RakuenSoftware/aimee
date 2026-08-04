package engine

import (
	"os"
	"testing"
)

// The production runner deliberately fails closed when its sealed Git identity
// is absent. Engine tests create throwaway repositories and need a deterministic
// identity of their own instead of depending on a developer's shell or global
// gitconfig.
func TestMain(m *testing.M) {
	if os.Getenv("AIMEE_GIT_AUTHOR_NAME") == "" {
		_ = os.Setenv("AIMEE_GIT_AUTHOR_NAME", "Aimee Test")
	}
	if os.Getenv("AIMEE_GIT_AUTHOR_EMAIL") == "" {
		_ = os.Setenv("AIMEE_GIT_AUTHOR_EMAIL", "aimee-test@example.invalid")
	}
	os.Exit(m.Run())
}
