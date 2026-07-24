package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"
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
		text       TEXT NOT NULL,
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
	return nil
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
		`SELECT speaker, text, created_at FROM channel_messages WHERE channel=? ORDER BY id LIMIT 200`,
		name,
	)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		fmt.Fprintf(w, "[]")
		return
	}
	defer rows.Close()
	type msg struct {
		Speaker   string `json:"sender"`
		Text      string `json:"text"`
		CreatedAt string `json:"created_at,omitempty"`
	}
	var msgs []msg
	for rows.Next() {
		var m msg
		rows.Scan(&m.Speaker, &m.Text, &m.CreatedAt)
		msgs = append(msgs, m)
	}
	if msgs == nil {
		msgs = []msg{}
	}
	json.NewEncoder(w).Encode(map[string]any{"messages": msgs})
}

func (s *server) handleChannelPost(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	var body struct {
		Speaker string `json:"speaker"`
		Text    string `json:"text"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Text == "" {
		http.Error(w, `{"error":"text required"}`, http.StatusBadRequest)
		return
	}
	if body.Speaker == "" {
		body.Speaker = "user"
	}
	now := time.Now().UTC().Format(time.RFC3339)
	_, err := s.db.Exec(
		`INSERT INTO channel_messages(channel, speaker, text, created_at) VALUES (?,?,?,?)`,
		name, body.Speaker, body.Text, now,
	)
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprintf(w, `{"error":"db error"}`)
		return
	}

	payload, _ := json.Marshal(map[string]string{
		"sender": body.Speaker, "text": body.Text, "created_at": now,
	})
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

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")

	ch := broker.subscribe(name)
	defer broker.unsubscribe(name, ch)

	ctx := r.Context()
	keepalive := time.NewTicker(25 * time.Second)
	defer keepalive.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-keepalive.C:
			fmt.Fprintf(w, ": keepalive\n\n")
			flusher.Flush()
		case data := <-ch:
			fmt.Fprintf(w, "data: %s\n\n", data)
			flusher.Flush()
		}
	}
}
