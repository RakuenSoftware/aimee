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
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

const (
	principalClass = 1
	// 69, NOT 67. Ref 67 is reserved for the module's own OUTBOUND identity
	// (aimee-db1, for reading the session directory out of db1). A probe sharing
	// it would be a duplicate principal the moment that client exists, and the
	// bus refuses the second attach -- so the failure would appear only after
	// DirectorySource was wired, in whichever of the two attached second.
	//
	// This ref is validation-only and deliberately NOT declared in
	// process-contracts.json: its grant is written by hand into a validation
	// container, never shipped. See
	// docs/validation/aimee-module-on-a-clean-container.md.
	principalRef = 69
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

	fmt.Println()
	if failures > 0 {
		fmt.Printf("peerprobe: %d check(s) FAILED\n", failures)
		os.Exit(1)
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
