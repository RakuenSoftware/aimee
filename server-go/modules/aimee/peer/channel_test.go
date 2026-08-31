package peer

import (
	"errors"
	"strings"
	"testing"
)

func TestChannelMembership(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B", "C")

	if err := r.ChannelJoin("review", "A"); err != nil {
		t.Fatalf("join: %v", err)
	}
	// Joining twice is not an error: membership is a set.
	if err := r.ChannelJoin("review", "A"); err != nil {
		t.Errorf("re-join: %v", err)
	}
	if err := r.ChannelJoin("review", "B"); err != nil {
		t.Fatalf("join B: %v", err)
	}

	if got := r.ChannelMembers("review"); strings.Join(got, ",") != "A,B" {
		t.Errorf("members = %v; want [A B] sorted", got)
	}
	if got := r.Channels("A"); strings.Join(got, ",") != "review" {
		t.Errorf("Channels(A) = %v", got)
	}

	// An unknown session cannot join: a channel of names nobody answers to
	// would report recipients that can never receive.
	if err := r.ChannelJoin("review", "ghost"); !errors.Is(err, ErrNoPeer) {
		t.Errorf("ghost join: %v; want ErrNoPeer", err)
	}
	// Names are constrained so a channel cannot be addressed two ways.
	for _, bad := range []string{"", "has space", strings.Repeat("x", ChannelNameMax+1), "sla/sh"} {
		if err := r.ChannelJoin(bad, "A"); !errors.Is(err, ErrChannelNameBad) {
			t.Errorf("name %q: err = %v; want ErrChannelNameBad", bad, err)
		}
	}

	if !r.ChannelLeave("review", "B") {
		t.Error("leave reported no membership")
	}
	if r.ChannelLeave("review", "B") {
		t.Error("leaving twice reported membership")
	}
	// The last member leaving removes the channel: an empty channel is
	// indistinguishable from one that never existed.
	r.ChannelLeave("review", "A")
	if got := r.ChannelMembers("review"); got != nil {
		t.Errorf("emptied channel still present: %v", got)
	}
}

func TestChannelSendFansOut(t *testing.T) {
	r, notes := newTestRegistry(t, "A", "B", "C")
	for _, id := range []string{"A", "B", "C"} {
		if err := r.ChannelJoin("review", id); err != nil {
			t.Fatal(err)
		}
	}

	out, err := r.ChannelSend("A", "review", "look at this", SendOptions{})
	if err != nil {
		t.Fatalf("ChannelSend: %v", err)
	}
	// One entry per intended recipient, sender excluded.
	if len(out) != 2 {
		t.Fatalf("deliveries = %d; want 2 (B and C, not A)", len(out))
	}
	for _, d := range out {
		if !d.Delivered() {
			t.Errorf("%s: %v", d.Session, d.Err)
		}
		if d.Session == "A" {
			t.Error("the sender received its own channel message")
		}
	}
	if r.Len("A") != 0 {
		t.Error("sender's own inbox received the fan-out")
	}
	if r.Len("B") != 1 || r.Len("C") != 1 {
		t.Errorf("inboxes = B:%d C:%d; want 1 each", r.Len("B"), r.Len("C"))
	}

	// One conversation spans the fan-out, and every message is at hop+1 so a
	// cycle through a channel burns the same budget as a direct exchange.
	b := r.Take("B", 1)[0]
	c := r.Take("C", 1)[0]
	if b.ConversationID != c.ConversationID || b.ConversationID == "" {
		t.Errorf("conversation ids differ: %q vs %q", b.ConversationID, c.ConversationID)
	}
	if b.Hop != 1 || c.Hop != 1 {
		t.Errorf("hops = %d/%d; want 1 (opts.Hop 0 + 1)", b.Hop, c.Hop)
	}
	if b.FromSession != "A" || b.OriginSession != "A" {
		t.Errorf("provenance = %+v", b)
	}
	// Fan-out is announced live like any other delivery.
	if len(notes.forSession("B")) != 1 || len(notes.forSession("C")) != 1 {
		t.Error("channel deliveries were not announced")
	}
}

// The sender must be a member. A non-member writing to a channel would reach
// sessions that never agreed to hear from it.
func TestChannelSendRequiresMembership(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	if err := r.ChannelJoin("review", "B"); err != nil {
		t.Fatal(err)
	}

	if _, err := r.ChannelSend("A", "review", "hi", SendOptions{}); !errors.Is(err, ErrNotMember) {
		t.Fatalf("non-member send: %v; want ErrNotMember", err)
	}
	if r.Len("B") != 0 {
		t.Error("a non-member's message was delivered anyway")
	}
	if _, err := r.ChannelSend("A", "nosuch", "hi", SendOptions{}); !errors.Is(err, ErrNoChannel) {
		t.Errorf("missing channel: %v; want ErrNoChannel", err)
	}
	if _, err := r.ChannelSend("ghost", "review", "hi", SendOptions{}); !errors.Is(err, ErrUnknownSender) {
		t.Errorf("unknown sender: %v; want ErrUnknownSender", err)
	}
}

