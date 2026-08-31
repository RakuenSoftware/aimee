package git

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Provider kinds, mirroring ws_provider_kind_t. Only DETACHED is named in the
// decision below; the rest are here so a caller's value is readable in a trace.
const (
	ProviderShared    uint32 = 0
	ProviderDetached  uint32 = 1
	ProviderMirror    uint32 = 2
	ProviderContainer uint32 = 3
)

// CredResolveRequest carries the facts C holds; the decisions stay here.
//
// The caller passes the registered roots rather than this module reading them:
// where the list is stored is not a decision, and shipping it keeps the module
// free of config I/O. AimeeHome is passed for the same reason, and deliberately
// as the HOME rather than a mirror path — where a provider materialises a root
// is a decision, so it is derived here.
type CredResolveRequest struct {
	Cwd          string   `json:"cwd"`
	ProviderKind uint32   `json:"provider_kind"`
	AimeeHome    string   `json:"aimee_home"`
	Workspaces   []string `json:"workspaces"`
}

// CredResolveResponse answers both questions the git path has to ask before it
// execs: does this command run on the server, and if so which registered
// workspace owns the directory it runs in (so a credential can be keyed to it).
//
// Workspace is the REGISTERED root, never the materialised path, so credential
// keys do not change when a provider relocates where work happens.
type CredResolveResponse struct {
	RunsOnServer bool   `json:"runs_on_server"`
	Workspace    string `json:"workspace"`
}

// runsOnServer states the rule instead of listing the kinds that satisfy it.
//
// Only a detached workspace marshals git to its client, which holds both the
// filesystem authority and its own credentials; everything else runs on the
// server and therefore needs the server-side credential. Written the other way
// round — as a list of kinds — this silently excluded `mirror`, whose git runs
// on the server like any other, so every push from a mirror workspace exec'd
// with no GIT_ASKPASS and no tty and reported "could not read Username". That
// reads as a dead token; the token was fine. A list fails the same way for the
// next provider added, whereas the negative form makes the credentialed path
// the default and leaves only the deliberate exception to state.
func runsOnServer(kind uint32) bool { return kind != ProviderDetached }

// mirrorBase resolves where the mirror tier keeps its server-side trees:
// AIMEE_WORKSPACES_DIR when it names an absolute path (a deployment points this
// at a persistent volume), else <aimee home>/workspaces. Mirrors
// workspace_mirror_base(); the two are pinned to the same answer by the tests.
func mirrorBase(aimeeHome string) string {
	if env := os.Getenv("AIMEE_WORKSPACES_DIR"); strings.HasPrefix(env, "/") {
		return env
	}
	if aimeeHome == "" {
		return ""
	}
	return aimeeHome + "/workspaces"
}

// fnv1aHex8 reproduces the 8-hex FNV-1a the mirror tier keys its server-side
// trees by. It must agree with the C derivation exactly — the two are compared
// against shared vectors in the tests — because a disagreement here does not
// fail loudly: it silently reports "no workspace owns this path", and the caller
// concludes there is no credential to inject.
func fnv1aHex8(s string) string {
	var h uint32 = 2166136261
	for i := 0; i < len(s); i++ {
		h ^= uint32(s[i])
		h *= 16777619
	}
	return fmt.Sprintf("%08x", h)
}

// atOrUnder reports whether path is prefix itself or sits beneath it, comparing
// whole path components: "/srv/repo" must never claim "/srv/repo-backup".
func atOrUnder(path, prefix string) bool {
	if prefix == "" || len(prefix) > len(path) || path[:len(prefix)] != prefix {
		return false
	}
	return len(path) == len(prefix) || path[len(prefix)] == '/' || prefix[len(prefix)-1] == '/'
}

// workspaceForPath returns the registered root that owns path, or "".
//
// A registered root is not always where the work happens. A `mirror` workspace
// is registered under the CLIENT's path — a directory that does not exist on
// this server at all — while the server reconstructs a worktree under
// <mirror base>/<hash(root)>/ and runs there. Matching only the registered root
// therefore answers "no workspace" about the live checkout.
//
// The hashed PARENT is matched rather than the "work" directory: a live worktree
// is generation-qualified ("work-1-<digest>"), so comparing against
// "<base>/<hash>/work" misses the very directory git runs in.
func workspaceForPath(cwd, mirrorBase string, workspaces []string) string {
	if cwd == "" {
		return ""
	}
	for _, root := range workspaces {
		if root == "" {
			continue
		}
		if atOrUnder(cwd, root) {
			return root
		}
		if mirrorBase != "" && atOrUnder(cwd, mirrorBase+"/"+fnv1aHex8(root)) {
			return root
		}
	}
	return ""
}

// handleCredResolve serves git-credential-resolve (stage 5).
func handleCredResolve(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded CredResolveRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	response := CredResolveResponse{RunsOnServer: runsOnServer(decoded.ProviderKind)}
	if response.RunsOnServer {
		response.Workspace = workspaceForPath(decoded.Cwd, mirrorBase(decoded.AimeeHome),
			decoded.Workspaces)
	}
	encoded, err := json.Marshal(response)
	if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
