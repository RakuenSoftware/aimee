package aimee

import (
	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

// DirectorySource answers WHO EXISTS, against the owner of that fact: db1's
// server_sessions, whose rows are created and deleted by the session lifecycle.
//
//	Owner -> server_session_get
//
// It deliberately does NOT resolve labels. An earlier version of this interface
// did, on the reading that a peer label is db1's server_sessions.title. That was
// a match of column SHAPE rather than meaning, and the column turns out to have
// no writer at all: the session insert stores the literal empty string and no
// statement ever updates it. Deferring to a writer that does not exist would
// have left peer addressing with nobody able to set a name.
//
// A peer label and a session title are different things. A title is a display
// name for a session; a label is the handle peer messaging is addressed by, and
// it is per-session state of exactly the kind this module already owns
// alongside inboxes. So labels are owned here, and only EXISTENCE is deferred.
type DirectorySource interface {
	// Owner reports a session's owner principal, or why it cannot.
	//
	// Return peer.ErrNoPeer for a definite absence and any other error for "I
	// could not answer". db1 distinguishes these (StatusMissing versus a
	// failure) and the Go caller side preserves the status word, so collapsing
	// them here would discard a distinction the store took care to make -- and
	// would report a live session as gone whenever the store hiccups.
	Owner(sessionID string) (string, error)
}

// PeerCapability serves session peer messaging: the verb by which one aimee
// session addresses another as a peer rather than as a delegate or a client.
//
// It owns inboxes and cross-owner grants and nothing else — notably not the
// session directory, which is db1's (see DirectorySource).
// The directory is deliberately NOT held as a field. It is bound into the
// registry by NewPeer, and the registry is the only way to reach it: a
// capability holding its own reference is how a second access path reappears
// after the first one was removed.
type PeerCapability struct {
	registry *peer.Registry
}

// NewPeer builds the peer-messaging capability. dir may be nil, in which case
// the registry answers directory questions from its own map, which is a test
// and bring-up fallback rather than a source of truth.
//
// When dir IS given it is bound into the registry, so existence has one answer
// whichever surface asked. Leaving them separate is how a module ends up
// authoritative for a caller arriving over the bus and guessing for the same
// caller arriving over HTTP.
func NewPeer(registry *peer.Registry, dir DirectorySource) *PeerCapability {
	if dir != nil && registry != nil {
		registry.SetSessionOwner(dir.Owner)
	}
	return &PeerCapability{registry: registry}
}

// Stages are the peer stages. Event kinds are derived from the bus formula
// rather than written out, so identity and advertisement cannot drift.
func (c *PeerCapability) Stages() []bus.ModuleStage {
	stages := []uint32{
		peerwire.StageDelivery, peerwire.StageInbox,
		peerwire.StageGrant, peerwire.StageChannel,
	}
	out := make([]bus.ModuleStage, 0, len(stages))
	for _, stage := range stages {
		out = append(out, bus.ModuleStage{
			EventKind: peerwire.EventKind(PrincipalRef, stage),
			StageID:   stage,
		})
	}
	return out
}

// Handle serves one peer invocation.
func (c *PeerCapability) Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	op, cells, err := peerwire.DecodeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var body []byte
	var encErr error
	switch invocation.StageID {
	case peerwire.StageDelivery:
		body, encErr = c.delivery(op, cells)
	case peerwire.StageInbox:
		body, encErr = c.inbox(op, cells)
	case peerwire.StageGrant:
		body, encErr = c.grant(op, cells)
	case peerwire.StageChannel:
		body, encErr = c.channel(op, cells)
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	if encErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	if body == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	return body, bus.ModuleStatusOK
}

// refuse builds a domain refusal. It is a SUCCESSFUL invocation: the module was
// asked a well-formed question and answered "no".
func refuse(status peerwire.Status) ([]byte, error) { return peerwire.EncodeResponse(status, nil) }

// ---- stage: delivery -----------------------------------------------------

func (c *PeerCapability) delivery(op uint32, cells []string) ([]byte, error) {
	switch op {
	case peerwire.OpSend:
		// from, to, text, conversation_id, hop, expect_reply
		if len(cells) != 6 {
			return nil, nil
		}
		hop, err := peerwire.Atoi(cells[4])
		if err != nil {
			return refuse(peerwire.StatusBadRequest)
		}
		msg, sendErr := c.registry.Send(cells[0], cells[1], cells[2], peer.SendOptions{
			ConversationID: cells[3],
			Hop:            hop,
			ExpectReply:    peerwire.Atob(cells[5]),
		})
		if sendErr != nil {
			return refuse(peerwire.StatusFor(sendErr))
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, peerwire.MessageCells(msg))

	case peerwire.OpReply:
		// from, and the message being answered as a full row
		if len(cells) != 1+peerwire.MessageWidth {
			return nil, nil
		}
		answered, err := peerwire.MessageRows(cells[1:])
		if err != nil || len(answered) != 1 {
			return refuse(peerwire.StatusBadRequest)
		}
		// The reply's own text rides in the answered row's text cell — the
		// caller sends what it is replying to plus what it is saying, and the
		// registry re-stamps provenance either way, so a forged row cannot
		// impersonate anyone.
		msg, replyErr := c.registry.Reply(cells[0], answered[0], answered[0].Text)
		if replyErr != nil {
			return refuse(peerwire.StatusFor(replyErr))
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, peerwire.MessageCells(msg))

	case peerwire.OpCancelWait:
		if len(cells) != 1 {
			return nil, nil
		}
		c.registry.CancelWait(cells[0])
		return peerwire.EncodeResponse(peerwire.StatusOK, nil)
	}
	return nil, nil
}

// ---- stage: inbox --------------------------------------------------------

func (c *PeerCapability) inbox(op uint32, cells []string) ([]byte, error) {
	if len(cells) < 1 {
		return nil, nil
	}
	// An unknown session is a REFUSAL, not an empty inbox. Those two are
	// indistinguishable in a message count, and conflating them is how a caller
	// polling for a reply waits forever on a session that no longer exists —
	// reading zero and going round again instead of failing fast. Every read
	// below is otherwise "I understood, and the answer is none", which is a
	// legitimate peerwire.StatusOK.
	if _, err := c.registry.Owner(cells[0]); err != nil {
		// Three outcomes, not two. A departed session is no_peer and the caller
		// should stop; a directory that did not answer is unavailable and the
		// caller should retry. Reporting the second as the first turns a
		// momentary outage into a permanent conclusion.
		return refuse(peerwire.StatusFor(err))
	}
	switch op {
	case peerwire.OpInboxLen:
		if len(cells) != 1 {
			return nil, nil
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, []string{
			peerwire.Itoa(c.registry.Len(cells[0])),
			peerwire.Itoa(int(c.registry.Dropped(cells[0]))),
		})

	case peerwire.OpInboxPeek:
		if len(cells) != 1 {
			return nil, nil
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, rows(c.registry.Inbox(cells[0])))

	case peerwire.OpInboxTake:
		if len(cells) != 2 {
			return nil, nil
		}
		max, err := peerwire.Atoi(cells[1])
		if err != nil {
			return refuse(peerwire.StatusBadRequest)
		}
		if max <= 0 {
			max = peer.InboxMax
		}
		// The reply leads with how many messages REMAIN, then the rows.
		// Rows alone cannot tell "that was all of it" from "that was the first
		// max of more", and a caller that drains once and assumes empty simply
		// stops asking. Complete and capped are two facts; one length carries
		// only one of them.
		taken, remaining := c.registry.Drain(cells[0], max)
		out := append([]string{peerwire.Itoa(remaining)}, rows(taken)...)
		return peerwire.EncodeResponse(peerwire.StatusOK, out)
	}
	return nil, nil
}

// rows flattens messages into cells. A list reply carries no row count: the
// caller divides by the row width, which is why the width must never change
// except by appending.
func rows(msgs []peer.Message) []string {
	out := make([]string, 0, len(msgs)*peerwire.MessageWidth)
	for _, m := range msgs {
		out = append(out, peerwire.MessageCells(m)...)
	}
	return out
}

// ---- stage: grant --------------------------------------------------------

func (c *PeerCapability) grant(op uint32, cells []string) ([]byte, error) {
	if len(cells) != 2 {
		return nil, nil
	}
	switch op {
	case peerwire.OpGrant:
		if err := c.registry.Grant(cells[0], cells[1]); err != nil {
			return refuse(peerwire.StatusFor(err))
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, nil)
	case peerwire.OpRevoke:
		return peerwire.EncodeResponse(peerwire.StatusOK, []string{peerwire.Btoa(c.registry.Revoke(cells[0], cells[1]))})
	case peerwire.OpGrantExists:
		return peerwire.EncodeResponse(peerwire.StatusOK, []string{peerwire.Btoa(c.registry.GrantExists(cells[0], cells[1]))})
	}
	return nil, nil
}

// ---- stage: channel ------------------------------------------------------

func (c *PeerCapability) channel(op uint32, cells []string) ([]byte, error) {
	switch op {
	case peerwire.OpChannelJoin:
		if len(cells) != 2 {
			return nil, nil
		}
		if err := c.registry.ChannelJoin(cells[0], cells[1]); err != nil {
			return refuse(peerwire.StatusFor(err))
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, nil)

	case peerwire.OpChannelLeave:
		if len(cells) != 2 {
			return nil, nil
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, []string{peerwire.Btoa(c.registry.ChannelLeave(cells[0], cells[1]))})

	case peerwire.OpChannelMembers:
		if len(cells) != 1 {
			return nil, nil
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, c.registry.ChannelMembers(cells[0]))

	case peerwire.OpChannelSend:
		// from, channel, text, conversation_id, hop
		if len(cells) != 5 {
			return nil, nil
		}
		hop, err := peerwire.Atoi(cells[4])
		if err != nil {
			return refuse(peerwire.StatusBadRequest)
		}
		deliveries, sendErr := c.registry.ChannelSend(cells[0], cells[1], cells[2], peer.SendOptions{
			ConversationID: cells[3],
			Hop:            hop,
		})
		if sendErr != nil {
			return refuse(peerwire.StatusFor(sendErr))
		}
		// peerwire.StatusOK here means the FAN-OUT was performed, not that every
		// recipient received it. Per-recipient outcomes ride in the rows, so a
		// partial delivery is visible rather than collapsed into one word.
		out := make([]string, 0, len(deliveries)*peerwire.DeliveryWidth)
		for _, d := range deliveries {
			out = append(out, peerwire.DeliveryCells(d)...)
		}
		return peerwire.EncodeResponse(peerwire.StatusOK, out)
	}
	return nil, nil
}
