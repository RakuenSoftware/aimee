// peerprobe drives the aimee module's peer-messaging stages over the real
// event bus, as an admitted client with its own grant.
//
// Everything the unit tests prove is in-process: they call Handle directly.
// This proves the parts they cannot -- that the module is admitted under its
// principal, that the bus routes its kinds to it, that db1-fields-v2 frames
// survive the transport, and that a domain refusal arrives as a SERVED call
// while a malformed frame arrives as a transport error. In process there is no
// transport, so that last distinction cannot be tested there at all.
//
// Run it inside a deployment with AIMEE_MODULE_BUS_SOCKET set, as a principal
// whose grant carries request= for this module's kinds. See
// docs/validation/aimee-module-on-a-clean-container.md.
package main

import (
	"context"
	"fmt"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

const (
	principalClass = 1
	// 200, and the distance from the client range is the point.
	//
	// It was 69, chosen only because 67 belongs to the module's own OUTBOUND
	// identity (aimee-db1) and a probe sharing that would be a duplicate
	// principal the bus refuses on the second attach. But 69 is inside the range
	// real clients are allocated from, and the control-plane module has since
	// taken it. Two principals at one ref is a live collision: the bus admits
	// whichever attaches first and denies the other, so the failure surfaces in
	// the innocent process rather than at the cause.
	//
	// A validation-only ref must therefore sit somewhere the contract will never
	// allocate, not merely somewhere unused TODAY. This ref is deliberately NOT
	// declared in process-contracts.json -- its grant is written by hand into a
	// validation container and never shipped -- which is exactly why it cannot
	// rely on the validator to keep it clear of anyone else.
	principalRef = 200
	callDeadline = 5 * time.Second
)

var failures int

func check(name string, ok bool, detail string) {
	if ok {
		fmt.Printf("  PASS  %-46s %s\n", name, detail)
		return
	}
	failures++
	fmt.Printf("  FAIL  %-46s %s\n", name, detail)
}

func main() {
	socket := os.Getenv("AIMEE_MODULE_BUS_SOCKET")
	if socket == "" {
		fmt.Println("AIMEE_MODULE_BUS_SOCKET unset")
		os.Exit(2)
	}
	ctx := context.Background()

	client, err := bus.ConnectClient(ctx, socket, principalClass, principalRef)
	if err != nil {
		fmt.Printf("attach as principal %d failed: %v\n", principalRef, err)
		os.Exit(2)
	}
	defer client.Detach()
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		fmt.Printf("caller: %v\n", err)
		os.Exit(2)
	}
	// The caller polls the bus's SHARED-MEMORY region from its own goroutine,
	// and Detach unmaps that region. CloseAndWait says so in as many words:
	// "It must run before the underlying Client is detached and its
	// shared-memory region is unmapped."
	//
	// Without this the probe segfaulted AFTER printing "all checks passed" --
	// every check green, then a fault in Control.Epoch reading through an
	// unmapped page. Deferred here so it runs before the Detach deferred above
	// it, and called explicitly on every os.Exit path below, because os.Exit
	// runs no defers at all and those paths would keep the fault.
	//
	// Nothing in process can find this: there is no region to unmap, so the
	// lifecycle rule has nothing to enforce it and the tests cannot fail.
	defer caller.CloseAndWait()
	exit := func(code int) {
		caller.CloseAndWait()
		client.Detach()
		os.Exit(code)
	}
	fmt.Printf("attached to %s as principal %d/%d\n\n", socket, principalClass, principalRef)

	call := func(stage, op uint32, cells []string) (peerwire.Status, []string, error) {
		frame, err := peerwire.EncodeRequest(op, cells)
		if err != nil {
			return 0, nil, err
		}
		reply, err := caller.Call(ctx, peerwire.EventKind(aimee.PrincipalRef, stage), stage, 0, callDeadline, frame)
		if err != nil {
			return 0, nil, err
		}
		return peerwire.DecodeResponse(reply)
	}

	fmt.Println("stage reachability and domain refusals over the bus:")

	var cells []string
	status, _, err := call(peerwire.StageDelivery, peerwire.OpSend,
		[]string{"probe-A", "probe-B", "hello over the bus", "", "0", "0"})
	check(fmt.Sprintf("delivery stage (kind %d) reachable", peerwire.EventKind(aimee.PrincipalRef, peerwire.StageDelivery)), err == nil,
		fmt.Sprintf("err=%v status=%v", err, status))

	// WHICH MODULE IS THIS. Established before anything is asserted about a
	// refusal, because a refusal on its own does not say.
	//
	// An earlier version of this probe asserted unknown_sender here and called
	// it a pass. It was: the module refuses an unregistered sender correctly.
	// But a module with NO SESSION DIRECTORY refuses identically, and that is
	// what shipped -- nothing registers a session there, so the check could not
	// have failed and proved nothing about delivery ever working. Every
	// session-scoped check below had the same blind spot.
	//
	// So the configuration is a finding in its own right, and the refusal
	// assertions are made against it rather than against a hope.
	// A TRANSPORT failure first, because every judgement below reads a status
	// word that only means anything if the call was actually served.
	//
	// Found on hardware: with the module absent the bus answers "capability
	// absent" and the status is left at its zero value, which is StatusOK. The
	// test below then read ok != no_directory and reported wired=true -- "the
	// module has a directory" about a module that was not running at all. Every
	// subsequent check then asserted the wired expectations and failed for a
	// reason that had nothing to do with what it was testing.
	//
	// Stopping here is the honest outcome: nothing downstream can be measured
	// through a module that is not there.
	if err != nil {
		fmt.Printf("  FAIL  %-46s %v\n",
			"delivery stage answered at all", err)
		fmt.Println("\npeerprobe: the module is not serving its stages; nothing below can be measured")
		exit(1)
	}

	wired := status != peerwire.StatusNoDirectory
	if wired {
		fmt.Println("  ....  module HAS a session directory; asserting real refusals")
	} else {
		fmt.Println("  ....  module has NO session directory; peer messaging is inert here")
	}
	check("module states whether it has a session directory", true,
		fmt.Sprintf("wired=%v (send answered %v)", wired, status))

	wantSender, wantSession := peerwire.StatusUnknownSender, peerwire.StatusNoPeer
	if !wired {
		wantSender, wantSession = peerwire.StatusNoDirectory, peerwire.StatusNoDirectory
	}

	// WHICH DIRECTORY IS THIS, one level below "is there one at all".
	//
	// A wired module's answer for an ABSENT session depends on the store behind
	// it. db1's Go store reports Missing, which arrives as no_peer -- a fact the
	// caller may act on. The C store returns -1 for a bad argument, a dead
	// connection AND a row that is not there, and its stage maps any non-zero rc
	// to FAILED, so absence arrives as unavailable.
	//
	// Both are accepted here and the observed one is REPORTED, because failing
	// on the C store would be failing the probe for a defect in a component it
	// does not own -- and an expected FAIL is how people learn to skim past
	// failures. What must never happen is the third possibility: absence
	// arriving as ok.
	if wired && status == peerwire.StatusUnavailable {
		fmt.Println("  ....  the directory CANNOT distinguish absent from broken; " +
			"this is db1's C store (its Go store reports missing)")
		wantSender, wantSession = peerwire.StatusUnavailable, peerwire.StatusUnavailable
	}
	check("unknown sender refused as a SERVED call", err == nil && status == wantSender,
		fmt.Sprintf("status=%v (want %v, and no transport error)", status, wantSender))

	status, cells, err = call(peerwire.StageInbox, peerwire.OpInboxLen, []string{"probe-B"})
	check(fmt.Sprintf("inbox stage (kind %d) reachable", peerwire.EventKind(aimee.PrincipalRef, peerwire.StageInbox)), err == nil,
		fmt.Sprintf("err=%v", err))
	check("unknown session refused, not reported empty",
		err == nil && status == wantSession,
		fmt.Sprintf("status=%v cells=%v (want %v)", status, cells, wantSession))

	// Owners are unique per run. The module is a long-lived process that holds
	// its grants in memory, so a probe reusing fixed owners passes once and then
	// fails against its own leftovers -- which is exactly what happened on the
	// second run of this probe, and is a defect in the probe rather than in the
	// module. Uniqueness per run is what makes it idempotent.
	from := fmt.Sprintf("uid:probe-%d-a", os.Getpid())
	to := fmt.Sprintf("uid:probe-%d-b", os.Getpid())

	status, cells, err = call(peerwire.StageGrant, peerwire.OpGrantExists, []string{from, to})
	check(fmt.Sprintf("grant stage (kind %d) reachable", peerwire.EventKind(aimee.PrincipalRef, peerwire.StageGrant)), err == nil,
		fmt.Sprintf("err=%v", err))
	check("absent grant answers OK with a false value",
		err == nil && status == peerwire.StatusOK && len(cells) == 1 && cells[0] == "0",
		fmt.Sprintf("status=%v cells=%v", status, cells))

	// A grant is directed and survives the round trip.
	status, _, err = call(peerwire.StageGrant, peerwire.OpGrant, []string{from, to})
	check("grant write accepted", err == nil && status == peerwire.StatusOK,
		fmt.Sprintf("status=%v err=%v", status, err))
	status, cells, err = call(peerwire.StageGrant, peerwire.OpGrantExists, []string{from, to})
	check("grant is readable back (module holds state)",
		err == nil && status == peerwire.StatusOK && len(cells) == 1 && cells[0] == "1",
		fmt.Sprintf("status=%v cells=%v", status, cells))
	status, cells, err = call(peerwire.StageGrant, peerwire.OpGrantExists, []string{to, from})
	check("grant did NOT leak in the reverse direction",
		err == nil && status == peerwire.StatusOK && len(cells) == 1 && cells[0] == "0",
		fmt.Sprintf("status=%v cells=%v", status, cells))

	// A drain reply leads with how many remain, so a caller can tell a complete
	// drain from a capped one without a second call.
	status, cells, err = call(peerwire.StageInbox, peerwire.OpInboxTake, []string{"probe-A", "1"})
	check("take on an unknown session is refused", err == nil && status == wantSession,
		fmt.Sprintf("status=%v (want %v)", status, wantSession))

	// Channel stage: reachable, and refusing a non-member rather than fanning
	// out to sessions that never agreed to hear from the sender.
	status, _, err = call(peerwire.StageChannel, peerwire.OpChannelSend,
		[]string{"probe-A", "nosuch", "hello channel", "", "0"})
	check(fmt.Sprintf("channel stage (kind %d) reachable", peerwire.EventKind(aimee.PrincipalRef, peerwire.StageChannel)), err == nil,
		fmt.Sprintf("err=%v", err))
	check("channel send by an unknown sender is refused",
		err == nil && status == wantSender,
		fmt.Sprintf("status=%v (want %v)", status, wantSender))

	// The sharpest case, and the one that most deserved to be caught here.
	//
	// An absent channel answering OK with no members is correct FOR A MODULE
	// THAT HAS SESSIONS. Unwired, the same call answered ok/none too -- a
	// successful empty answer from a module where no channel can ever have a
	// member. That is not a refusal a reader would question; it is a healthy
	// reply, and it is the one shape that looks like the feature working.
	status, cells, err = call(peerwire.StageChannel, peerwire.OpChannelMembers, []string{"nosuch"})
	wantMembers := peerwire.StatusOK
	if !wired {
		wantMembers = peerwire.StatusNoDirectory
	}
	check("members of an absent channel answers OK with none",
		err == nil && status == wantMembers && len(cells) == 0,
		fmt.Sprintf("status=%v cells=%v (want %v)", status, cells, wantMembers))

	// A malformed frame is a TRANSPORT-level refusal, distinct from a domain one.
	_, err = caller.Call(ctx, peerwire.EventKind(aimee.PrincipalRef, peerwire.StageDelivery), peerwire.StageDelivery, 0, callDeadline,
		[]byte("not a fields-v2 frame"))
	check("malformed frame refused at the transport level", err != nil,
		fmt.Sprintf("err=%v (want a non-nil transport error)", err))

	// An unadvertised stage under this module's principal must not be served.
	_, err = caller.Call(ctx, peerwire.EventKind(aimee.PrincipalRef, 9), 9, 0, callDeadline, mustFrame())
	check("unadvertised stage 9 is not served", err != nil,
		fmt.Sprintf("err=%v (want refusal)", err))

	// A CORRUPT CELL is refused rather than read as an unset one.
	//
	// These two exist because the decoders changed after the last hardware run,
	// and a re-run that exercises only what the previous one did proves the
	// build compiles rather than that the change works. Atob used to answer
	// plain false for anything unrecognised, so a malformed flag and a
	// deliberate "no" were one value; textToTime mapped an unparseable timestamp
	// onto the zero time, which is exactly what the encoder writes for a message
	// that has none.
	//
	// Over the wire, both now arrive as bad_request. In process the same is
	// asserted by unit tests -- what only hardware can show is that the frame
	// carrying a corrupt cell survives the transport intact and is refused by
	// the MODULE rather than mangled on the way.
	status, _, err = call(peerwire.StageDelivery, peerwire.OpSend,
		[]string{"probe-A", "probe-B", "hi", "", "0", "not-a-flag"})
	check("a corrupt boolean cell is refused, not read as false",
		err == nil && status == peerwire.StatusBadRequest,
		fmt.Sprintf("status=%v (want bad_request)", status))

	corruptRow := append([]string{"probe-A"},
		peerwire.MessageCells(peer.Message{ID: "m1"})...)
	corruptRow[1+9] = "yesterday" // sent_at
	status, _, err = call(peerwire.StageDelivery, peerwire.OpReply, corruptRow)
	check("a corrupt timestamp cell is refused, not read as absent",
		err == nil && status == peerwire.StatusBadRequest,
		fmt.Sprintf("status=%v (want bad_request)", status))

	// ---- delivery, end to end ------------------------------------------
	//
	// Everything above is reachability and refusal, and every one of those
	// checks passes against a module that can never deliver anything. This is
	// the part that cannot: two sessions that really exist, a message that
	// really crosses, and an inbox that really holds it.
	//
	// It runs only when a directory is wired, because without one no session
	// can exist and there is nothing to send between. Skipped is reported as
	// skipped rather than passed -- a run that could not try must not read like
	// a run that succeeded.
	if !wired {
		fmt.Println("\ndelivery end to end: SKIPPED, no session directory (peer messaging is inert here)")
	} else {
		fmt.Println("\ndelivery end to end, between two sessions db1 actually holds:")
		deliveryChecks(ctx, caller, call)
	}

	fmt.Println()
	if failures > 0 {
		fmt.Printf("peerprobe: %d check(s) FAILED\n", failures)
		exit(1)
	}
	fmt.Println("peerprobe: all checks passed")
}

