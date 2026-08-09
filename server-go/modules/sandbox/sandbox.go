// Package sandbox owns the learned apt toolchain for delegate sandboxes.
//
// A delegate sandbox is `--network none` by intent, so its toolchain has to be
// baked into the image at build time. Rather than make every project author a
// spec, aimee learns what a project actually needed: when a delegate runs
// `apt-get install <pkgs>` inside its sandbox, the package names are captured
// and recorded against the project's git root, and the next image build pre-bakes
// them.
//
// This was src/modules/sandbox/sandbox_learned.c. It is feature work, not
// communication substrate, so it belongs in a module rather than the C core.
// The store moved with it: this process owns sandbox-learned.json.
//
// SECURITY MODEL, carried over unchanged. The input is an UNTRUSTED,
// delegate-authored shell command. The tokenizer is deliberately a best-effort,
// QUOTE-UNAWARE lexical splitter: it does not interpret quotes, backslash
// escapes, command substitution, expansion or globbing -- the delegate's real
// shell does. That is safe because packageValid is THE security boundary: a
// token is recorded ONLY if it matches the strict Debian package-name grammar,
// so no shell metacharacter, quote, expansion, path or flag can be recorded.
// The command word must equal exactly "apt"/"apt-get". Worst case a benign but
// wrong name is recorded, which fails its own build and falls back to the base
// image; there is no path from this parser to shell execution.
package sandbox

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

const (
	// EventObserve and EventLoad are fixed by the process contract at
	// 4096 + ordinal*256 + stage; sandbox is ordinal 26.
	EventObserve uint32 = 10753
	StageObserve uint32 = 1
	EventLoad    uint32 = 10754
	StageLoad    uint32 = 2

	// PackageNameMax mirrors SBX_PKG_MAX: Debian caps package names near this.
	PackageNameMax = 64
	// LearnMax mirrors SBX_LEARN_MAX: retained packages per project.
	LearnMax = 128

	storeName = "sandbox-learned.json"
	// The C reader refused a store larger than 1 MiB rather than grow unbounded.
	maxStoreBytes = 1 << 20
)

// ObserveRequest asks that an untrusted command be parsed for apt installs and
// anything found recorded against GitRoot.
//
// The C caller keeps the cheap "does this even mention apt" prefilter, the
// config gate and git-root resolution, because those are config/core concerns.
type ObserveRequest struct {
	GitRoot string `json:"git_root"`
	Command string `json:"command"`
}

// ObserveResponse reports what was learned. Recorded counts only names that were
// new to the project's set.
type ObserveResponse struct {
	Parsed   int      `json:"parsed"`
	Recorded int      `json:"recorded"`
	Packages []string `json:"packages"`
}

// LoadRequest asks for a project's learned set.
type LoadRequest struct {
	GitRoot string `json:"git_root"`
}

// LoadResponse carries the learned set, sorted so the derived Dockerfile -- and
// thus the content-hash image tag -- is stable regardless of insertion order.
type LoadResponse struct {
	Packages []string `json:"packages"`
}

// packageValid is the Debian package-name grammar: leading alphanumeric, then a
// small punctuation whitelist. This is the security boundary; nothing else in
// this package may relax it.
func packageValid(s string) bool {
	if s == "" {
		return false
	}
	if !isAlnum(s[0]) {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if isAlnum(c) || c == '.' || c == '_' || c == '+' || c == ':' || c == '-' {
			continue
		}
		return false
	}
	return true
}

func isAlnum(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}

func isOperator(tok string) bool {
	switch tok {
	case "&&", "||", "|", ";", "&", "\n":
		return true
	}
	return false
}

// isEnvAssign reports a leading VAR=value, skipped before the command word.
func isEnvAssign(tok string) bool {
	eq := strings.IndexByte(tok, '=')
	if eq <= 0 {
		return false
	}
	for i := 0; i < eq; i++ {
		if !isAlnum(tok[i]) && tok[i] != '_' {
			return false
		}
	}
	return true
}

// aptOptionTakesArg reports an apt option whose ARGUMENT is a separate following
// token (`-t bookworm`). Attached forms (`-t=x`, `-oDebug=1`) carry their value
// in the same token and are skipped as plain flags, so they are not listed.
// Recognising these stops an option's value being mis-recorded as a package.
func aptOptionTakesArg(tok string) bool {
	switch tok {
	case "-t", "-o", "-c", "-a",
		"--target-release", "--default-release", "--option", "--config-file", "--arch":
		return true
	}
	return false
}

func isDelim(c byte) bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == '|' || c == '&'
}

// tokenize splits on whitespace and emits shell operators as their own tokens.
func tokenize(cmd string) []string {
	var tokens []string
	i := 0
	for i < len(cmd) {
		for i < len(cmd) && (cmd[i] == ' ' || cmd[i] == '\t' || cmd[i] == '\r') {
			i++
		}
		if i >= len(cmd) {
			break
		}
		start := i
		for i < len(cmd) && !isDelim(cmd[i]) {
			i++
		}
		if i > start {
			tokens = append(tokens, cmd[start:i])
		}
		if i >= len(cmd) {
			break
		}
		switch {
		case cmd[i] == '&' && i+1 < len(cmd) && cmd[i+1] == '&':
			tokens = append(tokens, "&&")
			i += 2
		case cmd[i] == '|' && i+1 < len(cmd) && cmd[i+1] == '|':
			tokens = append(tokens, "||")
			i += 2
		case cmd[i] == '|':
			tokens = append(tokens, "|")
			i++
		case cmd[i] == ';':
			tokens = append(tokens, ";")
			i++
		case cmd[i] == '&':
			tokens = append(tokens, "&")
			i++
		case cmd[i] == '\n':
			tokens = append(tokens, "\n")
			i++
		default:
			i++
		}
	}
	return tokens
}

