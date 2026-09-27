package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

// Deterministic text indexes formerly maintained by memory_core_helpers*.c.
// Keep the historical extraction bounds, but never split a UTF-8 encoding or
// let overlapping chunks revisit the same input position.
type derivedTerm struct {
	Text   string  `json:"text"`
	Role   string  `json:"role"`
	Weight float64 `json:"weight"`
}
type derivedFrame struct{ Actor, Action, Object, Location, Time, Evidence string }
type derivedText struct {
	Aliases, Entities, Temporal []derivedTerm
	Headline, Signals           string
	Frames                      []derivedFrame
	Chunks                      []string
}

func wordIn(word, words string) bool { return strings.Contains(" "+words+" ", " "+word+" ") }
func derivedStop(word string) bool {
	return wordIn(word, "a an and are as at be but by did do for from had has have her him his if in is it its just like me my no not of on or our so also than that the them then there they this to was were what when where who why how with yes your")
}
func derivedRelation(word string) bool {
	return wordIn(word, "at in on to from with for into onto near by after before during")
}
func derivedLocation(word string) bool {
	return wordIn(word, "home office school park restaurant hospital airport hotel beach store room city town campus")
}
func derivedAction(word string) bool {
	return wordIn(word, "go went meet met call called visit visited join joined work worked start started finish finished travel traveled move moved buy bought learn learned study studied eat ate talk talked support supported plan planned celebrate celebrated install installed configure configured setup set deploy deployed") || len(word) > 3 && (strings.HasSuffix(word, "ed") || strings.HasSuffix(word, "ing"))
}
func derivedMonth(word string) time.Month {
	for i, names := range []string{"january jan", "february feb", "march mar", "april apr", "may", "june jun", "july jul", "august aug", "september sep sept", "october oct", "november nov", "december dec"} {
		if wordIn(word, names) {
			return time.Month(i + 1)
		}
	}
	return 0
}
func derivedWeekday(word string) int {
	for i, w := range strings.Fields("sunday monday tuesday wednesday thursday friday saturday") {
		if word == w {
			return i
		}
	}
	return -1
}
func textBound(text string, n int) string {
	if len(text) <= n {
		return text
	}
	for n > 0 && !utf8.RuneStart(text[n]) {
		n--
	}
	return text[:n]
}
func derivedNormalize(text string) string {
	text = strings.ToLower(textBound(text, 4095))
	chars := []rune(text)
	for i, c := range chars {
		if unicode.IsSpace(c) || strings.ContainsRune("[](){}", c) {
			chars[i] = ' '
		}
		if c == '-' && i > 0 && i+1 < len(chars) {
			before, after := chars[i-1], chars[i+1]
			start, end := i-1, i+1
			for start > 0 && unicode.IsLetter(chars[start-1]) {
				start--
			}
			for end < len(chars) && unicode.IsLetter(chars[end]) {
				end++
			}
			if unicode.IsDigit(before) && (unicode.IsDigit(after) || derivedMonth(string(chars[i+1:end])) > 0) || unicode.IsDigit(after) && derivedMonth(string(chars[start:i])) > 0 {
				chars[i] = ' '
			}
		}
	}
	words := strings.Fields(string(chars))
	out := words[:0]
	for _, w := range words {
		if len(w) > 2 {
			if strings.HasSuffix(w, "'s") {
				w = strings.TrimSuffix(w, "'s")
			} else if strings.HasSuffix(w, "s'") {
				w = strings.TrimSuffix(w, "'")
			}
		}
		if !wordIn(w, "the a an is are was were be uh um actually well think maybe kinda sorta") {
			out = append(out, w)
		}
	}
	return strings.Join(out, " ")
}
func derivedTokens(text string, n int) []string {
	words := strings.Fields(textBound(derivedNormalize(text), 2047))
	if len(words) > n {
		words = words[:n]
	}
	for i := range words {
		words[i] = textBound(words[i], 63)
	}
	return words
}
func derivedCanonical(text string) string {
	text = derivedNormalize(textBound(text, 255))
	for _, p := range []string{"mr ", "mrs ", "ms ", "dr ", "prof ", "sir "} {
		if strings.HasPrefix(text, p) {
			text = strings.TrimPrefix(text, p)
			break
		}
	}
	if len(text) > 4 {
		if strings.HasSuffix(text, "ies") {
			return strings.TrimSuffix(text, "ies") + "y"
		}
		if strings.HasSuffix(text, "es") {
			return strings.TrimSuffix(text, "es")
		}
		if strings.HasSuffix(text, "s") && !strings.HasSuffix(text, "ss") {
			return strings.TrimSuffix(text, "s")
		}
	}
	return text
}
func derivedUseful(word string) bool {
	letters, digits := 0, 0
	for _, c := range word {
		if unicode.IsLetter(c) {
			letters++
		}
		if unicode.IsDigit(c) {
			digits++
		}
	}
	return letters >= 3 || digits >= 4
}
func appendDerived(out *[]derivedTerm, text, role string, weight float64) {
	if text == "" {
		return
	}
	for i, t := range *out {
		if t.Text == text && t.Role == role {
			if weight > t.Weight {
				(*out)[i].Weight = weight
			}
			return
		}
	}
	*out = append(*out, derivedTerm{text, role, weight})
}
func deriveText(key, content, created string) derivedText {
	var d derivedText
	alias := func(text string, w float64) { appendDerived(&d.Aliases, derivedNormalize(text), "", w) }
	entity := func(text, role string, w float64) { appendDerived(&d.Entities, derivedCanonical(text), role, w) }
	alias(key, 4)
	entity(key, "key", 3)
	words := derivedTokens(content, 16)
	emitted := 0
	for span := min(6, len(words)); span >= 3 && emitted < 24; span-- {
		alias(strings.Join(words[:span], " "), 3+float64(span)*.1)
		emitted++
	}
	for _, span := range []int{2, 3, 1} {
		for i := 0; i+span <= len(words) && emitted < 24; i++ {
			useful := true
			for _, w := range words[i : i+span] {
				useful = useful && derivedUseful(w)
			}
			if useful {
				weight := 1.0
				if span == 2 {
					weight = 2
				}
				if span == 3 {
					weight = 2.5
				}
				alias(strings.Join(words[i:i+span], " "), weight)
				emitted++
			}
		}
	}
	if colon := strings.IndexByte(content, ':'); colon > 0 && colon <= 48 {
		prefix := content[:colon]
		valid, letter := true, false
		for _, c := range prefix {
			letter = letter || unicode.IsLetter(c)
			valid = valid && (unicode.IsLetter(c) || unicode.IsSpace(c) || c == '-')
		}
		if valid && letter {
			entity(strings.TrimSpace(prefix), "actor", 2.8)
		}
	}
	words = derivedTokens(content, 24)
	for i, w := range words {
		if len(w) >= 5 && !derivedStop(w) && !derivedAction(w) && derivedMonth(w) == 0 && derivedWeekday(w) < 0 && !derivedRelation(w) && !derivedLocation(w) {
			weight := 1.0
			if len(w) >= 8 {
				weight = 1.5
			}
			entity(w, "term", weight)
		}
		for _, span := range []int{2, 3} {
			if i+span > len(words) {
				continue
			}
			valid := true
			for _, v := range words[i : i+span] {
				valid = valid && len(v) >= 4 && !derivedStop(v) && !derivedAction(v) && (span == 3 || !derivedRelation(v))
			}
			if valid {
				entity(strings.Join(words[i:i+span], " "), "phrase", 1+float64(span)*.5)
			}
		}
	}
	source := content
	if source == "" {
		source = key
	}
	words = derivedTokens(source, 32)
	d.Headline = strings.Join(words[:min(20, len(words))], " ")
	seen := map[string]bool{}
	signals := []string{}
	for _, w := range words {
		v := derivedCanonical(w)
		if len(w) >= 3 && !derivedStop(w) && !wordIn(w, "user assistant system speaker") && len(v) >= 4 && !seen[v] && len(signals) < 24 {
			seen[v] = true
			signals = append(signals, v)
		}
	}
	d.Signals = strings.Join(signals, " ")
	for _, sentence := range strings.FieldsFunc(source, func(r rune) bool { return strings.ContainsRune(".!?;\n", r) }) {
		if len(d.Frames) == 8 {
			break
		}
		sentence = strings.TrimSpace(textBound(sentence, 511))
		if sentence == "" {
			continue
		}
		f := deriveFrame(sentence, key)
		f.Evidence = "derived"
		if len(d.Frames) > 0 {
			f.Evidence = "chunk"
		}
		d.Frames = append(d.Frames, f)
	}
	d.Chunks = deriveChunks(content)
	d.Temporal = deriveTemporal(key, content, created)
	return d
}
func deriveFrame(text, key string) derivedFrame {
	var f derivedFrame
	words := derivedTokens(text, 32)
	action := -1
	for i, w := range words {
		if f.Action == "" && derivedAction(w) {
			f.Action = w
			action = i
		}
		if f.Location == "" && derivedLocation(w) {
			f.Location = w
		}
		if f.Location == "" && derivedRelation(w) && i+1 < len(words) {
			n := 1
			if i+2 < len(words) && !derivedRelation(words[i+2]) {
				n = 2
			}
			f.Location = strings.Join(words[i+1:i+1+n], " ")
		}
		if f.Time == "" {
			if derivedMonth(w) > 0 && i+1 < len(words) && len(words[i+1]) <= 4 && len(words[i+1]) > 0 && isDigit(words[i+1][0]) {
				f.Time = w + " " + words[i+1]
			} else if derivedMonth(w) > 0 || derivedWeekday(w) >= 0 || wordIn(w, "today yesterday tomorrow") || len(w) == 4 && isDigit(w[0]) {
				f.Time = w
			}
		}
	}
	if action > 0 {
		for i := action - 1; i >= 0; i-- {
			w := words[i]
			if derivedStop(w) || derivedMonth(w) > 0 || derivedWeekday(w) >= 0 || derivedRelation(w) {
				continue
			}
			f.Actor = w
			if i > 0 && len(words[i-1]) >= 3 && !derivedStop(words[i-1]) && !derivedRelation(words[i-1]) {
				f.Actor = words[i-1] + " " + w
			}
			break
		}
		start := action + 1
		for start < len(words) && (derivedStop(words[start]) || derivedRelation(words[start])) {
			start++
		}
		if start < len(words) && derivedMonth(words[start]) == 0 && derivedWeekday(words[start]) < 0 {
			n := 1
			if start+1 < len(words) && !derivedStop(words[start+1]) && !derivedRelation(words[start+1]) && derivedMonth(words[start+1]) == 0 {
				n = 2
			}
			f.Object = strings.Join(words[start:start+n], " ")
		}
	}
	if f.Actor == "" {
		f.Actor = key
	}
	if f.Action == "" {
		f.Action = "state"
	}
	if f.Object == "" {
		f.Object = key
	}
	return f
}
func deriveChunks(content string) []string {
	out := []string{}
	for pos := 0; pos < len(content) && len(out) < 8; {
		end := min(pos+200, len(content))
		for end < len(content) && end > pos && !utf8.RuneStart(content[end]) {
			end--
		}
		stop := end
		if end < len(content) {
			for i := end; i > pos; i-- {
				if strings.ContainsRune(".!?;\n", rune(content[i-1])) {
					stop = i
					break
				}
			}
		}
		if text := strings.TrimRightFunc(content[pos:stop], unicode.IsSpace); text != "" {
			out = append(out, text)
		}
		if stop == len(content) {
			break
		}
		next := stop - 40
		if next <= pos {
			next = stop
		}
		for next < stop && !utf8.RuneStart(content[next]) {
			next++
		}
		pos = next
	}
	return out
}
func deriveTemporal(key, content, created string) []derivedTerm {
	out := []derivedTerm{}
	add := func(text, role string, w float64) { appendDerived(&out, derivedNormalize(text), role, w) }
	date, err := time.Parse("2006-01-02", textBound(created, 10))
	hasDate := err == nil
	absolute := func(t time.Time, w float64) { add(t.Format("2006-01-02"), "absolute_day", w) }
	for _, w := range derivedTokens(key, 24) {
		if derivedMonth(w) > 0 {
			add(w, "month", 1.8)
		} else if derivedWeekday(w) >= 0 {
			add(w, "weekday", 1.5)
		} else if len(w) == 4 && allDigits(w) {
			add(w, "year", 2)
		}
	}
	words := derivedTokens(content, 24)
	for i := range words {
		words[i] = strings.Trim(words[i], ".,!?:;\"")
	}
	for i, w := range words {
		switch {
		case wordIn(w, "today yesterday tomorrow tonight morning afternoon evening"):
			add(w, "relative", 1.8)
			if hasDate && wordIn(w, "today yesterday tomorrow") {
				delta := 0
				if w == "yesterday" {
					delta = -1
				}
				if w == "tomorrow" {
					delta = 1
				}
				absolute(date.AddDate(0, 0, delta), 2.6)
			}
		case derivedMonth(w) > 0:
			add(w, "month", 2)
			if i+1 < len(words) && len(words[i+1]) <= 4 {
				add(w+" "+words[i+1], "date_phrase", 2.3)
				day, e := strconv.Atoi(words[i+1])
				if hasDate && e == nil && day > 0 && day <= 31 {
					t := time.Date(date.Year(), derivedMonth(w), day, 0, 0, 0, 0, time.UTC)
					if t.Month() == derivedMonth(w) {
						absolute(t, 2.8)
					}
				}
			}
		case derivedWeekday(w) >= 0:
			add(w, "weekday", 1.7)
		case len(w) == 4 && allDigits(w):
			add(w, "year", 2.2)
		case wordIn(w, "before after during first last recently between"):
			add(w, "relative", 1.2)
			if w == "last" && i+1 < len(words) && hasDate {
				next := words[i+1]
				switch next {
				case "week":
					absolute(date.AddDate(0, 0, -7), 2.2)
					add("last:week", "relative_phrase", 2)
				case "month":
					absolute(time.Date(date.Year(), date.Month()-1, 1, 0, 0, 0, 0, time.UTC), 2)
					add("last:month", "relative_phrase", 2)
				case "year":
					add(strconv.Itoa(date.Year()-1), "year", 2.2)
					add("last:year", "relative_phrase", 2)
				default:
					if target := derivedWeekday(next); target >= 0 {
						delta := (int(date.Weekday()) - target + 7) % 7
						if delta == 0 {
							delta = 7
						}
						absolute(date.AddDate(0, 0, -delta), 2.4)
					}
				}
			}
			if wordIn(w, "before after during") && i+1 < len(words) {
				add(w+":"+words[i+1], "ordering", 1.8)
			}
			if w == "between" && i+2 < len(words) {
				add(w+":"+words[i+1]+":"+words[i+2], "range", 2)
			}
		case i+2 < len(words) && words[i+2] == "ago" && hasDate:
			n, e := strconv.Atoi(w)
			if e != nil || n <= 0 || n > 36500 {
				continue
			}
			switch words[i+1] {
			case "day", "days":
				absolute(date.AddDate(0, 0, -n), 2.5)
				add("ago", "relative", 1)
			case "week", "weeks":
				absolute(date.AddDate(0, 0, -7*n), 2.4)
				add("ago", "relative", 1)
			case "month", "months":
				absolute(time.Date(date.Year(), date.Month()-time.Month(n), min(date.Day(), 28), 0, 0, 0, 0, time.UTC), 2.4)
				add("ago", "relative", 1)
			}
		}
	}
	return out
}

