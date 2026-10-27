package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

var channelMigrations = []string{
	`CREATE TABLE IF NOT EXISTS channels (
		name       TEXT PRIMARY KEY,
		created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
	)`,
	`CREATE TABLE IF NOT EXISTS channel_messages (
		id         INTEGER PRIMARY KEY AUTOINCREMENT,
		channel    TEXT NOT NULL REFERENCES channels(name) ON DELETE CASCADE,
		speaker    TEXT NOT NULL DEFAULT 'user',
		message    TEXT NOT NULL,
		created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
	)`,
	`CREATE INDEX IF NOT EXISTS idx_channel_messages_channel ON channel_messages(channel, id DESC)`,
}

type channelBroker struct {
	mu          sync.Mutex
	subscribers map[string][]chan string
}

var broker = &channelBroker{subscribers: map[string][]chan string{}}

func (b *channelBroker) subscribe(channel string) chan string {
	ch := make(chan string, 16)
	b.mu.Lock()
	b.subscribers[channel] = append(b.subscribers[channel], ch)
	b.mu.Unlock()
	return ch
}

func (b *channelBroker) unsubscribe(channel string, ch chan string) {
	b.mu.Lock()
	subs := b.subscribers[channel]
	for i, s := range subs {
		if s == ch {
			b.subscribers[channel] = append(subs[:i], subs[i+1:]...)
			break
		}
	}
	b.mu.Unlock()
}

func (b *channelBroker) publish(channel, data string) {
	b.mu.Lock()
	subs := append([]chan string{}, b.subscribers[channel]...)
	b.mu.Unlock()
	for _, ch := range subs {
		select {
		case ch <- data:
		default:
		}
	}
}

func applyChannelMigrations(db *sql.DB) error {
	for _, m := range channelMigrations {
		if _, err := db.Exec(m); err != nil {
			return err
		}
	}
	// Explicit import boundary for the retired flat channel archive. New writes
	// and reads use canonical messages; preserve sender metadata and durable IDs.
	rows, err := db.Query(`PRAGMA table_info(channel_messages)`)
	if err != nil {
		return err
	}
	legacy := false
	for rows.Next() {
		var cid, notnull, pk int
		var name, typ string
		var def any
		if err := rows.Scan(&cid, &name, &typ, &notnull, &def, &pk); err != nil {
			rows.Close()
			return err
		}
		legacy = legacy || name == "text"
	}
	err = rows.Err()
	rows.Close()
	if err != nil || !legacy {
		return err
	}
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err = tx.Exec(`CREATE TABLE channel_messages_ir (id INTEGER PRIMARY KEY AUTOINCREMENT, channel TEXT NOT NULL REFERENCES channels(name) ON DELETE CASCADE, speaker TEXT NOT NULL, message TEXT NOT NULL, created_at TEXT NOT NULL)`); err != nil {
		return err
	}
	old, err := tx.Query(`SELECT id, channel, speaker, text, created_at FROM channel_messages ORDER BY id`)
	if err != nil {
		return err
	}
	type imported struct {
		id                              int64
		channel, speaker, text, created string
	}
	var archive []imported
	for old.Next() {
		var item imported
		if err = old.Scan(&item.id, &item.channel, &item.speaker, &item.text, &item.created); err != nil {
			old.Close()
			return err
		}
		archive = append(archive, item)
	}
	err = old.Err()
	old.Close()
	if err != nil {
		return err
	}
	var highwater int64
	if err = tx.QueryRow(`SELECT COALESCE(MAX(seq),0) FROM sqlite_sequence WHERE name='channel_messages'`).Scan(&highwater); err != nil {
		return err
	}
	for _, item := range archive {
		if !utf8.ValidString(item.text) || strings.ContainsRune(item.text, 0) {
			return fmt.Errorf("legacy channel message %d is not canonical text", item.id)
		}
		message, err := json.Marshal(canonical.Message{Role: "user", Blocks: []canonical.Block{{Type: "text", Text: &item.text}}})
		if err != nil {
			return err
		}
		if _, err = tx.Exec(`INSERT INTO channel_messages_ir(id,channel,speaker,message,created_at) VALUES (?,?,?,?,?)`, item.id, item.channel, item.speaker, string(message), item.created); err != nil {
			return err
		}
	}
	if _, err = tx.Exec(`UPDATE sqlite_sequence SET seq=MAX(seq,?) WHERE name='channel_messages_ir'`, highwater); err != nil {
		return err
	}
	if _, err = tx.Exec(`INSERT INTO sqlite_sequence(name,seq) SELECT 'channel_messages_ir',? WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name='channel_messages_ir')`, highwater); err != nil {
		return err
	}
	for _, sql := range []string{`DROP TABLE channel_messages`, `ALTER TABLE channel_messages_ir RENAME TO channel_messages`, `CREATE INDEX idx_channel_messages_channel ON channel_messages(channel, id DESC)`} {
		if _, err = tx.Exec(sql); err != nil {
			return err
		}
	}
	return tx.Commit()
}

type channelMessage struct {
	ID        string          `json:"id"`
	Sender    string          `json:"sender"`
	Message   json.RawMessage `json:"message"`
	CreatedAt string          `json:"created_at,omitempty"`
}

func (s *server) handleChannelsList(w http.ResponseWriter, r *http.Request) {
	rows, err := s.db.Query(`SELECT name, created_at FROM channels ORDER BY created_at`)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		fmt.Fprintf(w, `{"channels":[]}`)
		return
	}
	defer rows.Close()
	type ch struct {
		Name      string `json:"name"`
		CreatedAt string `json:"created_at,omitempty"`
	}
	var channels []ch
	for rows.Next() {
		var c ch
		rows.Scan(&c.Name, &c.CreatedAt)
		channels = append(channels, c)
	}
	if channels == nil {
		channels = []ch{}
	}
	json.NewEncoder(w).Encode(map[string]any{"channels": channels})
}