// mustFrame builds the known-good frame used to prove an UNADVERTISED stage is
// not served.
//
// The error was discarded here, which quietly changed what the check tested: a
// failed encode yields a nil frame, the call is then refused for being empty
// rather than for naming a stage the module does not advertise, and the check
// passes for the wrong reason. It is the same defect the probe exists to find,
// in the probe.
func mustFrame() []byte {
	f, err := peerwire.EncodeRequest(peerwire.OpInboxLen, []string{"x"})
	if err != nil {
		fmt.Printf("peerprobe: cannot build the probe frame: %v\n", err)
		os.Exit(2)
	}
	return f
}

// db1's sessions family, addressed by the same bus formula. Op and status
// numbers are db1's, not this module's: its 1 is MISSING where peerwire's is
// no_peer, so they are named separately here and never crossed.
const (
	db1SessionsStage   uint32 = 6
	db1OpSessionCreate uint32 = 1
	db1OpSessionDelete uint32 = 4
	db1StatusOK        uint32 = 0
)

// deliveryChecks proves the thing the rest of this probe cannot: that a message
// sent by one real session arrives in another real session's inbox.
//
// The sessions are created in db1 over the bus rather than by a CLI, so the
// module's directory reads them from the same store this probe wrote them to --
// which is the point. Nothing here registers a session with the peer module; it
// learns of them entirely through db1.
func deliveryChecks(ctx context.Context, caller *bus.ConcurrentModuleCaller,
	call func(stage, op uint32, cells []string) (peerwire.Status, []string, error)) {

	// Unique per run: this module holds inboxes in memory and the store keeps
	// rows, so fixed ids would make a second run collide with the first.
	sessionA := fmt.Sprintf("probe-a-%d", os.Getpid())
	sessionB := fmt.Sprintf("probe-b-%d", os.Getpid())
	owner := fmt.Sprintf("uid:probe-%d", os.Getpid())

	db1Call := func(op uint32, cells []string) (uint32, error) {
		frame, err := peerwire.EncodeRequest(op, cells)
		if err != nil {
			return 0, err
		}
		reply, err := caller.Call(ctx, peerwire.EventKind(aimee.DB1PrincipalRef, db1SessionsStage),
			db1SessionsStage, 0, callDeadline, frame)
		if err != nil {
			return 0, err
		}
		status, _, err := peerwire.DecodeReply(reply)
		return status, err
	}

	// Cleanup is arranged before the rows exist, so a failure below still
	// removes them.
	defer func() {
		for _, id := range []string{sessionA, sessionB} {
			if _, err := db1Call(db1OpSessionDelete, []string{id}); err != nil {
				fmt.Printf("  WARN  could not delete session %s: %v\n", id, err)
			}
		}
	}()

	for _, id := range []string{sessionA, sessionB} {
		status, err := db1Call(db1OpSessionCreate, []string{id, "cli", owner})
		check(fmt.Sprintf("db1 holds session %s", id), err == nil && status == db1StatusOK,
			fmt.Sprintf("db1 status=%d err=%v", status, err))
	}

	// The module has never been told these sessions exist. It must learn that
	// from db1 alone.
	const body = "hello across the bus, from one session to another"
	status, cells, err := call(peerwire.StageDelivery, peerwire.OpSend,
		[]string{sessionA, sessionB, body, "", "0", "0"})
	check("send between two directory-known sessions is accepted",
		err == nil && status == peerwire.StatusOK,
		fmt.Sprintf("status=%v err=%v", status, err))

	sent, rowErr := peerwire.MessageRows(cells)
	check("the send reply carries a stamped envelope",
		rowErr == nil && len(sent) == 1 && sent[0].FromSession == sessionA && sent[0].FromOwner == owner,
		fmt.Sprintf("rows=%d err=%v", len(sent), rowErr))

	status, cells, err = call(peerwire.StageInbox, peerwire.OpInboxLen, []string{sessionB})
	check("the recipient's inbox reports exactly one message",
		err == nil && status == peerwire.StatusOK && len(cells) == 2 && cells[0] == "1",
		fmt.Sprintf("status=%v cells=%v", status, cells))

	// The payload itself. This is the only check in the probe that reads the
	// text a sender actually wrote.
	status, cells, err = call(peerwire.StageInbox, peerwire.OpInboxTake, []string{sessionB, "10"})
	remaining, taken, takeErr := peerwire.TakeReply(cells)
	check("DELIVERED: the recipient drains the sender's exact text",
		err == nil && status == peerwire.StatusOK && takeErr == nil &&
			len(taken) == 1 && taken[0].Text == body && taken[0].FromSession == sessionA,
		fmt.Sprintf("status=%v remaining=%d rows=%d err=%v", status, remaining, len(taken), takeErr))

	check("the drain reports nothing left behind", takeErr == nil && remaining == 0,
		fmt.Sprintf("remaining=%d", remaining))

	// Draining removed it: a second take must find the inbox empty rather than
	// handing the same message out twice.
	status, cells, err = call(peerwire.StageInbox, peerwire.OpInboxLen, []string{sessionB})
	check("the drained message is gone, not re-delivered",
		err == nil && status == peerwire.StatusOK && len(cells) == 2 && cells[0] == "0",
		fmt.Sprintf("status=%v cells=%v", status, cells))
}
