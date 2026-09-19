// Package audit implements the shared observability action wire. The daemon
// owns the ledger and capture sinks; Go producers send the same bounded events
// as native producers without linking memory to a native audit hook.
package audit

import (
	"context"
	"encoding/binary"
	"errors"
	"time"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

const ActionKind uint32 = 3000

type Action struct {
	Actor, Tool, ArgsHash, Command, Mode, Reason, Verdict string
	TaskID                                                int64
}

// EncodeAction matches obs_bus.c's length-prefixed, little-endian wire. Reserve
// the terminating byte expected by the native reader, retain whole UTF-8 code
// points, and sanitize control bytes before either capture or ledger sees them.
func EncodeAction(a Action) []byte {
	fields := []string{a.Actor, a.Tool, a.ArgsHash, a.Command, a.Mode, a.Reason, a.Verdict}
	caps := []int{128, 256, 96, 512, 64, 128, 32}
	out := make([]byte, 0, 1252)
	for i, field := range fields {
		if len(field) >= caps[i] {
			end := caps[i] - 1
			for end > 0 && !utf8.RuneStart(field[end]) {
				end--
			}
			field = field[:end]
		}
		out = binary.LittleEndian.AppendUint32(out, uint32(len(field)))
		for j := 0; j < len(field); j++ {
			c := field[j]
			if c < 0x20 || c == 0x7f {
				c = '?'
			}
			out = append(out, c)
		}
	}
	return binary.LittleEndian.AppendUint64(out, uint64(a.TaskID))
}

type Publisher interface{ Publish(uint32, []byte) error }

// PublishAction retries ring backpressure within the caller's deadline. Success
// means enqueued, not a durability acknowledgement. Durable mutation records
// remain the storage owner's responsibility; callers must report publish errors
// without pretending an already committed mutation was rolled back.
func PublishAction(ctx context.Context, p Publisher, a Action) error {
	body := EncodeAction(a)
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		err := p.Publish(ActionKind, body)
		if !errors.Is(err, bus.ErrWouldBlock) {
			return err
		}
		timer := time.NewTimer(time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
}
