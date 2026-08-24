package aimee

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// fakeDirectoryCaller answers one canned reply, and records what it was asked.
//
// Named for the directory rather than sharing the store client tests' fakeCaller:
// the two absorbed into one package with the same name and different shapes,
// and this one records the decoded op and cells the directory sent.
type fakeDirectoryCaller struct {
	reply []byte
	err   error

	gotKind  uint32
	gotStage uint32
	gotOp    uint32
	gotCells []string
}

func (f *fakeDirectoryCaller) Call(_ context.Context, kind, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	f.gotKind, f.gotStage = kind, stage
	f.gotOp, f.gotCells, _ = peerwire.DecodeRequest(request)
	return f.reply, f.err
}

func db1Reply(t *testing.T, status uint32, cells []string) []byte {
	t.Helper()
	// Built with EncodeResponse purely as a frame builder; the status word here
	// is db1's enum, which is why the code under test decodes it raw.
	frame, err := peerwire.EncodeResponse(peerwire.Status(status), cells)
	if err != nil {
		t.Fatalf("build reply: %v", err)
	}
	return frame
}

func tenCells(principal string) []string {
	cells := make([]string, db1ReplyWidth)
	cells[0] = "sess-1"
	cells[1] = "cli"
	cells[db1ReplyPrincipal] = principal
	return cells
}

