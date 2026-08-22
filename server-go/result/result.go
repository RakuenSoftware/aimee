// Package result is what a module reply says happened.
//
// See docs/proposals/pending/module-reply-outcomes.md.
//
// A bus reply is bytes, and bus.ModuleStatus only says whether the CALL
// happened -- not whether the work succeeded. Every operation in the DB2
// catalog currently fakes the difference with an unsigned `acknowledged` or
// `outcome` field and a private mapping at each end that no schema describes.
// This is that vocabulary, written down once, carried as the reply's i32
// result field.
//
//	< 0   the work failed, and which failure
//	= 0   the work has not finished
//	> 0   the work succeeded, and which success
//
// Zero is not an outcome. It means queued, running, in flight -- a caller
// holding zero has not been told the result because there is not one yet. It is
// reserved so an operation that models unfinished work has a value for it, and
// so that no success ever collides with "still running".
//
// This is NOT for in-process Go. Go answers with `error` plus a value: which
// failure is a sentinel, which success is the value returned. Nothing here
// belongs in a Go signature. A Code appears where a value crosses to something
// that reads an integer -- the reply field -- and error.go is the only place
// that conversion happens.
package result

// Code is what a reply says happened. It crosses as the reply's i32 field.
type Code int32

// Pending: the work has not finished. Reserved, and never a success.
const Pending Code = 0

// Successes. The work finished and did what it was asked.
const (
	// Done: the effect was applied.
	Done Code = 1
	// DoneNoChange: it ran and nothing needed changing. An UPDATE whose WHERE
	// matched no rows is this.
	DoneNoChange Code = 2
	// DoneAlready: it was already so, so nothing needed to run. An ON CONFLICT
	// DO NOTHING that conflicted is this, and it is NOT DoneNoChange -- one
	// write matched nothing, the other never ran.
	DoneAlready Code = 3
	// DonePartial: part of the work finished; the remainder is reported
	// alongside and is not an error.
	DonePartial Code = 4
	// DoneEmpty: the answer is legitimately nothing. A read that found no rows
	// is this, and it exists so that stops being indistinguishable from a read
	// that could not run.
	DoneEmpty Code = 5
	// DoneRefused: the decision this was asked to make is no. A policy verdict,
	// an ontology check, a gate that ran and declined. The operation succeeded;
	// its answer was no.
	DoneRefused Code = 6
)

// Failures. The work did not finish.
const (
	// Failed: unspecified. Honest where nothing more is known.
	Failed Code = -1
	// Invalid: the request was malformed, out of range, or incoherent.
	Invalid Code = -2
	// Denied: authorization refused the caller. Distinct from DoneRefused --
	// this one stopped the operation from running.
	Denied Code = -3
	// Unauthenticated: there was no verified principal to authorize.
	Unauthenticated Code = -4
	// Unavailable: a dependency was absent -- no connection, no provider.
	Unavailable Code = -5
	// Conflict: lost a race, or a precondition moved underneath.
	Conflict Code = -6
	// NotFound: the named thing does not exist AND its absence is an error.
	// Where absence is the answer, that is DoneEmpty and positive; both exist
	// so the two stop being one.
	NotFound Code = -7
	// TooLarge: the input or the answer exceeds a stated bound.
	TooLarge Code = -8
	// Timeout: the deadline passed before an outcome.
	Timeout Code = -9
	// Integrity: stored state failed its own check.
	Integrity Code = -10
	// Cancelled: the caller withdrew before an outcome.
	Cancelled Code = -11
)

// Succeeded reports whether the work finished and succeeded.
func (c Code) Succeeded() bool { return c > 0 }

// Failed reports whether the work finished and failed.
func (c Code) Failed() bool { return c < 0 }

// Pending reports whether the work has not finished.
func (c Code) Pending() bool { return c == 0 }