func (s *postgresDataStore) replaceDerivedText(ctx context.Context, id int64, d derivedText) error {
	// These are deterministic indexes, not authored graph relations. Keep coref
	// bindings until the configured resolver can replace them in the same scope.
	if _, err := s.db.Exec(ctx, `WITH aliases AS (
 DELETE FROM memory_aliases WHERE memory_id=$1
), entities AS (
 DELETE FROM memory_entities WHERE memory_id=$1 AND role<>'coref'
), temporal AS (
 DELETE FROM memory_temporal_refs WHERE memory_id=$1
), frames AS (
 DELETE FROM memory_event_frames WHERE memory_id=$1 AND evidence_kind IN ('derived','chunk')
) DELETE FROM memory_chunks WHERE memory_id=$1`, id); err != nil {
		return err
	}
	for _, group := range []struct {
		terms []derivedTerm
		sql   string
	}{
		{d.Aliases, `INSERT INTO memory_aliases(memory_id,alias,weight)
 SELECT $1,text,weight FROM jsonb_to_recordset($2::jsonb) AS x(text text,weight double precision)`},
		{d.Entities, `INSERT INTO memory_entities(memory_id,entity,weight,role)
 SELECT $1,text,weight,role FROM jsonb_to_recordset($2::jsonb) AS x(text text,weight double precision,role text)`},
		{d.Temporal, `INSERT INTO memory_temporal_refs(memory_id,ref_key,weight,granularity)
 SELECT $1,text,weight,role FROM jsonb_to_recordset($2::jsonb) AS x(text text,weight double precision,role text)`},
	} {
		if len(group.terms) == 0 {
			continue
		}
		encoded, err := json.Marshal(group.terms)
		if err != nil {
			return err
		}
		if _, err = s.db.Exec(ctx, group.sql, id, string(encoded)); err != nil {
			return err
		}
	}
	for _, summary := range []struct{ scope, text string }{{"headline", d.Headline}, {"signals", d.Signals}} {
		if summary.text != "" {
			if _, err := s.db.Exec(ctx, `INSERT INTO memory_summaries(memory_id,scope,summary) VALUES($1,$2,$3) ON CONFLICT(memory_id,scope) DO UPDATE SET summary=EXCLUDED.summary`, id, summary.scope, summary.text); err != nil {
				return err
			}
		}
	}
	if _, err := s.db.Exec(ctx, `WITH stale AS MATERIALIZED (
 SELECT id FROM memory_summaries WHERE memory_id=$1 AND ((scope='headline' AND $2='') OR (scope='signals' AND $3=''))
), deps AS (DELETE FROM derived_memory_dependencies WHERE derived_kind='summary' AND derived_memory_id IN(SELECT id::text FROM stale)),
 queues AS (DELETE FROM derived_rederivation_queue WHERE derived_kind='summary' AND derived_memory_id IN(SELECT id::text FROM stale)),
 registry AS (DELETE FROM derived_memory_registry WHERE derived_kind='summary' AND derived_memory_id IN(SELECT id::text FROM stale))
 DELETE FROM memory_summaries WHERE id IN(SELECT id FROM stale)`, id, d.Headline, d.Signals); err != nil {
		return err
	}
	for _, f := range d.Frames {
		if _, err := s.db.Exec(ctx, `INSERT INTO memory_event_frames(memory_id,actor,action,object,location,event_time,evidence_kind) VALUES($1,$2,$3,$4,$5,$6,$7)`, id, f.Actor, f.Action, f.Object, f.Location, f.Time, f.Evidence); err != nil {
			return err
		}
	}
	for i, chunk := range d.Chunks {
		if _, err := s.db.Exec(ctx, `INSERT INTO memory_chunks(memory_id,chunk_index,chunk_text) VALUES($1,$2,$3)`, id, i, chunk); err != nil {
			return err
		}
	}
	return nil
}

