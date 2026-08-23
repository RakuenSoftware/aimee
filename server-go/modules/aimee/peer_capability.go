package aimee

import (
	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
)

// DirectorySource resolves peer directory questions against their owner — db1
// in production. Absent, the registry's own in-memory view answers, which
// exists for tests and for bringing a process up before db1 is reachable; it is
// never a second source of truth in production.
//
// The db1 implementation needs NO new operations and no new stage. The existing
// db1-sessions family (stage 6, kind 11782) already covers this from
// server_sessions, whose columns map onto the directory exactly — session id to
// `id`, owner to `principal`, label to `title`, surface to `client_type`:
//
//	Resolve  -> server_session_search_by_title (scoped to the caller's principal)
//	Owner    -> server_session_get
//	listing  -> server_session_list_recent
//
// Calling that catalog rather than adding peer-shaped operations is the point:
// two directories would mean two answers to "which sessions exist", and the
// interesting failure is the one row where they disagree.
type DirectorySource interface {
	// Resolve maps (owner, label) to a session id.
	Resolve(owner, label string) (string, bool)
	// Owner reports a session's owner principal.
	Owner(sessionID string) (string, bool)
}

// PeerCapability serves session peer messaging: the verb by which one aimee
// session addresses another as a peer rather than as a delegate or a client.
//
// It owns inboxes and cross-owner grants and nothing else — notably not the
// session directory, which is db1's (see DirectorySource).
type PeerCapability struct {
	registry *peer.Registry
	dir      DirectorySource
}

// NewPeer builds the peer-messaging capability. dir may be nil, in which case
// the registry answers directory questions from its own map, which is a test
// and bring-up fallback rather than a source of truth.
//
// When dir IS given it is bound into the registry, so label resolution has one
// path whichever surface asked. Leaving them separate is how a module ends up
// authoritative for a caller arriving over the bus and guessing for the same
// caller arriving over HTTP.
func NewPeer(registry *peer.Registry, dir DirectorySource) *PeerCapability {
	if dir != nil && registry != nil {
		registry.SetResolver(dir.Resolve)
		registry.SetSessionExists(func(sessionID string) bool {
			_, ok := dir.Owner(sessionID)
			return ok
		})
	}
	return &PeerCapability{registry: registry, dir: dir}
}

// Stages are the peer stages. Event kinds are derived from the bus formula
// rather than written out, so identity and advertisement cannot drift.
func (c *PeerCapability) Stages() []bus.ModuleStage {
	return []bus.ModuleStage{
		{EventKind: EventDelivery, StageID: StageDelivery},
		{EventKind: EventInbox, StageID: StageInbox},
		{EventKind: EventGrant, StageID: StageGrant},
		{EventKind: EventChannel, StageID: StageChannel},
	}
}

