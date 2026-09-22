package memory

import (
	"context"
	"encoding/json"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode"
)

type derivedSettings struct {
	CorefMode, Command string
	CorefWindow        int
	Negation           bool
}

func (s *postgresDataStore) derivedSettings() (derivedSettings, error) {
	values := map[string]any{}
	if s.settings != nil {
		var err error
		values, err = s.settings()
		if err != nil {
			return derivedSettings{}, err
		}
	}
	out := derivedSettings{CorefMode: "off", CorefWindow: 5, Negation: configNumber(values, "memory_negation_enabled") != 0}
	if mode, ok := values["memory_coref_mode"].(string); ok && mode != "" {
		out.CorefMode = mode
	}
	if n := int(configNumber(values, "memory_coref_window")); n > 0 {
		out.CorefWindow = n
	}
	out.Command, _ = values["memory_cognify_command"].(string)
	if mode := os.Getenv("AIMEE_MEMORY_COREF_MODE"); mode != "" {
		out.CorefMode = mode
	}
	if n, err := strconv.Atoi(os.Getenv("AIMEE_MEMORY_COREF_WINDOW")); err == nil {
		out.CorefWindow = n
	}
	out.CorefWindow = max(1, min(12, out.CorefWindow))
	return out, nil
}
func corefPronoun(content string) bool {
	for _, word := range derivedTokens(textBound(content, 1023), 32) {
		if wordIn(strings.Trim(word, ".,!?;:"), "she her he him they them their") {
			return true
		}
	}
	return false
}
func corefNames(text string) []string {
	chars := []rune(text)
	out := []string{}
	seen := map[string]bool{}
	for i := 0; i < len(chars) && len(out) < 8; {
		if !unicode.IsLetter(chars[i]) {
			i++
			continue
		}
		if !unicode.IsUpper(chars[i]) {
			for i < len(chars) && unicode.IsLetter(chars[i]) {
				i++
			}
			continue
		}
		words := []string{}
		for len(words) < 3 && i < len(chars) && unicode.IsUpper(chars[i]) {
			start := i
			for i < len(chars) && unicode.IsLetter(chars[i]) {
				i++
			}
			words = append(words, string(chars[start:i]))
			for i < len(chars) && chars[i] == ' ' {
				i++
			}
		}
		name := textBound(derivedCanonical(strings.Join(words, " ")), 127)
		if len(name) >= 3 && !derivedStop(name) && !wordIn(name, "she her he him they them their") && derivedMonth(name) == 0 && derivedWeekday(name) < 0 && !seen[name] {
			seen[name] = true
			out = append(out, name)
		}
	}
	return out
}

type corefPrior struct{ key, content string }

func heuristicCoref(prior []corefPrior) (string, string) {
	for _, p := range prior {
		names := corefNames(p.content)
		if len(names) == 0 {
			names = corefNames(p.key)
		}
		if len(names) == 1 {
			return names[0], "bound"
		}
		if len(names) > 1 {
			return "", "ambiguous"
		}
	}
	return "", "unbound"
}
func (s *postgresDataStore) refreshCoreference(ctx context.Context, id int64, content string, settings derivedSettings) error {
	// Coref rows are derived: disabling the resolver or removing the pronoun
	// must not retain a binding to a previous version of the note.
	if _, err := s.db.Exec(ctx, `DELETE FROM memory_entities WHERE memory_id=$1 AND role='coref'`, id); err != nil {
		return err
	}
	if !corefPronoun(content) || (settings.CorefMode != "heuristic" && settings.CorefMode != "llm") {
		return nil
	}
	var session string
	if err := s.db.QueryRow(ctx, `SELECT COALESCE(source_session,'') FROM memories WHERE id=$1`, id).Scan(&session); err != nil {
		return err
	}
	prior := []corefPrior{}
	if session != "" {
		rows, err := s.db.Query(ctx, `SELECT p.key,p.content FROM memories p JOIN memories current ON current.id=$1
 WHERE p.id<current.id AND p.source_session=current.source_session AND p.lifecycle_state='active'
 AND p.scope_type=current.scope_type AND p.scope_value=current.scope_value ORDER BY p.id DESC LIMIT $2`, id, settings.CorefWindow)
		if err != nil {
			return err
		}
		for rows.Next() {
			var p corefPrior
			if err = rows.Scan(&p.key, &p.content); err != nil {
				rows.Close()
				return err
			}
			p.key = textBound(p.key, 255)
			p.content = textBound(p.content, 2047)
			prior = append(prior, p)
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return err
		}
	}
	entity, outcome := "", "unbound"
	confidence := 0.0
	if settings.CorefMode == "heuristic" {
		entity, outcome = heuristicCoref(prior)
		if entity != "" {
			confidence = 1
		}
	} else if settings.Command != "" {
		snippets := []string{}
		for _, p := range prior {
			if p.content != "" {
				snippets = append(snippets, p.content)
			}
		}
		input, _ := json.Marshal(map[string]any{"task": "coref", "memory_id": id, "content": textBound(content, 64*1024), "context": snippets})
		runner := s.episodeCommand
		if runner == nil {
			runner = runEpisodeCommand
		}
		callCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		raw, err := runner(callCtx, settings.Command, input)
		cancel()
		if err == nil && len(raw) <= episodeMaxOutput {
			var result struct {
				Bindings []struct {
					Entity     string   `json:"entity"`
					Confidence *float64 `json:"confidence"`
				} `json:"coref_bindings"`
			}
			if json.Unmarshal(raw, &result) == nil {
				for _, b := range result.Bindings[:min(8, len(result.Bindings))] {
					c := .5
					if b.Confidence != nil {
						c = *b.Confidence
					}
					name := textBound(derivedCanonical(b.Entity), 127)
					if name != "" && c >= .5 && c <= 1 && c > confidence {
						entity, confidence, outcome = name, c, "bound"
					}
				}
			}
		}
	}
	if entity != "" {
		if _, err := s.db.Exec(ctx, `INSERT INTO memory_entities(memory_id,entity,role,weight) VALUES($1,$2,'coref',2.8)
 ON CONFLICT(memory_id,entity,role) DO UPDATE SET weight=EXCLUDED.weight`, id, entity); err != nil {
			return err
		}
	}
	_, err := s.db.Exec(ctx, `INSERT INTO memory_coref_audit(memory_id,session_id,outcome,entity,mode,confidence) VALUES($1,$2,$3,$4,$5,$6)`, id, session, outcome, entity, settings.CorefMode, confidence)
	return err
}
