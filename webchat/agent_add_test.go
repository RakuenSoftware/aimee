package main

import "testing"

func TestArgsHaveLiteralKey(t *testing.T) {
	cases := []struct {
		name string
		args []string
		want bool
	}{
		{"no key", []string{"d", "https://h/v1", "gpt-5", "--provider", "openai"}, false},
		{"literal key spaced", []string{"d", "-", "m", "--key", "sk-abc123"}, true},
		{"literal key equals", []string{"d", "-", "m", "--key=sk-abc123"}, true},
		{"env ref spaced", []string{"d", "-", "m", "--key", "$OPENAI_KEY"}, false},
		{"env ref equals", []string{"d", "-", "m", "--key=$OPENAI_KEY"}, false},
		{"empty key value", []string{"d", "-", "m", "--key", ""}, false},
		{"key flag last no value", []string{"d", "-", "m", "--key"}, false},
		{"whitespace-only value", []string{"d", "-", "m", "--key", "   "}, false},
		{"nil args", nil, false},
	}
	for _, c := range cases {
		if got := argsHaveLiteralKey(c.args); got != c.want {
			t.Errorf("%s: argsHaveLiteralKey(%v) = %v, want %v", c.name, c.args, got, c.want)
		}
	}
}