func (s *server) handleChannelsCreate(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name string `json:"name"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Name == "" {
		http.Error(w, `{"error":"name required"}`, http.StatusBadRequest)
		return
	}
	_, err := s.db.Exec(`INSERT OR IGNORE INTO channels(name) VALUES (?)`, body.Name)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprintf(w, `{"error":"db error"}`)
		return
	}
	fmt.Fprintf(w, `{"status":"ok"}`)
}

func (s *server) handleChannelMessages(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	rows, err := s.db.Query(
		`SELECT id, speaker, message, created_at FROM (SELECT id, speaker, message, created_at FROM channel_messages WHERE channel=? ORDER BY id DESC LIMIT 200) ORDER BY id`,
		name,
	)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		fmt.Fprintf(w, "[]")
		return
	}
	defer rows.Close()
	var msgs []channelMessage
	for rows.Next() {
		var m channelMessage
		var encoded string
		if err := rows.Scan(&m.ID, &m.Sender, &encoded, &m.CreatedAt); err != nil {
			http.Error(w, "channel history unavailable", http.StatusInternalServerError)
			return
		}
		m.Message = json.RawMessage(encoded)
		if !canonical.ValidMessage(m.Message) {
			http.Error(w, "invalid stored channel message", http.StatusInternalServerError)
			return
		}
		msgs = append(msgs, m)
	}
	if msgs == nil {
		msgs = []channelMessage{}
	}
	json.NewEncoder(w).Encode(map[string]any{"messages": msgs})
}

func (s *server) handleChannelPost(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	var body struct {
		Message json.RawMessage `json:"message"`
	}
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<20))
	var message canonical.Message
	if err != nil || !canonical.Decode(raw, &body) || !canonical.ValidMessage(body.Message) || !canonical.Decode(body.Message, &message) || message.Role != "user" || len(message.Blocks) == 0 {
		http.Error(w, "canonical user message required", http.StatusBadRequest)
		return
	}
	hasContent := false
	for _, block := range message.Blocks {
		if block.Type != "text" || block.Text == nil || block.CacheControl != nil || block.CacheTTL != nil || block.Context != nil {
			http.Error(w, "channel supports text blocks only", http.StatusBadRequest)
			return
		}
		hasContent = hasContent || *block.Text != ""
	}
	if !hasContent {
		http.Error(w, "message content required", http.StatusBadRequest)
		return
	}
	sender := currentUser(r)
	if sender == "" {
		sender = "user"
	}
	now := time.Now().UTC().Format(time.RFC3339)
	result, err := s.db.Exec(
		`INSERT INTO channel_messages(channel, speaker, message, created_at) VALUES (?,?,?,?)`,
		name, sender, string(body.Message), now,
	)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprintf(w, `{"error":"db error"}`)
		return
	}

	id, err := result.LastInsertId()
	if err != nil {
		http.Error(w, "message identity unavailable", http.StatusInternalServerError)
		return
	}
	payload, _ := json.Marshal(channelMessage{ID: strconv.FormatInt(id, 10), Sender: sender, Message: body.Message, CreatedAt: now})
	broker.publish(name, string(payload))
	fmt.Fprintf(w, `{"status":"ok"}`)
}

func (s *server) handleChannelEvents(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	var cursor int64
	if raw := r.Header.Get("Last-Event-ID"); raw != "" {
		var err error
		cursor, err = strconv.ParseInt(raw, 10, 64)
		if err != nil || cursor < 0 {
			http.Error(w, "invalid event cursor", http.StatusBadRequest)
			return
		}
	} else if err := s.db.QueryRow(`SELECT COALESCE(MAX(id),0) FROM channel_messages WHERE channel=?`, name).Scan(&cursor); err != nil {
		http.Error(w, "channel history unavailable", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")

	ch := broker.subscribe(name)
	defer broker.unsubscribe(name, ch)

	ctx := r.Context()
	keepalive := time.NewTicker(time.Second)
	defer keepalive.Stop()

	// The broker only wakes the reader. SQLite supplies ordered, durable frames,
	// including anything missed while the bounded broker queue was full.
	flusher.Flush()
	for {
		rows, err := s.db.QueryContext(ctx, `SELECT id, speaker, message, created_at FROM channel_messages WHERE channel=? AND id>? ORDER BY id LIMIT 200`, name, cursor)
		if err != nil {
			return
		}
		count := 0
		for rows.Next() {
			var id int64
			var sender, created string
			var encoded string
			if err = rows.Scan(&id, &sender, &encoded, &created); err != nil {
				break
			}
			if !canonical.ValidMessage([]byte(encoded)) {
				rows.Close()
				return
			}
			payload, _ := json.Marshal(channelMessage{ID: strconv.FormatInt(id, 10), Sender: sender, Message: json.RawMessage(encoded), CreatedAt: created})
			if _, err = fmt.Fprintf(w, "id: %d\ndata: %s\n\n", id, payload); err != nil {
				break
			}
			cursor = id
			count++
		}
		rowsErr := rows.Err()
		rows.Close()
		if err != nil || rowsErr != nil {
			return
		}
		if count > 0 {
			flusher.Flush()
			continue
		}
		select {
		case <-ctx.Done():
			return
		case <-keepalive.C:
			fmt.Fprintf(w, ": keepalive\n\n")
			flusher.Flush()
		case <-ch:
		}
	}
}
