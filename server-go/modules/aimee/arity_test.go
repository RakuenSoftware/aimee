package aimee

import (
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// Every operation's field count is CHECKED, and this is what makes that true of
// the next one somebody adds.
//
// The wire carries everything as text, and Atoi("") is (0, nil) by design -- an
// empty cell is a legitimate zero. So where zero is meaningful, a caller that
// OMITTED a field would be indistinguishable from one that sent zero. The only
// thing standing between those two readings is the arity check, and the checks
// here are twelve hand-written `len(cells) != N` lines rather than one
// mechanism. A peer whose store has a single structural check made the
// distinction that prompted this: "we happen to be fine" and "a mechanism
// prevents it" are different answers, and only the second survives the next
// operation.
//
// This ranges over op numbers rather than over a list of the operations that
// exist, so an operation added tomorrow is inside the population WITHOUT anyone
// remembering to add it -- which is the failure mode a list has and a range does
// not. The legitimate shapes are pinned below; every other (stage, op, width)
// must refuse.
func TestEveryOperationChecksItsFieldCount(t *testing.T) {
	// The arity table, made explicit. This is the same information as the
	// scattered len(cells) checks, in one place where it can be compared
	// against behaviour instead of read.
	type shape struct{ stage, op uint32 }
	legitimate := map[shape]int{
		{peerwire.StageDelivery, peerwire.OpSend}:          6,
		{peerwire.StageDelivery, peerwire.OpReply}:         1 + peerwire.MessageWidth,
		{peerwire.StageDelivery, peerwire.OpCancelWait}:    1,
		{peerwire.StageInbox, peerwire.OpInboxLen}:         1,
		{peerwire.StageInbox, peerwire.OpInboxPeek}:        1,
		{peerwire.StageInbox, peerwire.OpInboxTake}:        2,
		{peerwire.StageGrant, peerwire.OpGrant}:            2,
		{peerwire.StageGrant, peerwire.OpRevoke}:           2,
		{peerwire.StageGrant, peerwire.OpGrantExists}:      2,
		{peerwire.StageChannel, peerwire.OpChannelJoin}:    2,
		{peerwire.StageChannel, peerwire.OpChannelLeave}:   2,
		{peerwire.StageChannel, peerwire.OpChannelMembers}: 1,
		{peerwire.StageChannel, peerwire.OpChannelSend}:    5,
	}

	m := moduleOver(t, newRegistry(t, "A", "B"))

	// An empty table would make this test STRICTER, not weaker -- every shape
	// becomes unknown and any StatusOK fails. An empty STAGE list is the danger:
	// the loops would not run and the test would pass having sent nothing.
	if len(legitimate) == 0 {
		t.Fatal("the arity table is empty; this test would then assert nothing")
	}

	// Op numbers beyond those that exist are included on purpose: an operation
	// added later lands inside this range and must arrive with its check.
	const opCeiling = 8
	const widthCeiling = 14

	stages := []uint32{
		peerwire.StageDelivery, peerwire.StageInbox,
		peerwire.StageGrant, peerwire.StageChannel,
	}
	if len(stages) == 0 {
		t.Fatal("no stages to exercise; the loops below would send nothing")
	}
	sent := 0
	for _, stage := range stages {
		for op := uint32(1); op <= opCeiling; op++ {
			want, known := legitimate[shape{stage, op}]
			for width := 0; width <= widthCeiling; width++ {
				if known && width == want {
					continue // the shape the operation is specified to take
				}
				cells := make([]string, width)
				for i := range cells {
					cells[i] = "x"
				}
				frame, err := peerwire.EncodeRequest(op, cells)
				if err != nil {
					t.Fatalf("encode: %v", err)
				}
				sent++
				body, transport := m.Handle(bus.ModuleInvocation{StageID: stage}, frame)
				if transport != bus.ModuleStatusOK {
					continue // refused at the transport level, which is a refusal
				}
				status, _, err := peerwire.DecodeResponse(body)
				if err != nil {
					continue
				}
				if status == peerwire.StatusOK {
					t.Errorf("stage %d op %d SERVED a %d-cell frame (specified width %d, known=%v). "+
						"An unchecked field count lets a caller omit a field, and an omitted "+
						"numeric cell is Atoi(\"\") = 0 -- indistinguishable from a deliberate zero.",
						stage, op, width, want, known)
				}
			}
		}
	}

	// The count is the proof that the loops above ran. Every guard in this file
	// is an assertion about what the module REFUSES, and a refusal check that
	// sent no frames passes trivially -- so the one thing that cannot be
	// established by observing refusals is whether any were requested.
	if sent == 0 {
		t.Fatal("no frames were sent; a test that asserts refusals and sends nothing " +
			"reports success for having asked no questions")
	}
	t.Logf("%d wrong-shaped frames refused across %d stages", sent, len(stages))
}

// The arity table above must describe the operations that actually exist, or it
// pins nothing: an entry naming an operation the module does not serve would be
// dead, and a served operation missing from the table would have every width
// treated as illegitimate and so never checked against its real shape.
func TestTheArityTableDescribesRealOperations(t *testing.T) {
	m := moduleOver(t, newRegistry(t, "A", "B"))
	for _, tc := range []struct {
		stage, op uint32
		cells     []string
	}{
		{peerwire.StageDelivery, peerwire.OpSend, []string{"A", "B", "hi", "", "0", "0"}},
		{peerwire.StageDelivery, peerwire.OpCancelWait, []string{"A"}},
		{peerwire.StageInbox, peerwire.OpInboxLen, []string{"B"}},
		{peerwire.StageInbox, peerwire.OpInboxPeek, []string{"B"}},
		{peerwire.StageInbox, peerwire.OpInboxTake, []string{"B", "5"}},
		{peerwire.StageGrant, peerwire.OpGrantExists, []string{"uid:1", "uid:2"}},
		{peerwire.StageGrant, peerwire.OpGrant, []string{"uid:1", "uid:2"}},
		{peerwire.StageGrant, peerwire.OpRevoke, []string{"uid:1", "uid:2"}},
		// Ordered so each operation's precondition holds when it runs: leaving
		// the channel before sending to it made ChannelSend answer no_channel,
		// which is a correct domain refusal and says nothing about arity.
		{peerwire.StageChannel, peerwire.OpChannelJoin, []string{"c", "A"}},
		{peerwire.StageChannel, peerwire.OpChannelMembers, []string{"c"}},
		{peerwire.StageChannel, peerwire.OpChannelSend, []string{"A", "c", "hi", "", "0"}},
		{peerwire.StageChannel, peerwire.OpChannelLeave, []string{"c", "A"}},
	} {
		status, _ := call(t, m, tc.stage, tc.op, tc.cells)
		if status != peerwire.StatusOK {
			t.Errorf("stage %d op %d at its specified width answered %v; the arity table "+
				"claims this shape is the legitimate one", tc.stage, tc.op, status)
		}
	}
	// peer.InboxMax is referenced so this file fails to compile if the peer
	// package's shape changes underneath it rather than silently drifting.
	_ = peer.InboxMax
}
