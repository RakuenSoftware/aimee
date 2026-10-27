package peer

import "errors"

// Channels: addressing sugar over Send, and deliberately nothing more.
//
// A channel owns membership and fan-out. It owns no history, no ordering
// guarantee stronger than each recipient's own inbox order, and no transport of
// its own -- every delivery is an ordinary peer message subject to the same
// authorization, hop ceiling and inbox bound. That is what keeps a channel from
// quietly becoming a second messaging path that governance would have to learn
// about separately.
const (
	// ChannelsMax bounds how many channels one registry carries.
	ChannelsMax = 32
	// ChannelMembersMax bounds one channel's membership, and so bounds the
	// fan-out a single send can produce.
	ChannelMembersMax = 32
	// ChannelNameMax bounds a channel name.
	ChannelNameMax = 64
)

var (
	ErrNoChannel      = errors.New("peer: no such channel")
	ErrNotMember      = errors.New("peer: sender is not a member of this channel")
	ErrChannelFull    = errors.New("peer: channel membership is full")
	ErrChannelsFull   = errors.New("peer: no room for another channel")
	ErrChannelNameBad = errors.New("peer: invalid channel name")
)

// Delivery is one recipient's outcome from a fan-out.
//
// ChannelSend reports PER RECIPIENT rather than collapsing to one status,
// because the interesting case is partial: a five-member channel where two
// deliveries were denied for want of a cross-owner grant and three landed. A
// single "sent" would hide exactly the thing the sender needs to know, and
// silent partial success is the failure shape this design keeps running into.
type Delivery struct {
	Session string
	Message Message // zero when Err is non-nil
	Err     error
}

// Delivered reports whether this recipient received the message.
func (d Delivery) Delivered() bool { return d.Err == nil }

func validChannelName(name string) bool {
	if name == "" || len(name) > ChannelNameMax {
		return false
	}
	for _, r := range name {
		ok := r == '-' || r == '_' || r == '.' ||
			(r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9')
		if !ok {
			return false
		}
	}
	return true
}

// ChannelJoin adds a session to a channel, creating it on first join.
// Joining twice is not an error: membership is a set.
func (r *Registry) ChannelJoin(name, sessionID string) error {
	if !validChannelName(name) || sessionID == "" {
		return ErrChannelNameBad
	}
	// Joining is admission too: a session the directory vouches for may join,
	// and one it does not know may not.
	if _, err := r.admit(sessionID); err != nil {
		return err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return ErrShutdown
	}
	if _, known := r.sessions[sessionID]; !known {
		return ErrNoPeer
	}
	members, exists := r.channels[name]
	if !exists {
		if len(r.channels) >= ChannelsMax {
			return ErrChannelsFull
		}
		members = make(map[string]bool)
		r.channels[name] = members
	}
	if members[sessionID] {
		return nil
	}
	if len(members) >= ChannelMembersMax {
		return ErrChannelFull
	}
	members[sessionID] = true
	return nil
}

// ChannelLeave removes a session, reporting whether it was a member. A channel
// with no members left is removed: an empty channel is indistinguishable from
// one that never existed, and keeping it would let the channel table fill with
// names nobody uses.
func (r *Registry) ChannelLeave(name, sessionID string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	members, exists := r.channels[name]
	if !exists || !members[sessionID] {
		return false
	}
	delete(members, sessionID)
	if len(members) == 0 {
		delete(r.channels, name)
	}
	return true
}

// ChannelMembers lists a channel's members, sorted for a stable answer.
func (r *Registry) ChannelMembers(name string) []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	members, exists := r.channels[name]
	if !exists {
		return nil
	}
	out := make([]string, 0, len(members))
	for id := range members {
		out = append(out, id)
	}
	sortStrings(out)
	return out
}

// Channels lists the channel names a session belongs to, sorted.
func (r *Registry) Channels(sessionID string) []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	var out []string
	for name, members := range r.channels {
		if members[sessionID] {
			out = append(out, name)
		}
	}
	sortStrings(out)
	return out
}

// ChannelSend fans a message out to every OTHER member of a channel.
//
// The sender must be a member: you address a channel you are in, and a
// non-member writing to a channel would be a way to reach sessions that never
// agreed to hear from it. Delivery to the sender itself is skipped rather than
// refused, since a session addressing itself is refused everywhere else here.
//
// One conversation id spans the fan-out, and every message goes out at
// opts.Hop + 1, so a cycle through a channel terminates on the same hop budget
// as a direct exchange rather than escaping it.
//
// The returned slice has one entry per intended recipient, in sorted order,
// each carrying its own outcome. The error return covers only whole-call
// failures -- an unknown sender, a missing channel, a body that is too long.
func (r *Registry) ChannelSend(from, name, text string, opts SendOptions) ([]Delivery, error) {
	if from == "" || !validChannelName(name) {
		return nil, ErrBadRequest
	}
	if err := validateText(text); err != nil {
		return nil, err
	}

	type pending struct {
		to  string
		msg Message
		err error
	}

	// Admitted through the directory, exactly as a direct send is.
	//
	// This was missed when Send gained admission, and the hardware run caught it:
	// the delivery stage answered from the directory while the channel stage
	// still answered from the local map, so one sender got two different verdicts
	// depending on which stage it used. A session the directory vouches for could
	// message a peer directly and not a channel.
	if _, err := r.admit(from); err != nil {
		if errors.Is(err, ErrNoPeer) {
			return nil, ErrUnknownSender
		}
		return nil, err
	}

	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return nil, ErrShutdown
	}
	fromS, known := r.sessions[from]
	if !known {
		r.mu.Unlock()
		return nil, ErrUnknownSender
	}
	members, exists := r.channels[name]
	if !exists {
		r.mu.Unlock()
		return nil, ErrNoChannel
	}
	if !members[from] {
		r.mu.Unlock()
		return nil, ErrNotMember
	}

	recipients := make([]string, 0, len(members))
	for id := range members {
		if id != from {
			recipients = append(recipients, id)
		}
	}
	sortStrings(recipients)

	conversation := opts.ConversationID
	if conversation == "" {
		conversation = r.nextID("conv")
	}

	results := make([]pending, 0, len(recipients))
	for _, to := range recipients {
		toS, live := r.sessions[to]
		if !live {
			// A member whose session is gone. Reported, not skipped: the sender
			// asked to reach it and did not.
			results = append(results, pending{to: to, err: ErrNoPeer})
			continue
		}
		msg, err := r.deliver(fromS, toS, deliverSpec{
			text:           text,
			conversationID: conversation,
			hop:            opts.Hop + 1,
		})
		results = append(results, pending{to: to, msg: msg, err: err})
	}
	r.mu.Unlock()

	out := make([]Delivery, 0, len(results))
	for _, p := range results {
		if p.err == nil {
			// Notify outside the lock, like every other delivery path.
			r.notify(p.to, p.msg)
		}
		out = append(out, Delivery{Session: p.to, Message: p.msg, Err: p.err})
	}
	return out, nil
}