// PARTIAL delivery is the case the per-recipient result exists for: a single
// "sent" would hide that some members never got it.
func TestChannelSendReportsPartialDelivery(t *testing.T) {
	r := New(Options{})
	for _, s := range []struct{ id, owner string }{
		{"A", "uid:1000"}, {"B", "uid:1000"}, {"X", "uid:2000"},
	} {
		if err := r.Register(s.id, s.owner, "cli"); err != nil {
			t.Fatal(err)
		}
	}
	for _, id := range []string{"A", "B", "X"} {
		if err := r.ChannelJoin("mixed", id); err != nil {
			t.Fatal(err)
		}
	}

	out, err := r.ChannelSend("A", "mixed", "hello all", SendOptions{})
	if err != nil {
		t.Fatalf("ChannelSend: %v", err)
	}
	if len(out) != 2 {
		t.Fatalf("deliveries = %d; want 2", len(out))
	}

	byID := map[string]Delivery{}
	for _, d := range out {
		byID[d.Session] = d
	}
	// Same owner: delivered.
	if !byID["B"].Delivered() {
		t.Errorf("B: %v; want delivered", byID["B"].Err)
	}
	// Cross owner without a grant: refused, and SAID SO rather than silently
	// dropped. This is the whole point of the per-recipient result.
	if !errors.Is(byID["X"].Err, ErrDenied) {
		t.Errorf("X: %v; want ErrDenied", byID["X"].Err)
	}
	if r.Len("X") != 0 {
		t.Error("a denied cross-owner member received the message")
	}

	// With a grant it lands, and the caller can tell the difference.
	if err := r.Grant("uid:1000", "uid:2000"); err != nil {
		t.Fatal(err)
	}
	out, _ = r.ChannelSend("A", "mixed", "second try", SendOptions{})
	for _, d := range out {
		if !d.Delivered() {
			t.Errorf("after grant %s: %v", d.Session, d.Err)
		}
	}
}

// A channel obeys the hop ceiling and the inbox bound like any direct send: it
// is addressing sugar, not an escape from the budgets.
func TestChannelObeysTheSameBounds(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	for _, id := range []string{"A", "B"} {
		if err := r.ChannelJoin("c", id); err != nil {
			t.Fatal(err)
		}
	}

	r.SetMaxHops(2)
	// hop 0 -> delivered at 1; hop 1 -> delivered at 2 == ceiling, refused.
	out, err := r.ChannelSend("A", "c", "at the edge", SendOptions{Hop: 1})
	if err != nil {
		t.Fatalf("ChannelSend: %v", err)
	}
	if len(out) != 1 || !errors.Is(out[0].Err, ErrHopLimit) {
		t.Fatalf("at ceiling: %+v; want ErrHopLimit", out)
	}
	r.SetMaxHops(0)

	// Inbox bound applies per recipient.
	for i := 0; i < InboxMax; i++ {
		if _, err := r.Send("A", "B", "fill", SendOptions{}); err != nil {
			t.Fatal(err)
		}
	}
	out, _ = r.ChannelSend("A", "c", "overflow", SendOptions{})
	if len(out) != 1 || !errors.Is(out[0].Err, ErrInboxFull) {
		t.Fatalf("full inbox: %+v; want ErrInboxFull", out)
	}
	if r.Dropped("B") == 0 {
		t.Error("channel overflow was not counted")
	}
}

// A member whose session goes away must not linger as a recipient that can
// never receive -- the in-memory form of the orphan-row problem.
func TestChannelDropsDepartedSessions(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B", "C")
	for _, id := range []string{"A", "B", "C"} {
		if err := r.ChannelJoin("c", id); err != nil {
			t.Fatal(err)
		}
	}

	// C leaves entirely, with an empty inbox, so its entry is removed.
	r.Unregister("C")
	if got := r.ChannelMembers("c"); strings.Join(got, ",") != "A,B" {
		t.Fatalf("members after departure = %v; want [A B]", got)
	}

	out, err := r.ChannelSend("A", "c", "still here?", SendOptions{})
	if err != nil {
		t.Fatalf("ChannelSend: %v", err)
	}
	if len(out) != 1 || out[0].Session != "B" || !out[0].Delivered() {
		t.Fatalf("deliveries = %+v; want B only", out)
	}
}

func TestChannelBounds(t *testing.T) {
	r, _ := newTestRegistry(t, "A")
	for i := 0; i < ChannelsMax; i++ {
		if err := r.ChannelJoin("chan"+string(rune('a'+i%26))+string(rune('a'+i/26)), "A"); err != nil {
			t.Fatalf("channel %d: %v", i, err)
		}
	}
	if err := r.ChannelJoin("one-too-many", "A"); !errors.Is(err, ErrChannelsFull) {
		t.Fatalf("channel ceiling: %v; want ErrChannelsFull", err)
	}
}