// ParseApt extracts apt-install package names from an untrusted shell command.
// Recognises `apt install` and `apt-get install` (optionally behind sudo),
// collects the package-name tokens that follow, and validates each against the
// Debian grammar. De-dupes within the call. Pure.
func ParseApt(cmd string, max int) []string {
	if cmd == "" || max <= 0 {
		return nil
	}
	tokens := tokenize(cmd)
	var found []string
	i := 0
	for i < len(tokens) && len(found) < max {
		// Start of a segment: skip env assignments, sudo, and sudo's own flags.
		for i < len(tokens) && (isEnvAssign(tokens[i]) || tokens[i] == "sudo" ||
			(i > 0 && tokens[i-1] == "sudo" && strings.HasPrefix(tokens[i], "-"))) {
			i++
		}
		if i >= len(tokens) {
			break
		}
		if tokens[i] != "apt" && tokens[i] != "apt-get" {
			for i < len(tokens) && !isOperator(tokens[i]) {
				i++
			}
			if i < len(tokens) {
				i++
			}
			continue
		}
		i++ // consume apt/apt-get
		seenInstall := false
		for i < len(tokens) && !isOperator(tokens[i]) && len(found) < max {
			t := tokens[i]
			i++
			// A value-taking option consumes its following token so the value is
			// never mistaken for a package or a subcommand, in either position.
			if strings.HasPrefix(t, "-") && aptOptionTakesArg(t) {
				if i < len(tokens) && !isOperator(tokens[i]) {
					i++
				}
				continue
			}
			if !seenInstall {
				if t == "install" {
					seenInstall = true
				} else if strings.HasPrefix(t, "-") {
					continue // flag before the subcommand: apt-get -y install
				} else {
					break // a different subcommand (update/remove/...)
				}
				continue
			}
			if strings.HasPrefix(t, "-") {
				continue // -y, --no-install-recommends, attached -oDebug=1
			}
			// Reject anything path- or URL-shaped: a package name has no '/'.
			// Accept a `pkg=version` pin by taking the name up to '='.
			if strings.ContainsRune(t, '/') {
				continue
			}
			name := t
			if eq := strings.IndexByte(name, '='); eq >= 0 {
				name = name[:eq]
			}
			if len(name) >= PackageNameMax {
				name = name[:PackageNameMax-1]
			}
			if packageValid(name) && !contains(found, name) {
				found = append(found, name)
			}
		}
		if i < len(tokens) && isOperator(tokens[i]) {
			i++
		}
	}
	return found
}

func contains(list []string, want string) bool {
	for _, v := range list {
		if v == want {
			return true
		}
	}
	return false
}

// Store is the JSON sidecar under AIMEE_HOME, keyed by git root.
//
// The C implementation serialised writers with a process mutex plus an flock
// across processes sharing one AIMEE_HOME. Only this module writes the file now,
// so the mutex is the whole story -- which is the point of moving ownership.
type Store struct {
	path string
	mu   sync.Mutex
}

// NewStore roots the sidecar at home.
func NewStore(home string) (*Store, error) {
	if strings.TrimSpace(home) == "" {
		return nil, os.ErrInvalid
	}
	return &Store{path: filepath.Join(home, storeName)}, nil
}

// read returns the whole document. A missing, oversized or unparseable store is
// an empty one: this is best-effort, and a broken sidecar must never fail a
// delegate turn.
func (s *Store) read() map[string][]string {
	info, err := os.Stat(s.path)
	if err != nil || info.Size() <= 0 || info.Size() > maxStoreBytes {
		return map[string][]string{}
	}
	raw, err := os.ReadFile(s.path)
	if err != nil {
		return map[string][]string{}
	}
	doc := map[string][]string{}
	if err := json.Unmarshal(raw, &doc); err != nil {
		return map[string][]string{}
	}
	return doc
}

// Load returns a project's learned set, sorted for a stable image hash.
func (s *Store) Load(gitRoot string, max int) []string {
	if gitRoot == "" || max <= 0 {
		return nil
	}
	s.mu.Lock()
	doc := s.read()
	s.mu.Unlock()

	var out []string
	for _, name := range doc[gitRoot] {
		if len(out) >= max {
			break
		}
		// Validate on read as well as write: the file is on disk and a hand-edit
		// must not reach a generated Dockerfile.
		if packageValid(name) {
			out = append(out, name)
		}
	}
	sort.Strings(out)
	return out
}

// Record unions names into a project's set and persists. Returns how many were
// new. The write is atomic (unique temp + rename) so a crash cannot truncate the
// store into place.
func (s *Store) Record(gitRoot string, pkgs []string) (int, error) {
	if gitRoot == "" || len(pkgs) == 0 {
		return 0, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()

	doc := s.read()
	existing := doc[gitRoot]
	added := 0
	for _, name := range pkgs {
		if !packageValid(name) {
			continue
		}
		if len(existing) >= LearnMax {
			break
		}
		if contains(existing, name) {
			continue
		}
		existing = append(existing, name)
		added++
	}
	if added == 0 {
		return 0, nil
	}
	doc[gitRoot] = existing

	encoded, err := json.Marshal(doc)
	if err != nil {
		return 0, err
	}
	temp, err := os.CreateTemp(filepath.Dir(s.path), storeName+".tmp.*")
	if err != nil {
		return 0, err
	}
	tempName := temp.Name()
	if _, err := temp.Write(encoded); err != nil {
		temp.Close()
		os.Remove(tempName)
		return 0, err
	}
	if err := temp.Close(); err != nil {
		os.Remove(tempName)
		return 0, err
	}
	if err := os.Rename(tempName, s.path); err != nil {
		os.Remove(tempName)
		return 0, err
	}
	return added, nil
}
