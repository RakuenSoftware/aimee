package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
)

type webchatIdentityStore interface {
	Authenticate(username, password string) (bool, error)
	UpdatePassword(username, current, replacement string) error
	List() ([]string, error)
}

func authError(w http.ResponseWriter, status int, message, code string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": message, "code": code})
}

func (s *server) handleAuthLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		authError(w, http.StatusMethodNotAllowed, "method not allowed", "auth.method_not_allowed")
		return
	}
	ip := auth.ClientIP(r)
	limited, err := s.rl.IsLimited(ip)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "authentication unavailable")
		return
	}
	if limited {
		authError(w, http.StatusTooManyRequests, "too many login attempts, try again later", "auth.rate_limited")
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		authError(w, http.StatusBadRequest, "invalid request body", "auth.invalid_request")
		return
	}
	if req.Username == "" || req.Password == "" {
		authError(w, http.StatusBadRequest, "username and password required", "auth.credentials_required")
		return
	}
	ok, err := s.identity.Authenticate(req.Username, req.Password)
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "authentication unavailable")
		return
	}
	if !ok {
		_ = s.rl.RecordAttempt(ip)
		authError(w, http.StatusUnauthorized, "invalid credentials", "auth.invalid_credentials")
		return
	}
	_ = s.rl.ClearAttempts(ip)
	token, err := s.sessions.CreateSession(req.Username)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "could not create session")
		return
	}
	http.SetCookie(w, &http.Cookie{
		Name: "session", Value: token, Path: "/", HttpOnly: true,
		Secure: s.cfg.tlsEnabled(), SameSite: http.SameSiteStrictMode,
		MaxAge: int((24 * time.Hour) / time.Second),
	})
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"user": map[string]string{"username": req.Username}})
}

func (s *server) handleAuthLogout(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		authError(w, http.StatusMethodNotAllowed, "method not allowed", "auth.method_not_allowed")
		return
	}
	if cookie, err := r.Cookie("session"); err == nil {
		_ = s.sessions.DeleteSession(cookie.Value)
	}
	http.SetCookie(w, &http.Cookie{
		Name: "session", Value: "", Path: "/", HttpOnly: true,
		Secure: s.cfg.tlsEnabled(), SameSite: http.SameSiteStrictMode, MaxAge: -1,
	})
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"status":"logged out"}`))
}

func (s *server) handleAuthPassword(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPut {
		authError(w, http.StatusMethodNotAllowed, "method not allowed", "auth.method_not_allowed")
		return
	}
	var req struct {
		CurrentPassword string `json:"current_password"`
		NewPassword     string `json:"new_password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		authError(w, http.StatusBadRequest, "invalid request body", "auth.invalid_request")
		return
	}
	if req.CurrentPassword == "" || req.NewPassword == "" {
		authError(w, http.StatusBadRequest, "current_password and new_password required", "auth.password_fields_required")
		return
	}
	if len(req.NewPassword) < setupAccountMinPassword || strings.ContainsAny(req.NewPassword, "\x00\r\n") {
		authError(w, http.StatusBadRequest,
			fmt.Sprintf("new password must be at least %d characters and contain no line breaks",
				setupAccountMinPassword), "auth.new_password_invalid")
		return
	}
	if err := s.identity.UpdatePassword(currentUser(r), req.CurrentPassword, req.NewPassword); err != nil {
		if errors.Is(err, errInvalidWebchatCredential) {
			authError(w, http.StatusUnauthorized, "current password is incorrect", "auth.current_password_incorrect")
			return
		}
		writeJSONError(w, http.StatusServiceUnavailable, "could not update password in Vault")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"status":"password updated"}`))
}

func (s *server) handleUsers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		authError(w, http.StatusMethodNotAllowed, "method not allowed", "users.method_not_allowed")
		return
	}
	names, err := s.identity.List()
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "could not list Vault accounts")
		return
	}
	users := make([]map[string]string, 0, len(names))
	for _, username := range names {
		users = append(users, map[string]string{"username": username})
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(users)
}
