package providers

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"time"
)

func metadataCachePath() string {
	base := os.Getenv("XDG_CACHE_HOME")
	if base == "" {
		home, _ := os.UserHomeDir()
		base = filepath.Join(home, ".cache")
	}
	return filepath.Join(base, "aimee/models_dev.json")
}
func (m *Manager) downloadMetadata(ctx context.Context) (object, error) {
	path := metadataCachePath()
	if st, err := os.Stat(path); err == nil && time.Since(st.ModTime()) >= 0 && time.Since(st.ModTime()) < 24*time.Hour {
		return object{"status": "ok", "message": "model metadata cache is fresh"}, nil
	}
	response, err := m.request(ctx, object{"endpoint": "https://models.dev", "auth_type": "none", "max_response_bytes": (16 << 20) - 12}, "GET", "/api.json", nil)
	if err != nil {
		return nil, err
	}
	if response.Status != 200 {
		return nil, errors.New("model metadata download unavailable")
	}
	entries, err := parseMetadata(response.Body)
	if err != nil || len(entries) == 0 {
		return nil, errors.New("model metadata download is invalid; existing cache retained")
	}
	if err = os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".models-dev-*")
	if err != nil {
		return nil, err
	}
	defer os.Remove(file.Name())
	if _, err = file.Write(response.Body); err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	if err = os.Rename(file.Name(), path); err != nil {
		return nil, err
	}
	dir, err := os.Open(filepath.Dir(path))
	if err != nil {
		return nil, err
	}
	err = dir.Sync()
	dir.Close()
	if err != nil {
		return nil, err
	}
	return m.metadata.request("metadata.refresh", nil)
}
