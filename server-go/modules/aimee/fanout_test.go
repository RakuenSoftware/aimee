package aimee

import (
	"testing"

	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// A channel fan-out that actually DELIVERS, which nothing exercised anywhere.
//
// Found by measuring coverage rather than reasoning about it. Every channel test
// in this module and every channel check in the hardware probe is a REFUSAL --
// an unknown sender, an absent channel, a non-member. So the success path was
// never run: DeliveryCells, which encodes the per-recipient outcomes a caller
// reads to learn who received what, had zero coverage while being called in
// production.
//
// Its decoder DeliveryRows had no caller at all, in tests or out. The module
// emits those rows and nothing in the tree reads them, which is the same shape
// as the /v1 routes that no caller mounts: built, correct, and unreachable. This
// test is now its one caller, which is honest about what that proves -- the rows
// decode, and no client yet needs them to.
//
// The fan-out is the one place this module returns PER-RECIPIENT outcomes rather
// than a single verdict, so a partial delivery is visible instead of collapsed
// into one word. That property is the whole reason the rows exist, and it was
// asserted nowhere.
func TestChannelFanOutDeliversAndReportsEachRecipient(t *testing.T) {
	m := moduleOver(t, newRegistry(t, "A", "B", "C"))

	for _, id := range []string{"A", "B", "C"} {
		if status, _ := call(t, m, peerwire.StageChannel, peerwire.OpChannelJoin,
			[]string{"room", id}); status != peerwire.StatusOK {
			t.Fatalf("%s joining = %v", id, status)
		}
	}

	const body = "one message, several recipients"
	status, cells := call(t, m, peerwire.StageChannel, peerwire.OpChannelSend,
		[]string{"A", "room", body, "", "0"})
	if status != peerwire.StatusOK {
		t.Fatalf("channel send = %v; want ok", status)
	}

	// StatusOK here means the FAN-OUT happened, not that every recipient got it.
	// The outcomes ride in the rows, which is what makes a partial delivery
	// visible -- and decoding them is what proves the rows are readable at all.
	deliveries, err := peerwire.DeliveryRows(cells)
	if err != nil {
		t.Fatalf("decode deliveries: %v", err)
	}
	if len(deliveries) != 2 {
		t.Fatalf("deliveries = %d; want 2 (the members other than the sender)", len(deliveries))
	}

	got := map[string]bool{}
	for _, d := range deliveries {
		if d.Err != nil {
			t.Errorf("delivery to %s failed: %v", d.Session, d.Err)
		}
		if d.Message.ID == "" {
			t.Errorf("delivery to %s carries no message id to correlate on", d.Session)
		}
		got[d.Session] = true
	}
	if !got["B"] || !got["C"] {
		t.Errorf("recipients = %v; want B and C", got)
	}
	if got["A"] {
		t.Error("the sender received its own channel message")
	}

	// And the messages really landed, rather than the rows merely describing an
	// intention. A row saying "delivered" with an empty inbox behind it is the
	// same class of lie as a status that cannot fail.
	for _, id := range []string{"B", "C"} {
		_, lenCells := call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{id})
		if len(lenCells) != 2 || lenCells[0] != "1" {
			t.Errorf("%s inbox = %v; want exactly the one channel message", id, lenCells)
		}
	}
	_, senderInbox := call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"A"})
	if senderInbox[0] != "0" {
		t.Errorf("the sender's own inbox = %v; a fan-out must not echo to the sender", senderInbox)
	}
}
