package main

import (
	"context"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/aimee"
)

// The directory SELECTION, which had no test at all until a peer's method found
// it.
//
// They asked how to detect a test passing for a reason outside the repository,
// and answered it by taking the reason away: build a database holding nothing
// and re-run. The environment is this module's version of that, so the suite was
// run with AIMEE_PEER_DIRECTORY=db1 set to see whether anything moved. Nothing
// did — and the reason was not that the code is env-independent. It was that NO
// TEST REACHES aimeeDirectory. The experiment could not have failed, which is
// the shape it was run to look for, arriving one level over.
//
// The bus-attaching branches need a bus and stay hardware-tested; the DECISION
// does not, and the decision is what silently inverts. A flipped default would
// still print a confident line, and the line is an operator's only signal.
func TestDirectorySelectionAndItsDescription(t *testing.T) {
	t.Run("unset means none, and says how to ask", func(t *testing.T) {
		t.Setenv("AIMEE_PEER_DIRECTORY", "")
		dir, description := aimeeDirectory(context.Background(), "/nonexistent.sock")
		if _, ok := dir.(aimee.NoDirectory); !ok {
			t.Fatalf("dir = %T; want NoDirectory when the variable is unset", dir)
		}
		if !strings.Contains(description, "AIMEE_PEER_DIRECTORY=db1") {
			t.Errorf("description = %q; it is the operator's only signal, so it must "+
				"name how to ask for a directory", description)
		}
	})

	t.Run("an unrecognised value means none, not db1", func(t *testing.T) {
		// Fails CLOSED. A typo must not select the directory, and must not be
		// treated as the empty case either -- both would be a configuration the
		// operator did not write.
		t.Setenv("AIMEE_PEER_DIRECTORY", "db-1")
		dir, _ := aimeeDirectory(context.Background(), "/nonexistent.sock")
		if _, ok := dir.(aimee.NoDirectory); !ok {
			t.Fatalf("dir = %T; want NoDirectory for an unrecognised value", dir)
		}
	})

	t.Run("db1 without a socket says WHY it is none", func(t *testing.T) {
		t.Setenv("AIMEE_PEER_DIRECTORY", "db1")
		dir, description := aimeeDirectory(context.Background(), "")
		if _, ok := dir.(aimee.NoDirectory); !ok {
			t.Fatalf("dir = %T; want NoDirectory with no bus socket", dir)
		}
		// The distinction that matters: a run that MEANT to use db1 and could
		// not must not read the same as one that never asked.
		if !strings.Contains(description, "db1 was asked for") {
			t.Errorf("description = %q; a run that asked for db1 and did not get it "+
				"must not look like one that never asked", description)
		}
	})

	t.Run("db1 with an unreachable socket reports the attach failure", func(t *testing.T) {
		t.Setenv("AIMEE_PEER_DIRECTORY", "db1")
		dir, description := aimeeDirectory(context.Background(), "/nonexistent/bus.sock")
		if _, ok := dir.(aimee.NoDirectory); !ok {
			t.Fatalf("dir = %T; want NoDirectory when the attach fails", dir)
		}
		// Reported, not silently downgraded. This exact string appeared on
		// hardware before the grants were right -- "none: could not attach as
		// principal 67: no such file or directory" -- and it is what made the
		// cause findable instead of leaving a module that quietly answered
		// no_directory for reasons nobody could see.
		if !strings.Contains(description, "could not attach as principal") {
			t.Errorf("description = %q; want the attach failure named", description)
		}
	})
}
