package sandbox

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// Every case below is ported from src/tests/test_sandbox_learned.c so the Go
// module is held to the C implementation's exact behaviour, not merely to a
// plausible reading of it. The parser is a security boundary; parity matters
// more than elegance.
func TestParseAptMatchesTheCImplementation(t *testing.T) {
	for name, tc := range map[string]struct {
		cmd  string
		want []string
	}{
		"plain install":            {"apt install gcc make", []string{"gcc", "make"}},
		"flags are not packages":   {"apt-get install -y --no-install-recommends libssl-dev cmake", []string{"libssl-dev", "cmake"}},
		"sudo and env prefix":      {"DEBIAN_FRONTEND=noninteractive sudo apt-get -y install pkg-config", []string{"pkg-config"}},
		"stops at operator":        {"apt-get install -y git && make && ./configure", []string{"git"}},
		"update then install":      {"apt-get update && apt-get install -y curl wget", []string{"curl", "wget"}},
		"non-install subcommands":  {"apt-get remove gcc; apt-get update; apt-get upgrade -y", nil},
		"version pin and path":     {"apt-get install -y gcc=4:12.2 libfoo/bookworm", []string{"gcc"}},
		"option with separate arg": {"apt-get install -t bookworm-backports curl", []string{"curl"}},
		"option before subcommand": {"apt-get -t bookworm install -o APT::x=1 htop", []string{"htop"}},
		"sudo with flags":          {"sudo -E apt-get install -y make", []string{"make"}},
		"local and url debs":       {"apt-get install -y ./local.deb https://h/p.deb wget", []string{"wget"}},
		"dedupes":                  {"apt install gcc gcc gcc", []string{"gcc"}},
		"apt inside another cmd":   {"echo apt install fake | tee /tmp/x", nil},
		"not apt at all":           {"pip install requests && npm install left-pad", nil},
	} {
		t.Run(name, func(t *testing.T) {
			got := ParseApt(tc.cmd, LearnMax)
			if len(got) != len(tc.want) {
				t.Fatalf("got %v, want %v", got, tc.want)
			}
			for _, w := range tc.want {
				if !contains(got, w) {
					t.Fatalf("missing %q in %v", w, got)
				}
			}
		})
	}
}

// Whatever survives an injection-shaped command must be a clean package name.
// This is the property the C test asserted, and it is the one that matters.
func TestParseAptNeverYieldsAnInjectableToken(t *testing.T) {
	for _, cmd := range []string{
		"apt-get install -y 'gcc; rm -rf /' $(whoami) ../etc/passwd",
		"apt-get install -y `id` $(cat /etc/passwd) pkg&&rm",
		"apt-get install -y ../../etc/shadow \"a b\" pkg|sh",
		"apt-get install -y $HOME/x ${EVIL} normalpkg",
	} {
		got := ParseApt(cmd, LearnMax)
		for _, name := range got {
			if name == "" || strings.HasPrefix(name, "-") {
				t.Fatalf("%q yielded a flag-shaped name %q", cmd, name)
			}
			if strings.ContainsAny(name, "/;$ `\"'|&*?<>(){}[]\\\n\t") {
				t.Fatalf("%q yielded an unsafe name %q", cmd, name)
			}
			if !packageValid(name) {
				t.Fatalf("%q yielded a name failing the grammar: %q", cmd, name)
			}
		}
	}
}

// The grammar is the security boundary, so pin it directly too.
func TestPackageValidGrammar(t *testing.T) {
	for _, ok := range []string{"gcc", "libssl-dev", "g++0", "python3.11", "a_b", "gcc:amd64", "x+y"} {
		if !packageValid(ok) {
			t.Fatalf("%q should be valid", ok)
		}
	}
	for _, bad := range []string{"", "-y", ".hidden", "/abs/path", "a/b", "a;b", "$(x)", "a b", "a\nb", "a*"} {
		if packageValid(bad) {
			t.Fatalf("%q must be rejected", bad)
		}
	}
}

func TestParseAptRespectsMax(t *testing.T) {
	got := ParseApt("apt install a b c d e", 3)
	if len(got) != 3 {
		t.Fatalf("expected 3, got %v", got)
	}
	if ParseApt("apt install gcc", 0) != nil {
		t.Fatal("max<=0 must yield nothing")
	}
	if ParseApt("", LearnMax) != nil {
		t.Fatal("empty command must yield nothing")
	}
}

