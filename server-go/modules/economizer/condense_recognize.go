package economizer

import "strings"

// Command recognition for tool-output condensation.
//
// Ported from tc_recognize in src/modules/economizer/tool_condense.c.
//
// Every ambiguity resolves toward UNRECOGNIZED or OPAQUE. A misrecognized
// command gets the WRONG family filter applied to its output, which silently
// deletes lines the operator needed — so the recognizer is deliberately timid.

// TCMaxTok bounds tokenization; enough to reach a command and its subcommand.
const TCMaxTok = 24

// TCOutcome is the recognition verdict.
type TCOutcome int

const (
	// TCUnrecognized: not a command we handle (or a compound/piped line) —
	// passthrough.
	TCUnrecognized TCOutcome = iota
	// TCOpaque: multiplexer/make/script — only a generic fallback may ever apply,
	// never a family rule.
	TCOpaque
	// TCRecognized: a command we intend to condense.
	TCRecognized
)

// TCRecoResult is the recognition outcome plus the routing tokens.
type TCRecoResult struct {
	Outcome TCOutcome
	Cmd     string // normalized inner command basename after wrapper unwrapping
	Sub     string // the subcommand token (git <sub>, cargo <sub>, …)
}

// tcTokenize splits a simple command line into argv, honoring quotes.
//
// Returns ok=false when the line contains a shell COMPOUND operator, a command
// substitution, or anything else whose output we cannot attribute to one
// command. That is a fail-open signal: the caller treats the line as
// unrecognized and passes the output through untouched.
func tcTokenize(s string) ([]string, bool) {
	var toks []string
	i := 0
	for i < len(s) {
		for i < len(s) && (s[i] == ' ' || s[i] == '\t') {
			i++
		}
		if i >= len(s) {
			break
		}
		// Compound operators / substitution / newline -> bail.
		switch s[i] {
		case '|', ';', '&', '\n', '`':
			return nil, false
		}
		if s[i] == '$' && i+1 < len(s) && s[i+1] == '(' {
			return nil, false
		}
		// A redirect ends the command portion; argv[0..] is already captured.
		if s[i] == '<' || s[i] == '>' {
			break
		}
		if len(toks) >= TCMaxTok {
			break // enough tokens to recognize the command
		}

		var tok strings.Builder
		for i < len(s) {
			c := s[i]
			if c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' ||
				c == '<' || c == '>' || c == '\n' {
				break
			}
			if c == '\'' || c == '"' {
				q := c
				i++
				for i < len(s) && s[i] != q {
					tok.WriteByte(s[i])
					i++
				}
				if i < len(s) && s[i] == q {
					i++
				}
				continue
			}
			tok.WriteByte(c)
			i++
		}
		toks = append(toks, tok.String())
	}
	return toks, true
}

// tcBase is the basename of a path token.
func tcBase(p string) string {
	if i := strings.LastIndexByte(p, '/'); i >= 0 {
		return p[i+1:]
	}
	return p
}

// tcWrapperSkip reports how many leading tokens to skip past a known
// single-command wrapper, or 0 if t is not one.
func tcWrapperSkip(t string, next string, hasNext bool) int {
	switch t {
	case "time", "nice", "nohup", "stdbuf", "npx":
		return 1
	}
	if hasNext {
		switch t {
		case "uv", "poetry", "pipenv":
			if next == "run" {
				return 2
			}
		case "bun":
			if next == "x" {
				return 2
			}
		case "pnpm", "npm", "yarn":
			if next == "exec" {
				return 2
			}
		}
	}
	return 0
}

var tcKnownCmds = map[string]bool{
	"git": true, "cargo": true, "go": true, "pytest": true, "jest": true,
	"vitest": true, "mocha": true, "ctest": true, "mvn": true, "gradle": true,
	"dotnet": true, "tsc": true, "eslint": true, "ruff": true, "mypy": true,
	"flake8": true, "rustc": true, "gcc": true, "g++": true, "cc": true,
	"clang": true, "clang++": true, "ls": true, "grep": true, "rg": true,
	"find": true, "npm": true, "yarn": true, "pnpm": true, "pip": true,
	"pip3": true, "cmake": true,
}

