package main

import (
	"bufio"
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestChannelHistoryAndReconnectKeepDurableIdentity(t *testing.T) {
	db, err := sql.Open("sqlite3", filepath.Join(t.TempDir(), "channels.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if err = applyChannelMigrations(db); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(`INSERT INTO channels(name) VALUES ('room')`); err != nil {
		t.Fatal(err)
	}
	s := &server{db: db}
	for i := 0; i < 2; i++ {
		req := httptest.NewRequest("POST", "/", strings.NewReader(`{"text":"same"}`))
		req.SetPathValue("name", "room")
		res := httptest.NewRecorder()
		s.handleChannelPost(res, req)
		if res.Code != 200 {
			t.Fatal(res.Body.String())
		}
	}
	req := httptest.NewRequest("GET", "/", nil)
	req.SetPathValue("name", "room")
	res := httptest.NewRecorder()
	s.handleChannelMessages(res, req)
	var history struct {
		Messages []struct {
			ID   string `json:"id"`
			Text string `json:"text"`
		} `json:"messages"`
	}
	if err = json.Unmarshal(res.Body.Bytes(), &history); err != nil {
		t.Fatal(err)
	}
	if len(history.Messages) != 2 || history.Messages[0].ID != "1" || history.Messages[1].ID != "2" {
		t.Fatalf("lost identity: %s", res.Body.String())
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /room", func(w http.ResponseWriter, r *http.Request) {
		r.SetPathValue("name", "room")
		s.handleChannelEvents(w, r)
	})
	httpServer := httptest.NewServer(mux)
	defer httpServer.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	request, _ := http.NewRequestWithContext(ctx, "GET", httpServer.URL+"/room", nil)
	request.Header.Set("Last-Event-ID", "1")
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	scanner := bufio.NewScanner(response.Body)
	if !scanner.Scan() || scanner.Text() != "id: 2" {
		t.Fatalf("reconnect did not resume: %q %v", scanner.Text(), scanner.Err())
	}
	if !scanner.Scan() || !strings.Contains(scanner.Text(), `"id":"2"`) {
		t.Fatalf("missing durable identity: %q", scanner.Text())
	}
	// A write without a broker notification must still arrive from the log.
	if _, err = db.Exec(`INSERT INTO channel_messages(channel,text) VALUES ('room','missed wake')`); err != nil {
		t.Fatal(err)
	}
	for scanner.Scan() {
		if scanner.Text() == "id: 3" {
			return
		}
	}
	t.Fatalf("durable catch-up failed: %v", scanner.Err())
}
