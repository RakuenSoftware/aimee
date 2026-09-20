package memory

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

func TestEpisodeCardParserAndConfiguration(t *testing.T) {
	raw := `{"session_id":"sess_abc","title":"Camping trip with family","participants":["Caroline","Melanie"],"places":["Yosemite"],"events":["arrived May 1","hiked Half Dome May 2"],"outcomes":["everyone safe"],"open_threads":["returning in fall"]}`
	card, err := parseEpisodeCard([]byte(raw))
	if err != nil || card.SessionID != "sess_abc" || !strings.Contains(card.text(), "Caroline, Melanie") || !strings.Contains(card.text(), "everyone safe") {
		t.Fatal(card, err)
	}
	for _, bad := range []string{"not json", `{"participants":["Alice"]}`, `{"title":" "}`, `{"title":"ok","events":"bad"}`, raw + " trailing", strings.Repeat("x", episodeMaxOutput+1)} {
		if _, err := parseEpisodeCard([]byte(bad)); err == nil {
			t.Fatal("accepted malformed card", bad[:min(len(bad), 50)])
		}
	}
	// Neither disabled summaries nor an absent command may query or mutate the
	// database. This backend deliberately has no database capability.
	backend := &postgresDataStore{}
	for _, settings := range []map[string]any{nil, {"memory_episode_summaries_enabled": false, "memory_cognify_command": "must not execute"}, {"memory_episode_summaries_enabled": true}} {
		backend.settings = func() (map[string]any, error) { return settings, nil }
		if _, err := backend.GenerateEpisodeCard(context.Background(), "session"); !errors.Is(err, errEpisodeDisabled) {
			t.Fatal(err)
		}
	}
}

func TestEpisodeCommandPreservesInputAndBounds(t *testing.T) {
	input := []byte(`{"title":"literal $(printf unsafe) ` + "`command`" + `"}`)
	out, err := runEpisodeCommand(context.Background(), "cat", input)
	if err != nil || string(out) != string(input) {
		t.Fatal(string(out), err)
	}
	if _, err := runEpisodeCommand(context.Background(), "head -c 70000 /dev/zero", nil); err == nil {
		t.Fatal("unbounded output accepted")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	start := time.Now()
	if _, err := runEpisodeCommand(ctx, "sleep 2", nil); err == nil || time.Since(start) > time.Second {
		t.Fatal("deadline ignored", err)
	}
}