// The store round-trip, ported from the C test including the sort and the
// project-isolation assertions.
func TestStoreRecordLoadRoundTrip(t *testing.T) {
	store, err := NewStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	const root = "/proj/alpha"

	if added, err := store.Record(root, []string{"gcc", "make"}); err != nil || added != 2 {
		t.Fatalf("first record: added=%d err=%v", added, err)
	}
	// "make" is a duplicate, "cmake" is new.
	if added, err := store.Record(root, []string{"make", "cmake"}); err != nil || added != 1 {
		t.Fatalf("second record: added=%d err=%v", added, err)
	}

	got := store.Load(root, LearnMax)
	// load sorts for a stable build hash.
	if want := []string{"cmake", "gcc", "make"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}

	// A different project is isolated.
	if _, err := store.Record("/proj/beta", []string{"nodejs"}); err != nil {
		t.Fatal(err)
	}
	if got := store.Load(root, LearnMax); !reflect.DeepEqual(got, []string{"cmake", "gcc", "make"}) {
		t.Fatalf("alpha leaked: %v", got)
	}
	if got := store.Load("/proj/beta", LearnMax); !reflect.DeepEqual(got, []string{"nodejs"}) {
		t.Fatalf("beta wrong: %v", got)
	}
}

func TestStoreRejectsInvalidNamesOnWriteAndRead(t *testing.T) {
	home := t.TempDir()
	store, err := NewStore(home)
	if err != nil {
		t.Fatal(err)
	}
	if added, err := store.Record("/p", []string{"../etc/passwd", "a;b", "good"}); err != nil || added != 1 {
		t.Fatalf("added=%d err=%v", added, err)
	}
	if got := store.Load("/p", LearnMax); !reflect.DeepEqual(got, []string{"good"}) {
		t.Fatalf("got %v", got)
	}

	// A hand-edited sidecar must not reach a generated Dockerfile.
	raw, _ := json.Marshal(map[string][]string{"/p": {"good", "evil; rm -rf /"}})
	if err := os.WriteFile(filepath.Join(home, storeName), raw, 0o600); err != nil {
		t.Fatal(err)
	}
	if got := store.Load("/p", LearnMax); !reflect.DeepEqual(got, []string{"good"}) {
		t.Fatalf("hand-edited store leaked: %v", got)
	}
}

