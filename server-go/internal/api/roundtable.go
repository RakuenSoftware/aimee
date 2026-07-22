package api

import (
	"errors"
	"io"
	"net/http"

	"github.com/JBailes/aimee/server-go/internal/roundtable"
)

func (s *Server) roundtableReview(w http.ResponseWriter, r *http.Request) {
	if s.roundtable == nil {
		writeError(w, http.StatusServiceUnavailable, errors.New("Go roundtable engine is unavailable"))
		return
	}
	var request roundtable.ReviewRequest
	decoder := jsonDecoder(r.Body)
	if err := decoder.Decode(&request); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, errors.New("request must contain one JSON value"))
		return
	}
	result, err := s.roundtable.Review(r.Context(), request)
	if err != nil {
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"ok": false, "error": err.Error(), "roundtable": result})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "roundtable": result})
}
