package delegates

import (
	"fmt"
	"strings"
)

// A delegate container's IDENTITY, decided where its shape is decided.
//
// `docker start` resumes by name. So a task id reused against a different
// workspace would resume a container carrying the OLD mounts, and the delegate
// would work in the previous tree with nothing to say so. Folding the mounts
// into the name makes a changed mount a different container by construction.
//
// That is precisely why this lives beside the spec rather than with the caller:
// the name exists to track the mounts, and a name computed from one set of
// mounts while a different set is rendered into the argv is the exact bug the
// fingerprint was added to prevent.

// containerNameMax bounds a container name. Matches the buffer the backend
// reads it into.
const containerNameMax = 127

const containerNamePrefix = "aimee-delegate-"

// sanitizeContainerName folds a task id into what docker accepts for a name:
// [a-zA-Z0-9_.-]. Everything else becomes '_'. The leading character must be
// alphanumeric, which the prefix already satisfies.
func sanitizeContainerName(taskID string) string {
	var b strings.Builder
	b.Grow(len(taskID))
	for i := 0; i < len(taskID); i++ {
		c := taskID[i]
		if isAlnum(c) || c == '_' || c == '.' || c == '-' {
			b.WriteByte(c)
			continue
		}
		b.WriteByte('_')
	}
	return b.String()
}

// mountsFingerprint hashes the rendered workspace binds (FNV-1a, 32-bit).
//
// It hashes the SAME strings that reach the argv, via renderBind, so the name
// cannot describe one set of mounts while another is created.
//
// Workspace mounts only, matching what the backend has always fingerprinted.
// The control socket is deliberately excluded: it is the same channel for every
// delegate on the host, so including it would churn every container name the
// moment the socket moved, without ever distinguishing two delegates.
func mountsFingerprint(spec SandboxSpec, mountTable string) uint32 {
	var h uint32 = 2166136261
	for _, m := range spec.Mounts {
		if m.Kind != SandboxWorkspace {
			continue
		}
		for _, c := range []byte(renderBind(m, mountTable)) {
			h ^= uint32(c)
			h *= 16777619
		}
	}
	return h
}

// ContainerName is the name to create or resume this delegate's container under.
//
// The mount fingerprint is appended only when a host tree is actually mounted.
// A delegate with no workspace runs in the backend's scratch dir, which is
// derived from the task id already — there resuming by task id is the point,
// and a fingerprint would defeat it.
//
// When the name would overflow, the BODY is truncated and the suffix kept.
// Dropping the suffix instead would collide two different mount sets onto one
// name, which is the failure this whole mechanism exists to avoid.
func ContainerName(taskID string, spec SandboxSpec, mountTable string) (string, error) {
	taskID = strings.TrimSpace(taskID)
	if taskID == "" {
		return "", fmt.Errorf("task id is required to name a container")
	}

	name := containerNamePrefix + sanitizeContainerName(taskID)

	hasWorkspace := false
	for _, m := range spec.Mounts {
		if m.Kind == SandboxWorkspace {
			hasWorkspace = true
			break
		}
	}
	if !hasWorkspace {
		if len(name) > containerNameMax {
			name = name[:containerNameMax]
		}
		return name, nil
	}

	suffix := fmt.Sprintf("-%08x", mountsFingerprint(spec, mountTable))
	if len(name)+len(suffix) > containerNameMax {
		name = name[:containerNameMax-len(suffix)]
	}
	return name + suffix, nil
}