// Handle serves one peer invocation.
func (c *PeerCapability) Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	op, cells, err := DecodeRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	var body []byte
	var encErr error
	switch invocation.StageID {
	case StageDelivery:
		body, encErr = c.delivery(op, cells)
	case StageInbox:
		body, encErr = c.inbox(op, cells)
	case StageGrant:
		body, encErr = c.grant(op, cells)
	case StageChannel:
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
func refuse(status Status) ([]byte, error) { return EncodeResponse(status, nil) }

// ---- stage: delivery -----------------------------------------------------

func (c *PeerCapability) delivery(op uint32, cells []string) ([]byte, error) {
	switch op {
	case OpSend:
		// from, to, text, conversation_id, hop, expect_reply
		if len(cells) != 6 {
			return nil, nil
		}
		hop, err := atoi(cells[4])
		if err != nil {
			return refuse(StatusBadRequest)
		}
		msg, sendErr := c.registry.Send(cells[0], cells[1], cells[2], peer.SendOptions{
			ConversationID: cells[3],
			Hop:            hop,
			ExpectReply:    atob(cells[5]),
		})
		if sendErr != nil {
			return refuse(StatusFor(sendErr))
		}
		return EncodeResponse(StatusOK, MessageCells(msg))

	case OpReply:
		// from, and the message being answered as a full row
		if len(cells) != 1+messageWidth {
			return nil, nil
		}
		answered, err := MessageRows(cells[1:])
		if err != nil || len(answered) != 1 {
			return refuse(StatusBadRequest)
		}
		// The reply's own text rides in the answered row's text cell — the
		// caller sends what it is replying to plus what it is saying, and the
		// registry re-stamps provenance either way, so a forged row cannot
		// impersonate anyone.
		msg, replyErr := c.registry.Reply(cells[0], answered[0], answered[0].Text)
		if replyErr != nil {
			return refuse(StatusFor(replyErr))
		}
		return EncodeResponse(StatusOK, MessageCells(msg))

	case OpCancelWait:
		if len(cells) != 1 {
			return nil, nil
		}
		c.registry.CancelWait(cells[0])
		return EncodeResponse(StatusOK, nil)
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
	// legitimate StatusOK.
	if !c.registry.Exists(cells[0]) {
		return refuse(StatusNoPeer)
	}
	switch op {
	case OpInboxLen:
		if len(cells) != 1 {
			return nil, nil
		}
		return EncodeResponse(StatusOK, []string{
			itoa(c.registry.Len(cells[0])),
			itoa(int(c.registry.Dropped(cells[0]))),
		})

	case OpInboxPeek:
		if len(cells) != 1 {
			return nil, nil
		}
		return EncodeResponse(StatusOK, rows(c.registry.Inbox(cells[0])))

	case OpInboxTake:
		if len(cells) != 2 {
			return nil, nil
		}
		max, err := atoi(cells[1])
		if err != nil {
			return refuse(StatusBadRequest)
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
		out := append([]string{itoa(remaining)}, rows(taken)...)
		return EncodeResponse(StatusOK, out)
	}
	return nil, nil
}

// rows flattens messages into cells. A list reply carries no row count: the
// caller divides by the row width, which is why the width must never change
// except by appending.
func rows(msgs []peer.Message) []string {
	out := make([]string, 0, len(msgs)*messageWidth)
	for _, m := range msgs {
		out = append(out, MessageCells(m)...)
	}
	return out
}

// ---- stage: grant --------------------------------------------------------

func (c *PeerCapability) grant(op uint32, cells []string) ([]byte, error) {
	if len(cells) != 2 {
		return nil, nil
	}
	switch op {
	case OpGrant:
		if err := c.registry.Grant(cells[0], cells[1]); err != nil {
			return refuse(StatusFor(err))
		}
		return EncodeResponse(StatusOK, nil)
	case OpRevoke:
		return EncodeResponse(StatusOK, []string{btoa(c.registry.Revoke(cells[0], cells[1]))})
	case OpGrantExists:
		return EncodeResponse(StatusOK, []string{btoa(c.registry.GrantExists(cells[0], cells[1]))})
	}
	return nil, nil
}

// ---- stage: channel ------------------------------------------------------

func (c *PeerCapability) channel(op uint32, cells []string) ([]byte, error) {
	switch op {
	case OpChannelJoin:
		if len(cells) != 2 {
			return nil, nil
		}
		if err := c.registry.ChannelJoin(cells[0], cells[1]); err != nil {
			return refuse(StatusFor(err))
		}
		return EncodeResponse(StatusOK, nil)

	case OpChannelLeave:
		if len(cells) != 2 {
			return nil, nil
		}
		return EncodeResponse(StatusOK, []string{btoa(c.registry.ChannelLeave(cells[0], cells[1]))})

	case OpChannelMembers:
		if len(cells) != 1 {
			return nil, nil
		}
		return EncodeResponse(StatusOK, c.registry.ChannelMembers(cells[0]))

	case OpChannelSend:
		// from, channel, text, conversation_id, hop
		if len(cells) != 5 {
			return nil, nil
		}
		hop, err := atoi(cells[4])
		if err != nil {
			return refuse(StatusBadRequest)
		}
		deliveries, sendErr := c.registry.ChannelSend(cells[0], cells[1], cells[2], peer.SendOptions{
			ConversationID: cells[3],
			Hop:            hop,
		})
		if sendErr != nil {
			return refuse(StatusFor(sendErr))
		}
		// StatusOK here means the FAN-OUT was performed, not that every
		// recipient received it. Per-recipient outcomes ride in the rows, so a
		// partial delivery is visible rather than collapsed into one word.
		out := make([]string, 0, len(deliveries)*deliveryWidth)
		for _, d := range deliveries {
			out = append(out, DeliveryCells(d)...)
		}
		return EncodeResponse(StatusOK, out)
	}
	return nil, nil
}