// A broken sidecar is an empty one: best-effort learning must never fail a turn.
func TestStoreToleratesAnUnreadableSidecar(t *testing.T) {
	home := t.TempDir()
	store, _ := NewStore(home)
	if err := os.WriteFile(filepath.Join(home, storeName), []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	if got := store.Load("/p", LearnMax); got != nil {
		t.Fatalf("expected nothing, got %v", got)
	}
	if added, err := store.Record("/p", []string{"gcc"}); err != nil || added != 1 {
		t.Fatalf("record over a broken store: added=%d err=%v", added, err)
	}
	if got := store.Load("/p", LearnMax); !reflect.DeepEqual(got, []string{"gcc"}) {
		t.Fatalf("got %v", got)
	}
}

func TestStoreCapsAtLearnMax(t *testing.T) {
	store, _ := NewStore(t.TempDir())
	many := make([]string, 0, LearnMax+10)
	for i := 0; i < LearnMax+10; i++ {
		many = append(many, "pkg"+string(rune('a'+i%26))+string(rune('a'+i/26)))
	}
	if _, err := store.Record("/p", many); err != nil {
		t.Fatal(err)
	}
	if got := store.Load("/p", LearnMax+100); len(got) > LearnMax {
		t.Fatalf("stored %d, cap is %d", len(got), LearnMax)
	}
}

// --- stage handlers ---

func invoke(t *testing.T, store *Store, stage uint32, payload any) ([]byte, bus.ModuleStatus) {
	t.Helper()
	encoded, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	return NewHandler(store)(bus.ModuleInvocation{StageID: stage}, encoded)
}

func TestObserveStageParsesAndRecords(t *testing.T) {
	store, _ := NewStore(t.TempDir())
	body, status := invoke(t, store, StageObserve, ObserveRequest{
		GitRoot: "/proj", Command: "apt-get install -y gcc make",
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	var response ObserveResponse
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatal(err)
	}
	if response.Parsed != 2 || response.Recorded != 2 {
		t.Fatalf("parsed=%d recorded=%d", response.Parsed, response.Recorded)
	}
	// Recording the same command again learns nothing new.
	body, _ = invoke(t, store, StageObserve, ObserveRequest{
		GitRoot: "/proj", Command: "apt-get install -y gcc make",
	})
	_ = json.Unmarshal(body, &response)
	if response.Recorded != 0 {
		t.Fatalf("re-observe recorded %d, want 0", response.Recorded)
	}
}

func TestLoadStageReturnsTheRecordedSet(t *testing.T) {
	store, _ := NewStore(t.TempDir())
	if _, err := store.Record("/proj", []string{"make", "gcc"}); err != nil {
		t.Fatal(err)
	}
	body, status := invoke(t, store, StageLoad, LoadRequest{GitRoot: "/proj"})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status %v", status)
	}
	var response LoadResponse
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(response.Packages, []string{"gcc", "make"}) {
		t.Fatalf("got %v", response.Packages)
	}
}

func TestStagesRejectBadRequests(t *testing.T) {
	store, _ := NewStore(t.TempDir())
	if _, status := invoke(t, store, StageObserve, ObserveRequest{Command: "apt install gcc"}); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("missing git root must be rejected, got %v", status)
	}
	if _, status := invoke(t, store, StageLoad, LoadRequest{}); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("missing git root must be rejected, got %v", status)
	}
	if _, status := NewHandler(store)(bus.ModuleInvocation{StageID: 99}, []byte(`{}`)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown stage must be rejected, got %v", status)
	}
	if _, status := NewHandler(store)(bus.ModuleInvocation{StageID: StageObserve}, []byte("not json")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("undecodable request must be rejected, got %v", status)
	}
	if _, status := NewHandler(nil)(bus.ModuleInvocation{StageID: StageLoad}, []byte(`{"git_root":"/p"}`)); status != bus.ModuleStatusInternal {
		t.Fatalf("nil store must be internal, got %v", status)
	}
}

// The kind allocation is fixed by the process contract, not chosen.
func TestKindsMatchTheContractAllocation(t *testing.T) {
	const ordinal = 26
	if want := uint32(4096 + ordinal*256 + int(StageObserve)); EventObserve != want {
		t.Fatalf("EventObserve must be %d, got %d", want, EventObserve)
	}
	if want := uint32(4096 + ordinal*256 + int(StageLoad)); EventLoad != want {
		t.Fatalf("EventLoad must be %d, got %d", want, EventLoad)
	}
}

// A store written by the C implementation this module replaced, taken verbatim
// from a production server (aimee-prod) where a real delegate ran an apt install
// and the old code recorded it. The migration is only correct if the Go module
// reads what the C one wrote -- byte-identical file, same answer, including the
// git-root key shape that real workspaces produce.
func TestLoadReadsAStoreWrittenByTheCImplementation(t *testing.T) {
	home := t.TempDir()
	const production = `{"/var/lib/aimee-workspaces/environment/rakuensoftware/aimee-workflow":["git"]}`
	if err := os.WriteFile(filepath.Join(home, storeName), []byte(production), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := NewStore(home)
	if err != nil {
		t.Fatal(err)
	}
	const root = "/var/lib/aimee-workspaces/environment/rakuensoftware/aimee-workflow"
	if got := store.Load(root, LearnMax); !reflect.DeepEqual(got, []string{"git"}) {
		t.Fatalf("got %v, want [git]", got)
	}
	// An unrelated project must not inherit it.
	if got := store.Load("/var/lib/aimee-workspaces/other", LearnMax); got != nil {
		t.Fatalf("unrelated project got %v", got)
	}
	// And a further observe must union rather than replace what C left behind.
	added, err := store.Record(root, []string{"make"})
	if err != nil || added != 1 {
		t.Fatalf("added=%d err=%v", added, err)
	}
	if got := store.Load(root, LearnMax); !reflect.DeepEqual(got, []string{"git", "make"}) {
		t.Fatalf("after union got %v, want [git make]", got)
	}
}
