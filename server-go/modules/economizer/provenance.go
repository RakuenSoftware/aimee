package economizer

import (
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"sync/atomic"
)

// Provenance capability — a single-use token binding a reduction to the exact
// bytes and the exact call it was issued for.
//
// Ported from src/modules/economizer/economizer_provenance.c.
//
// This is a security object, not bookkeeping. It answers "is this reduced
// payload the one that was authorized, for this tenant/task/call, produced by
// this transform version, and has it been used before?" — so a reduction cannot
// be replayed against a different call, swapped for different bytes, or consumed
// twice.

// ProvenanceMaxSource bounds the payload a capability may cover.
const ProvenanceMaxSource = 16 * 1024 * 1024

// ProvenanceBinding identifies the call a capability is valid for. Every field
// must be non-zero: a zero id is an unset id, and treating unset as "matches"
// would let a capability bind to anything.
type ProvenanceBinding struct {
	TenantID           uint64
	TaskID             uint64
	CallID             uint64
	SemanticContractID uint64
	TransformID        uint64
	TransformVersion   uint64
}

// Valid reports whether every field of the binding is set.
func (b ProvenanceBinding) Valid() bool {
	return b.TenantID != 0 && b.TaskID != 0 && b.CallID != 0 &&
		b.SemanticContractID != 0 && b.TransformID != 0 && b.TransformVersion != 0
}

// ProvenanceCapability is a single-use token over a specific payload.
type ProvenanceCapability struct {
	binding   ProvenanceBinding
	digest    [32]byte
	sourceLen int
	// consumed is atomic because a capability may be raced by concurrent
	// consumers; single-use has to hold under that race, not merely in the happy
	// path.
	consumed atomic.Bool
}

var (
	// ErrProvenanceInvalid covers every rejection reason deliberately: an
	// attacker learning WHICH check failed (wrong tenant vs wrong bytes vs
	// already consumed) is told more than they should be.
	ErrProvenanceInvalid = errors.New("economizer: provenance capability rejected")
)

// IssueProvenanceLocal mints a capability over source for the given binding.
func IssueProvenanceLocal(binding ProvenanceBinding, source []byte) (*ProvenanceCapability, error) {
	if !binding.Valid() || len(source) > ProvenanceMaxSource {
		return nil, ErrProvenanceInvalid
	}
	cap := &ProvenanceCapability{
		binding:   binding,
		digest:    sha256.Sum256(source),
		sourceLen: len(source),
	}
	return cap, nil
}

// Consume verifies the capability against the expected binding and the actual
// bytes, and marks it used. It succeeds at most once.
//
// The digest comparison is CONSTANT-TIME: a byte-at-a-time comparison leaks how
// much of a forged digest was correct, which is enough to construct one.
func (c *ProvenanceCapability) Consume(expected ProvenanceBinding, source []byte) error {
	if c == nil || !expected.Valid() || len(source) > ProvenanceMaxSource {
		return ErrProvenanceInvalid
	}
	if c.binding != expected || c.sourceLen != len(source) {
		return ErrProvenanceInvalid
	}
	actual := sha256.Sum256(source)
	if subtle.ConstantTimeCompare(c.digest[:], actual[:]) != 1 {
		return ErrProvenanceInvalid
	}
	// Compare-and-swap, so two concurrent consumers cannot both win.
	if !c.consumed.CompareAndSwap(false, true) {
		return ErrProvenanceInvalid
	}
	return nil
}

// Destroy releases the capability.
//
// The C original cleanses the struct because the digest lived in process memory
// the caller could later read. Go's GC gives no such guarantee, so this zeroes
// what it can and exists mainly so callers keep the same lifecycle shape; it is
// deliberately NOT relied on as a secrecy boundary.
func (c *ProvenanceCapability) Destroy() {
	if c == nil {
		return
	}
	for i := range c.digest {
		c.digest[i] = 0
	}
	c.binding = ProvenanceBinding{}
	c.sourceLen = 0
	c.consumed.Store(true) // a destroyed capability can never be consumed
}
