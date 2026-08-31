package economizer

import "testing"

// DIFFERENTIAL test against the real tc_recognize.
//
// Each row is the verbatim outcome/cmd/sub captured from the retired C
// implementation before removal. Outcome codes retain the old C enum values:
// 0 unrecognized, 1 opaque, 2 recognized.
func TestTCRecognizeMatchesC(t *testing.T) {
	cases := []struct {
		line    string
		outcome TCOutcome
		cmd     string
		sub     string
	}{
		{"git status", TCRecognized, "git", "status"},
		{"git -c x=1 status", TCRecognized, "git", "status"},
		{"cargo test --all", TCRecognized, "cargo", "test"},
		{"pytest -q", TCRecognized, "pytest", ""},
		{"ls -la", TCRecognized, "ls", ""},

		// Multiplexers and interpreters are OPAQUE: arbitrary output.
		{"make -j8", TCOpaque, "make", ""},
		{"xargs grep foo", TCOpaque, "xargs", ""},
		{"bash -c 'git status'", TCOpaque, "bash", ""},

		// MASQUERADE GUARD: a path-prefixed invocation never inherits a family
		// filter, even though its basename is a known command.
		{"./git status", TCOpaque, "git", ""},
		{"/usr/local/bin/git status", TCOpaque, "git", ""},

		// Wrapper unwrapping, including chains.
		{"sudo -u ci git status", TCRecognized, "git", "status"},
		{"env FOO=1 BAR=2 git status", TCRecognized, "git", "status"},
		{"FOO=1 git status", TCRecognized, "git", "status"},
		{"time git status", TCRecognized, "git", "status"},
		{"uv run pytest", TCRecognized, "pytest", ""},
		{"npm exec jest", TCRecognized, "jest", ""},
		{"bun x vitest", TCRecognized, "vitest", ""},
		{"npx tsc", TCRecognized, "tsc", ""},
		{"nice time sudo env A=1 git commit -m x", TCRecognized, "git", "commit"},

		// Compound lines and substitutions fail open to UNRECOGNIZED.
		{"git status | grep x", TCUnrecognized, "", ""},
		{"git status; ls", TCUnrecognized, "", ""},
		{"echo `id`", TCUnrecognized, "", ""},
		{"echo $(id)", TCUnrecognized, "", ""},

		// A redirect ends token collection; the command is still recognized.
		{"git status > out.txt", TCRecognized, "git", "status"},

		// An unknown command still reports its name, but is not recognized.
		{"unknowncmd --flag", TCUnrecognized, "unknowncmd", ""},

		{"", TCUnrecognized, "", ""},
		{"   ", TCUnrecognized, "", ""},

		{"go build ./...", TCRecognized, "go", "build"},
		{"npm run build", TCRecognized, "npm", "run"},
		{"pip3 install -r req.txt", TCRecognized, "pip3", "install"},
		{"grep -rn foo src/", TCRecognized, "grep", ""},
	}
	for _, c := range cases {
		got := TCRecognize(c.line)
		if got.Outcome != c.outcome || got.Cmd != c.cmd || got.Sub != c.sub {
			t.Errorf("TCRecognize(%q) = {%d %q %q}, want {%d %q %q}",
				c.line, got.Outcome, got.Cmd, got.Sub, c.outcome, c.cmd, c.sub)
		}
	}
}