// The request is addressed by the FORMULA, and the formula must agree with what
// db1 actually declares. Transcribing 11782 here would pass while pointing at
// nothing if db1's ref or stage ever moved.
func TestDB1DirectoryAddressesTheDeclaredStage(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "modules", "process-contracts.json"))
	if err != nil {
		t.Fatalf("read contracts: %v", err)
	}
	var doc struct {
		Components []struct {
			ID           string `json:"id"`
			PrincipalRef uint32 `json:"principal_ref"`
			Stages       []struct {
				ID        uint32 `json:"id"`
				Name      string `json:"name"`
				EventKind uint32 `json:"event_kind"`
			} `json:"stages"`
		} `json:"components"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("parse contracts: %v", err)
	}

	// The component is `aimee` and the stage is `aimee-sessions`: db1 absorbed
	// into this module, so the session family this directory reads is now
	// served by the same principal that serves the directory's caller. The ref
	// and stage id did not move -- that was the point of keeping ref 30 through
	// the rename -- so what this test pins is unchanged; only the names it
	// looks them up by are.
	var found bool
	for _, c := range doc.Components {
		if c.ID != "aimee" {
			continue
		}
		if c.PrincipalRef != DB1PrincipalRef {
			t.Errorf("aimee principal_ref = %d; this module addresses %d",
				c.PrincipalRef, DB1PrincipalRef)
		}
		for _, s := range c.Stages {
			if s.Name != "aimee-sessions" {
				continue
			}
			found = true
			if s.ID != DB1SessionsStage {
				t.Errorf("aimee-sessions stage id = %d; this module addresses %d",
					s.ID, DB1SessionsStage)
			}
			if got := peerwire.EventKind(DB1PrincipalRef, DB1SessionsStage); got != s.EventKind {
				t.Errorf("addressing kind %d; the contract declares %d", got, s.EventKind)
			}
		}
	}
	if !found {
		t.Fatal("no aimee-sessions stage is declared; the directory addresses a stage that is gone")
	}
}

// Each db1 outcome maps to the response a caller can act on, and the four are
// kept apart because the right action differs: use it, stop, fix the call, retry.
func TestDB1DirectoryMapsEveryOutcome(t *testing.T) {
	for _, tc := range []struct {
		name    string
		status  uint32
		cells   []string
		callErr error

		wantOwner string
		wantErr   error
	}{
		{name: "found", status: db1StatusOK, cells: tenCells("uid:1000"), wantOwner: "uid:1000"},
		{name: "absent", status: db1StatusMissing, wantErr: peer.ErrNoPeer},
		{name: "store failure", status: db1StatusFailed, wantErr: peer.ErrDirectoryUnavailable},
		{name: "refused as invalid", status: db1StatusInvalid, wantErr: ErrDirectoryRefused},
		{name: "refused as too long", status: db1StatusTooLong, wantErr: ErrDirectoryRefused},
		{
			name:   "unrecognised status is not absence",
			status: 99,
			// A store that grows a fifth outcome must not have it read as
			// "missing", which would report live sessions as gone.
			wantErr: peer.ErrDirectoryUnavailable,
		},
		{
			name:   "ok with the wrong cell count",
			status: db1StatusOK, cells: []string{"only", "three", "cells"},
			// Reading index 2 of a short reply is reading whatever is there.
			wantErr: peer.ErrDirectoryUnavailable,
		},
		{
			name:   "ok with an empty principal",
			status: db1StatusOK, cells: tenCells(""),
			// Held but unaddressable. Not absence: a caller told "no peer"
			// stops, and this row needs fixing instead.
			wantErr: peer.ErrDirectoryUnavailable,
		},
		{name: "transport failure", callErr: errors.New("bus down"), wantErr: peer.ErrDirectoryUnavailable},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &fakeDirectoryCaller{err: tc.callErr}
			if tc.callErr == nil {
				f.reply = db1Reply(t, tc.status, tc.cells)
			}
			d, err := NewDB1Directory(f, time.Second)
			if err != nil {
				t.Fatal(err)
			}

			owner, err := d.Owner("sess-1")
			if tc.wantErr != nil {
				if !errors.Is(err, tc.wantErr) {
					t.Fatalf("err = %v; want %v", err, tc.wantErr)
				}
				// The two that are easiest to confuse, kept explicitly apart.
				if !errors.Is(tc.wantErr, peer.ErrNoPeer) && errors.Is(err, peer.ErrNoPeer) {
					t.Error("reported as a missing session; a caller told that stops asking")
				}
				return
			}
			if err != nil || owner != tc.wantOwner {
				t.Fatalf("owner = %q, %v; want %q", owner, err, tc.wantOwner)
			}
		})
	}
}

// The request db1 receives is the one its catalog describes: op 2, one field.
func TestDB1DirectorySendsTheCatalogedRequest(t *testing.T) {
	f := &fakeDirectoryCaller{reply: db1Reply(t, db1StatusOK, tenCells("uid:1000"))}
	d, err := NewDB1Directory(f, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := d.Owner("sess-7"); err != nil {
		t.Fatal(err)
	}
	if f.gotOp != DB1OpServerSessionGet {
		t.Errorf("op = %d; want %d (server_session_get)", f.gotOp, DB1OpServerSessionGet)
	}
	if len(f.gotCells) != 1 || f.gotCells[0] != "sess-7" {
		t.Errorf("request cells = %q; want exactly the session id", f.gotCells)
	}
	if f.gotStage != DB1SessionsStage {
		t.Errorf("stage = %d; want %d", f.gotStage, DB1SessionsStage)
	}
}

// An empty id is refused here rather than sent. db1 answers Invalid for it, so
// asking spends a round trip to be told what is already known -- and the refusal
// must not read as absence.
func TestDB1DirectoryRefusesAnEmptyIDWithoutCalling(t *testing.T) {
	f := &fakeDirectoryCaller{reply: db1Reply(t, db1StatusMissing, nil)}
	d, err := NewDB1Directory(f, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	_, err = d.Owner("")
	if !errors.Is(err, ErrDirectoryRefused) {
		t.Fatalf("empty id = %v; want ErrDirectoryRefused", err)
	}
	if errors.Is(err, peer.ErrNoPeer) {
		t.Error("an empty id reported as a missing session")
	}
	if f.gotOp != 0 {
		t.Error("the call went out anyway")
	}
}

// db1's status word must never be read as this module's Status, and the type
// system cannot say so.
//
// peerwire.DecodeResponse(reply) compiles perfectly here and returns a
// peerwire.Status. db1's 1 is MISSING where this module's 1 is no_peer; its 4 is
// FAILED where this module's 4 is hop_limit. So the wrong decoder yields a value
// that reads as a sensible status, routes through a real case arm, and means
// something else entirely -- with no wrong-looking step anywhere for a reviewer
// to catch.
//
// The compiler stops the swap once the value is in hand, because uint32 and
// peerwire.Status will not compare. What it cannot express is "do not call that
// function from this file", which is the only move that produces the bad value
// in the first place. Hence a source check rather than a type.
//
// Comments are stripped first: this file's own explanation of the hazard names
// both, and a guard that fails on its own rationale teaches people to delete the
// rationale.
//
// THIS IS A SYNTAX CHECK AND IT IS SAFE ONLY BECAUSE OF ITS SCOPE. It bans two
// identifiers by name, which cannot distinguish a correct use from a wrong one
// -- and inside this ONE file there are no correct uses, so the two coincide.
// Widen it to the package and that stops being true immediately: peerwire.Status
// is the right type nearly everywhere else, and the check would report every
// legitimate use as a defect.
//
// A peer described a defect by its punctuation rather than its direction,
// scanned for it, and got 24 hits that were all correct usage. A scanner wrong
// about every hit is the same object as a guard that passes having checked
// nothing; it just fails loudly instead of quietly.
func TestDB1DirectoryNeverReadsDB1StatusAsOurOwn(t *testing.T) {
	src, err := os.ReadFile("db1directory.go")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	var code strings.Builder
	for _, line := range strings.Split(string(src), "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "//") {
			continue
		}
		code.WriteString(line)
		code.WriteString("\n")
	}
	for _, banned := range []string{"peerwire.DecodeResponse", "peerwire.Status"} {
		if strings.Contains(code.String(), banned) {
			t.Errorf("db1directory.go names %s outside a comment. db1's status enum "+
				"is not this module's: read it raw with peerwire.DecodeReply, or the "+
				"value reads as a sensible status and means something else.", banned)
		}
	}
	// The check is worth nothing if the thing it requires is absent.
	if !strings.Contains(code.String(), "peerwire.DecodeReply") {
		t.Error("db1directory.go no longer calls peerwire.DecodeReply; this guard is " +
			"now asserting the absence of something nothing needs")
	}
}

// A refusal survives the REGISTRY, which is where it was being lost.
//
// The mapping tests above check DB1Directory in isolation, and it was correct
// there the whole time. The defect lived one layer up: Registry.Owner wrapped
// anything that was not absence into ErrDirectoryUnavailable with %v, breaking
// the errors.Is chain, so db1 answering INVALID -- "I will not accept what you
// sent" -- reached the caller as `unavailable`, the one status that means retry.
// A permanent defect in a request this module built, reported as a transient
// condition, retried forever.
//
// Found by a peer's question: does each CLAUSE matter, and what does this one
// actually produce? Measured rather than reasoned about, because reading the
// isolated mapping says it is fine and reading the registry says it is fine, and
// only running the two together shows what a caller gets.
func TestARefusalSurvivesTheRegistryAsARefusal(t *testing.T) {
	r := peer.New(peer.Options{})
	d, err := NewDB1Directory(&fakeDirectoryCaller{reply: db1Reply(t, db1StatusInvalid, nil)}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	r.SetSessionOwner(d.Owner)

	_, ownerErr := r.Owner("sess-1")
	if !errors.Is(ownerErr, peer.ErrDirectoryRefused) {
		t.Fatalf("Registry.Owner = %v; want ErrDirectoryRefused to survive", ownerErr)
	}
	if errors.Is(ownerErr, peer.ErrDirectoryUnavailable) {
		t.Error("a refusal is reported as unavailable; the caller will retry a " +
			"request the directory will never accept")
	}
	if got := peerwire.StatusFor(ownerErr); got != peerwire.StatusDirectoryRefused {
		t.Errorf("on the wire = %v; want directory_refused. unavailable means retry, "+
			"and bad_request blames a caller whose request was fine.", got)
	}
}

// A directory with no caller reports that there is none, rather than reporting
// about a session.
func TestDB1DirectoryWithoutACallerSaysSo(t *testing.T) {
	if _, err := NewDB1Directory(nil, time.Second); err == nil {
		t.Fatal("nil caller accepted")
	}
	var d *DB1Directory
	if _, err := d.Owner("x"); !errors.Is(err, peer.ErrNoDirectory) {
		t.Fatalf("nil directory = %v; want ErrNoDirectory", err)
	}
}
