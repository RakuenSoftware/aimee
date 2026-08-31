// Package db1 is the shared caller-side contract for the DB1 store's bounded
// stages. It is deliberately outside modules/: any peer that reads or writes
// DB1 state exchanges these frames over the bus without importing DB1's
// implementation, which is the whole point of putting the store behind a
// module. Independently exported callers must also be listed by
// scripts/export_c_repositories.py:go_process_shared_sources.
//
// The wire is fixed by the catalog, server-go/modules/aimee/operations.json,
// and the module that serves it checks its own dispatch against that catalog.
// Keep this client in step with it:
// the C module is the serving side of exactly these bytes.
package db1

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"time"
)

const (
	// Carved from DB1's principal ref 30 as 4096 + ref*256 + stage.
	EventState uint32 = 11777
	StageState uint32 = 1

	opStateLoad uint32 = 1
	opStateSave uint32 = 2

	// StateMax bounds a reducer state blob. The module refuses an over-long
	// value rather than truncating one, so callers get an error instead of
	// silently corrupted state.
	StateMax = 6144

	// KeyMax is the module's key buffer. Stated here so an over-long key fails
	// before it costs a round trip.
	KeyMax = 511

	DefaultDeadline = 5 * time.Second
)

// Status codes the module reports in-band. A missing key is not an error: the
// first turn of a conversation has no state and the caller starts cold.
const (
	statusOK      uint32 = 0
	statusMissing uint32 = 1
	statusInvalid uint32 = 2
	statusTooLong uint32 = 3
	statusFailed  uint32 = 4
)

var (
	ErrConfig       = errors.New("db1 bus client is not configured")
	ErrInvalidKey   = errors.New("db1 state key is empty, over-long, or contains NUL")
	ErrStateTooLong = errors.New("db1 state blob exceeds the wire cap")
	ErrMalformed    = errors.New("db1 module returned a malformed response")
)

// StatusError is a refusal the module reported rather than a transport failure.
type StatusError struct {
	Op     string
	Status uint32
}

func (e *StatusError) Error() string {
	return fmt.Sprintf("db1 %s refused with status %d", e.Op, e.Status)
}

// StageCaller is the bus call this contract needs, kept as an interface so
// callers can seat it on any caller implementation and tests need no bus.
type StageCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

type Client struct {
	caller   StageCaller
	deadline time.Duration
}

func NewClient(caller StageCaller, deadline time.Duration) (*Client, error) {
	if caller == nil {
		return nil, ErrConfig
	}
	if deadline <= 0 {
		deadline = DefaultDeadline
	}
	return &Client{caller: caller, deadline: deadline}, nil
}

// validKey mirrors the module's own check. A key is spliced into a query
// parameter, so an embedded NUL would silently shorten it.
func validKey(key string) bool {
	return key != "" && len(key) <= KeyMax && !strings.Contains(key, "\x00")
}

func encode(op uint32, key, payload string) []byte {
	frame := make([]byte, 0, 12+len(key)+len(payload))
	var scratch [4]byte
	put := func(v uint32) {
		binary.LittleEndian.PutUint32(scratch[:], v)
		frame = append(frame, scratch[:]...)
	}
	put(op)
	put(uint32(len(key)))
	frame = append(frame, key...)
	put(uint32(len(payload)))
	frame = append(frame, payload...)
	return frame
}

// decode splits a response into its status and payload, refusing any frame
// whose declared length does not match what actually arrived.
func decode(response []byte) (uint32, string, error) {
	if len(response) < 8 {
		return 0, "", ErrMalformed
	}
	status := binary.LittleEndian.Uint32(response)
	length := binary.LittleEndian.Uint32(response[4:8])
	if uint64(length) != uint64(len(response)-8) {
		return 0, "", ErrMalformed
	}
	return status, string(response[8:]), nil
}

// LoadState returns the stored blob for key. A miss returns ("", false, nil):
// absence is the normal first-turn case, not a failure.
func (c *Client) LoadState(ctx context.Context, key string) (string, bool, error) {
	if c == nil || c.caller == nil {
		return "", false, ErrConfig
	}
	if !validKey(key) {
		return "", false, ErrInvalidKey
	}
	if ctx == nil {
		ctx = context.Background()
	}
	response, err := c.caller.Call(ctx, EventState, StageState, 0, c.deadline,
		encode(opStateLoad, key, ""))
	if err != nil {
		return "", false, err
	}
	status, payload, err := decode(response)
	if err != nil {
		return "", false, err
	}
	switch status {
	case statusOK:
		return payload, true, nil
	case statusMissing:
		return "", false, nil
	default:
		return "", false, &StatusError{Op: "state load", Status: status}
	}
}

// SaveState stores blob under key. The cap is checked here as well as in the
// module so an over-long blob fails before it costs a round trip.
func (c *Client) SaveState(ctx context.Context, key, blob string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	if !validKey(key) {
		return ErrInvalidKey
	}
	if len(blob) >= StateMax {
		return ErrStateTooLong
	}
	if ctx == nil {
		ctx = context.Background()
	}
	response, err := c.caller.Call(ctx, EventState, StageState, 0, c.deadline,
		encode(opStateSave, key, blob))
	if err != nil {
		return err
	}
	status, _, err := decode(response)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "state save", Status: status}
	}
	return nil
}