// A read of a newer parent cannot certify an older generated summary. Only the
// deterministic producer records this observation, while holding the parent
// lock from reading its source through finishing the derived replacement.
func (s *postgresDataStore) pinDerivedSummaryInputs(ctx context.Context, id int64) error {
	_, err := s.db.Exec(ctx, `UPDATE derived_memory_dependencies d
 SET input_version=m.record_revision::text,extractor_version='go-derived-text-v1',
 derivation_policy_version='summary-input-v1'
 FROM memory_summaries summary,memories m
 WHERE d.derived_kind='summary' AND d.derived_memory_id=summary.id::text
 AND d.input_kind='memory' AND d.input_id=m.id::text
 AND summary.memory_id=m.id AND m.id=$1 AND summary.scope IN ('headline','signals')`, id)
	if err != nil {
		return err
	}
	// Only these rebuilt summaries acquired new observations. Source mutations
	// already reconcile descendants; walking that graph again per index row is
	// both redundant and quadratic during a full rebuild.
	_, err = s.db.Exec(ctx, `WITH evaluated AS MATERIALIZED (
 SELECT d.*,knowledge_input_moved(d.input_kind,d.input_id,d.input_version,d.source_hash,
 d.derivation_policy_version,d.derived_kind) AS moved
 FROM derived_memory_dependencies d JOIN memory_summaries summary ON d.derived_kind='summary'
 AND d.derived_memory_id=summary.id::text WHERE summary.memory_id=$1 AND summary.scope IN ('headline','signals')
 ), fresh AS (
 SELECT derived_kind,derived_memory_id,
 CASE WHEN bool_or(moved AND contribution='essential') THEN 'unsupported'
 WHEN bool_or(moved) THEN 'stale' ELSE 'fresh' END AS status,
 COALESCE((array_agg(input_kind ORDER BY id) FILTER(WHERE moved))[1],'') AS cause_kind,
 COALESCE((array_agg(input_id ORDER BY id) FILTER(WHERE moved))[1],'') AS cause_id
 FROM evaluated GROUP BY derived_kind,derived_memory_id
 ) UPDATE derived_memory_registry r SET current_status=f.status,stale_cause_kind=f.cause_kind,
 stale_cause_id=f.cause_id,updated_at=pg_now_text() FROM fresh f
 WHERE r.derived_kind=f.derived_kind AND r.derived_memory_id=f.derived_memory_id`, id)
	return err
}

// Unknown or additional input contracts cannot be treated as a fresh one-parent
// summary. Consumers may still use the independently eligible canonical text.
func summaryCurrentInputsSQL(summary, parent string) string {
	return `EXISTS(SELECT 1 FROM derived_memory_dependencies summary_input
 WHERE summary_input.derived_kind='summary' AND summary_input.derived_memory_id=` + summary + `.id::text
 AND summary_input.input_kind='memory' AND summary_input.input_id=` + parent + `.id::text
 AND summary_input.input_version=` + parent + `.record_revision::text
 AND summary_input.extractor_version='go-derived-text-v1'
 AND summary_input.derivation_policy_version='summary-input-v1')
 AND NOT EXISTS(SELECT 1 FROM derived_memory_dependencies other_input
 WHERE other_input.derived_kind='summary' AND other_input.derived_memory_id=` + summary + `.id::text
 AND (other_input.input_kind<>'memory' OR other_input.input_id<>` + parent + `.id::text))`
}
