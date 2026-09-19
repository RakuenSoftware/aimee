package memory

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os/exec"
	"strings"
	"time"
)

var errEpisodeDisabled = errors.New("memory: episode summaries are disabled or no cognifier is configured")
var errEpisodeCapacity = errors.New("memory: episode sources exceed the bounded derivation capacity")

const episodeMaxSources = 200
const episodeMaxInput = 512 * 1024
const episodeMaxOutput = 64 * 1024

type episodeCard struct {
	SessionID    string   `json:"session_id"`
	Title        string   `json:"title"`
	Participants []string `json:"participants"`
	Places       []string `json:"places"`
	Events       []string `json:"events"`
	Outcomes     []string `json:"outcomes"`
	OpenThreads  []string `json:"open_threads"`
}

type episodeTurn struct {
	ID   int64  `json:"id"`
	Text string `json:"text"`
}

func parseEpisodeCard(raw []byte) (episodeCard, error) {
	var card episodeCard
	if len(raw) == 0 || len(raw) > episodeMaxOutput || json.Unmarshal(raw, &card) != nil || strings.TrimSpace(card.Title) == "" {
		return card, errors.New("memory: malformed episode card")
	}
	return card, nil
}

func (card episodeCard) text() string {
	field := func(items []string, separator string) string {
		if len(items) == 0 {
			return "(none)"
		}
		return strings.Join(items, separator)
	}
	return "Episode: " + card.Title + "\nParticipants: " + field(card.Participants, ", ") +
		"\nPlaces: " + field(card.Places, ", ") + "\nEvents:\n" + field(card.Events, "\n") +
		"\nOutcomes:\n" + field(card.Outcomes, "\n") + "\nOpen threads:\n" + field(card.OpenThreads, "\n")
}

type episodeOutput struct{ buffer bytes.Buffer }

func (out *episodeOutput) Write(p []byte) (int, error) {
	if out.buffer.Len()+len(p) > episodeMaxOutput {
		return 0, errors.New("memory: episode response exceeds bounds")
	}
	return out.buffer.Write(p)
}

func runEpisodeCommand(ctx context.Context, command string, input []byte) ([]byte, error) {
	cmd := exec.CommandContext(ctx, "sh", "-c", command)
	cmd.Stdin = bytes.NewReader(input)
	out := &episodeOutput{}
	cmd.Stdout, cmd.Stderr = out, io.Discard
	cmd.WaitDelay = 200 * time.Millisecond
	if err := cmd.Run(); err != nil {
		return nil, err
	}
	return out.buffer.Bytes(), nil
}

func (s *postgresDataStore) episodeCognifier() (string, error) {
	if s.settings == nil {
		return "", errEpisodeDisabled
	}
	values, err := s.settings()
	if err != nil {
		return "", err
	}
	enabled := false
	switch v := values["memory_episode_summaries_enabled"].(type) {
	case bool:
		enabled = v
	case float64:
		enabled = v != 0
	case int:
		enabled = v != 0
	}
	command, _ := values["memory_cognify_command"].(string)
	if !enabled || strings.TrimSpace(command) == "" {
		return "", errEpisodeDisabled
	}
	return command, nil
}

func (s *postgresDataStore) episodeCards(ctx context.Context, session string, limit int) ([]string, error) {
	rows, err := s.db.Query(ctx, `SELECT u.unit_text FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE m.source_session=$1 AND m.lifecycle_state='active' AND u.is_episode_card=1
 ORDER BY u.id DESC LIMIT $2`, session, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	cards := []string{}
	for rows.Next() {
		var card string
		if err := rows.Scan(&card); err != nil {
			return nil, err
		}
		cards = append(cards, card)
	}
	return cards, rows.Err()
}
