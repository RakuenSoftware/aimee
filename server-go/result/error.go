package result

import (
	"errors"
	"fmt"
)

// Go returns errors; a reply carries a code. This is the bridge, and the only
// place either has to know about the other.
//
// A handler does its work in idiomatic Go and returns an error. Where it
// encodes a reply, it converts -- once. Converting anywhere else is how a Go
// implementation grows integer return conventions inward, which is the thing
// this package exists to avoid rather than to spread.

// Error is a failure code presented as a Go error.
type Error struct{ Code Code }

func (e *Error) Error() string { return e.Code.String() }

// Is lets errors.Is match on the code rather than on the pointer, so
// errors.Is(err, result.Err(result.Denied)) works on a wrapped chain.
func (e *Error) Is(target error) bool {
	other, ok := target.(*Error)
	return ok && other.Code == e.Code
}

// Err presents c as an error, or nil when c is not a failure.
//
// A success is not an error, and neither is Pending: work that has not finished
// has not failed, and a caller that treats "still running" as an error will
// abandon something that was going to succeed.
func Err(c Code) error {
	if !c.Failed() {
		return nil
	}
	return &Error{Code: c}
}

// CodeOf reports the code an error carries.
//
// A nil error is Done, never Pending: a Go function that has returned has
// finished, whatever it was doing. Pending reaches Go only from something that
// models unfinished work explicitly -- a job row, a queue entry, a code read
// off a boundary -- and never from a return that already happened.
//
// An error carrying no code is Failed, which is the honest answer: something
// went wrong and this layer does not know which thing.
func CodeOf(err error) Code {
	if err == nil {
		return Done
	}
	var coded *Error
	if errors.As(err, &coded) {
		return coded.Code
	}
	return Failed
}

// String names the code, or reports an unknown one as itself rather than
// guessing at a name for it.
func (c Code) String() string {
	if name, ok := names[c]; ok {
		return name
	}
	return fmt.Sprintf("result %d", c)
}

var names = map[Code]string{
	Pending:         "pending",
	Done:            "done",
	DoneNoChange:    "done, no change",
	DoneAlready:     "done, already so",
	DonePartial:     "done in part",
	DoneEmpty:       "done, nothing to report",
	DoneRefused:     "done, refused",
	Failed:          "failed",
	Invalid:         "invalid",
	Denied:          "denied",
	Unauthenticated: "unauthenticated",
	Unavailable:     "unavailable",
	Conflict:        "conflict",
	NotFound:        "not found",
	TooLarge:        "too large",
	Timeout:         "timeout",
	Integrity:       "integrity",
	Cancelled:       "cancelled",
}
