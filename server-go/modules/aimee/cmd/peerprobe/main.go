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

	call := func(stage, op uint32, cells []string) (aimee.Status, []string, error) {
		frame, err := aimee.EncodeRequest(op, cells)
		if err != nil {
			return 0, nil, err
		}
		reply, err := caller.Call(ctx, aimee.EventKind(stage), stage, 0, callDeadline, frame)
		if err != nil {
			return 0, nil, err
		}
		return aimee.DecodeResponse(reply)
	}

	// The module has no directory source, so sessions must register through the
	// bus before they can be addressed. Registration is not a stage; the
	// registry is process-local, so the probe drives what IS reachable: a send
	// to an unregistered sender must be refused, not crash the module.
	fmt.Println("stage reachability and domain refusals over the bus:")

	var cells []string
	status, _, err := call(aimee.StageDelivery, aimee.OpSend,
		[]string{"probe-A", "probe-B", "hello over the bus", "", "0", "0"})
	check(fmt.Sprintf("delivery stage (kind %d) reachable", aimee.EventDelivery), err == nil,
		fmt.Sprintf("err=%v status=%v", err, status))
	check("unknown sender refused as a SERVED call", err == nil && status == aimee.StatusUnknownSender,
		fmt.Sprintf("status=%v (want unknown_sender, and no transport error)", status))

	status, cells, err = call(aimee.StageInbox, aimee.OpInboxLen, []string{"probe-B"})
	check(fmt.Sprintf("inbox stage (kind %d) reachable", aimee.EventInbox), err == nil,
		fmt.Sprintf("err=%v", err))
	check("unknown session refused, not reported empty",
		err == nil && status == aimee.StatusNoPeer,
		fmt.Sprintf("status=%v cells=%v (want no_peer)", status, cells))

	// Owners are unique per run. The module is a long-lived process that holds
	// its grants in memory, so a probe reusing fixed owners passes once and then
	// fails against its own leftovers -- which is exactly what happened on the
	// second run of this probe, and is a defect in the probe rather than in the
	// module. Uniqueness per run is what makes it idempotent.
	from := fmt.Sprintf("uid:probe-%d-a", os.Getpid())
	to := fmt.Sprintf("uid:probe-%d-b", os.Getpid())

	status, cells, err = call(aimee.StageGrant, aimee.OpGrantExists, []string{from, to})
	check(fmt.Sprintf("grant stage (kind %d) reachable", aimee.EventGrant), err == nil,
		fmt.Sprintf("err=%v", err))
	check("absent grant answers OK with a false value",
		err == nil && status == aimee.StatusOK && len(cells) == 1 && cells[0] == "0",
		fmt.Sprintf("status=%v cells=%v", status, cells))

	// A grant is directed and survives the round trip.
	status, _, err = call(aimee.StageGrant, aimee.OpGrant, []string{from, to})
	check("grant write accepted", err == nil && status == aimee.StatusOK,
		fmt.Sprintf("status=%v err=%v", status, err))
	status, cells, err = call(aimee.StageGrant, aimee.OpGrantExists, []string{from, to})
	check("grant is readable back (module holds state)",
		err == nil && status == aimee.StatusOK && len(cells) == 1 && cells[0] == "1",
		fmt.Sprintf("status=%v cells=%v", status, cells))
	status, cells, err = call(aimee.StageGrant, aimee.OpGrantExists, []string{to, from})
	check("grant did NOT leak in the reverse direction",
		err == nil && status == aimee.StatusOK && len(cells) == 1 && cells[0] == "0",
		fmt.Sprintf("status=%v cells=%v", status, cells))

	// A drain reply leads with how many remain, so a caller can tell a complete
	// drain from a capped one without a second call.
	status, cells, err = call(aimee.StageInbox, aimee.OpInboxTake, []string{"probe-A", "1"})
	check("take on an unknown session is refused", err == nil && status == aimee.StatusNoPeer,
		fmt.Sprintf("status=%v (want no_peer)", status))

	// Channel stage: reachable, and refusing a non-member rather than fanning
	// out to sessions that never agreed to hear from the sender.
	status, _, err = call(aimee.StageChannel, aimee.OpChannelSend,
		[]string{"probe-A", "nosuch", "hello channel", "", "0"})
	check(fmt.Sprintf("channel stage (kind %d) reachable", aimee.EventChannel), err == nil,
		fmt.Sprintf("err=%v", err))
	check("channel send by an unknown sender is refused",
		err == nil && status == aimee.StatusUnknownSender,
		fmt.Sprintf("status=%v (want unknown_sender)", status))

	status, cells, err = call(aimee.StageChannel, aimee.OpChannelMembers, []string{"nosuch"})
	check("members of an absent channel answers OK with none",
		err == nil && status == aimee.StatusOK && len(cells) == 0,
		fmt.Sprintf("status=%v cells=%v", status, cells))

	// A malformed frame is a TRANSPORT-level refusal, distinct from a domain one.
	_, err = caller.Call(ctx, aimee.EventDelivery, aimee.StageDelivery, 0, callDeadline,
		[]byte("not a fields-v2 frame"))
	check("malformed frame refused at the transport level", err != nil,
		fmt.Sprintf("err=%v (want a non-nil transport error)", err))

	// An unadvertised stage under this module's principal must not be served.
	_, err = caller.Call(ctx, aimee.EventKind(9), 9, 0, callDeadline, mustFrame())
	check("unadvertised stage 9 is not served", err != nil,
		fmt.Sprintf("err=%v (want refusal)", err))

	fmt.Println()
	if failures > 0 {
		fmt.Printf("peerprobe: %d check(s) FAILED\n", failures)
		os.Exit(1)
	}
	fmt.Println("peerprobe: all checks passed")
}

func mustFrame() []byte {
	f, _ := aimee.EncodeRequest(aimee.OpInboxLen, []string{"x"})
	return f
}