var tcSubcommandCmds = map[string]bool{
	"git": true, "cargo": true, "go": true, "dotnet": true, "npm": true,
	"yarn": true, "pnpm": true, "pip": true, "pip3": true,
}

// tcIsVarAssign reports whether t is a leading VAR=VALUE assignment.
func tcIsVarAssign(t string) bool {
	eq := strings.IndexByte(t, '=')
	if eq <= 0 || t[0] == '-' || strings.ContainsRune(t, '/') {
		return false
	}
	for i := 0; i < eq; i++ {
		c := t[i]
		if !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '_') {
			return false
		}
	}
	return true
}

// sudo short options that take a SEPARATE argument, so both the option and its
// value are consumed while unwrapping.
const sudoOptsWithArg = "ugpCrtTURhDP"

// TCRecognize classifies a shell command line.
//
// Strips leading VAR=VALUE assignments, `env` and its assignments, `sudo` and
// its options, and chained single-command wrappers, then classifies the inner
// command.
func TCRecognize(cmdline string) TCRecoResult {
	r := TCRecoResult{Outcome: TCUnrecognized}
	toks, ok := tcTokenize(cmdline)
	if !ok || len(toks) == 0 {
		return r // empty or compound -> passthrough
	}

	n := len(toks)
	i := 0
	for guard := 0; i < n && guard < TCMaxTok; guard++ {
		if tcIsVarAssign(toks[i]) {
			i++
			continue
		}
		b := tcBase(toks[i])
		if b == "env" {
			i++
			for i < n && tcIsVarAssign(toks[i]) {
				i++
			}
			continue
		}
		if b == "sudo" {
			i++
			for i < n && strings.HasPrefix(toks[i], "-") {
				o := toks[i]
				takesArg := len(o) == 2 && strings.IndexByte(sudoOptsWithArg, o[1]) >= 0
				i++
				if takesArg && i < n {
					i++
				}
			}
			continue
		}
		next, hasNext := "", false
		if i+1 < n {
			next, hasNext = toks[i+1], true
		}
		if skip := tcWrapperSkip(b, next, hasNext); skip > 0 {
			i += skip
			continue
		}
		break
	}
	if i >= n {
		return r
	}

	cmd := tcBase(toks[i])
	r.Cmd = cmd

	// Multiplexers, make, and shell interpreters are always OPAQUE: their output
	// is arbitrary, so only a generic fallback may ever apply.
	switch cmd {
	case "xargs", "make", "bash", "sh", "zsh":
		r.Outcome = TCOpaque
		return r
	}

	// MASQUERADE GUARD. Any path-prefixed invocation is OPAQUE, because a family
	// rule may only apply to a BARE command name resolved by the shell against
	// $PATH. Honouring the basename of a path would let ./git or /tmp/git — a
	// script that merely shares a known name — inherit that command's filter and
	// have its output silently rewritten.
	if strings.HasPrefix(toks[i], ".") || strings.ContainsRune(toks[i], '/') {
		r.Outcome = TCOpaque
		return r
	}

	if tcKnownCmds[cmd] {
		r.Outcome = TCRecognized
		if tcSubcommandCmds[cmd] {
			// The subcommand is a bare word: skip options and option-arguments
			// (paths with '/', or KEY=VALUE). Best-effort — the family rule
			// re-parses precisely.
			for j := i + 1; j < n; j++ {
				t := toks[j]
				if !strings.HasPrefix(t, "-") && !strings.ContainsRune(t, '/') &&
					!strings.ContainsRune(t, '=') {
					r.Sub = t
					break
				}
			}
		}
	}
	return r
}
