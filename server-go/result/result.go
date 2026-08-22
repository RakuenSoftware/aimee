// Package result is the integer a function in this project answers with.
//
// See docs/proposals/pending/project-result-code-convention.md, and
// src/headers/aimee_result.h, which this mirrors value for value.
//
//	< 0   failed, and the specific value says which failure
//	= 0   not finished: running, queued, in flight -- no outcome yet
//	> 0   succeeded, and the specific value says which success
//
// Every function in this project whose integer return is how the caller learns
// what happened -- not one subsystem's house style.
//
// UNLESS another standard already owns that number. Where the value is defined
// outside this project and read by something outside it, that standard wins and
// this one does not apply: HTTP status codes, POSIX errno, SQLSTATE, process
// exit status, signal numbers, and protocol wire statuses. The test is not "is
// this number small" but "did someone else define it". A function translating
// between the two worlds translates, and its own return follows this convention
// even when its argument does not.
//
// Three properties, each doing work:
//
// Zero is not an outcome. It means the statement is still running, the job is
// still queued, the process is alive with nothing decided. A caller holding a
// zero has not been told the result, because there is not one. This inverts the
// practice it replaces, where zero was the usual way to say "fine".
//
// A success that changed nothing is still a success, with its own number. An
// upsert whose row already held what it would have written, a delete of
// something already gone, an update whose WHERE matched nothing: all three ran
// correctly. They are DoneNoChange or DoneAlready, never zero.
//
// A determination is a success. An operation asked to decide something, which
// decides no, has succeeded: that is DoneRefused. Only a refusal that stopped
// the operation running -- an authorization check the caller failed -- is
// Denied and negative.
//
// This is NOT bus.ModuleStatus. That answers whether the call happened, on an
// unsigned envelope field, and stays separate: an operation that runs and fails
// answers ModuleStatusOK with a negative Code in its body. The call succeeded;
// the work did not.
//
// Adoption is not a flag day. A converted function and an unconverted one can
// call each other: -1 still means failure and a positive still means success.
// The only value whose meaning actually moves is zero.
package result

// Code is a result in the project convention. It crosses the module bus as an
// i32 field, so it is int32 rather than int.
type Code int32

// Pending is reserved project-wide: never a domain code, never a success, never
// an error. The work has not finished.
const Pending Code = 0

// Universal successes, 1..99.
const (
	// Done: completed; the effect was applied.
	Done Code = 1
	// DoneNoChange: completed; the statement ran and nothing needed changing.
	// An UPDATE whose WHERE matched no rows is this.
	DoneNoChange Code = 2
	// DoneAlready: completed; it was already so, so nothing needed to run. An
	// ON CONFLICT DO NOTHING that conflicted is this, and it is NOT
	// DoneNoChange -- one write matched nothing, the other never ran.
	DoneAlready Code = 3
	// DonePartial: completed for part of the work; the remainder is reported
	// alongside and is not an error.
	DonePartial Code = 4
	// DoneEmpty: completed; the answer is legitimately nothing. A read that
	// found no rows is this, and it exists so that answer stops being
	// indistinguishable from a read that could not run.
	DoneEmpty Code = 5
	// DoneRefused: completed; the decision this was asked to make is no. A
	// policy verdict, an ontology check, a gate that ran and declined.
	DoneRefused Code = 6
)

// Universal failures, -1..-99.
const (
	// Failed: unspecified. Permitted only where nothing more is known -- a
	// function still answering this is unfinished, not wrong.
	Failed Code = -1
	// Invalid: the request was malformed, out of range, or incoherent.
	Invalid Code = -2
	// Denied: authorization refused the caller. Distinct from DoneRefused:
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

// Domain bands, one hundred per domain. A domain code is used only where no
// universal code says it. Successes count up from the band, failures down from
// its negation.
//
// The bands are named for what the code does, not for which store or which
// binary it lives in. Storage is one Postgres, whoever hosts it, and the band
// does not move or change name when the schema owner does.
const (
	BandTenancy   Code = 100 // tenancy and identity
	BandStorage   Code = 200 // postgres, pools, transactions
	BandOrg       Code = 300 // budget, egress, rate, spend, telemetry
	BandCustody   Code = 400 // vault, enrolments, witness
	BandCodeIndex Code = 500 // code index and projects
	BandKBDoc     Code = 600 // knowledge base documents and ingest
	BandMemory    Code = 700 // memory and facts
	BandCSS       Code = 800 // css analysis and rendering
	BandTransport Code = 900 // http client, sockets
)

// Succeeded reports whether the work finished and succeeded.
func (c Code) Succeeded() bool { return c > 0 }

// Failed reports whether the work finished and failed.
func (c Code) Failed() bool { return c < 0 }

// Pending reports whether the work has not finished. Not an outcome either way.
func (c Code) Pending() bool { return c == 0 }
