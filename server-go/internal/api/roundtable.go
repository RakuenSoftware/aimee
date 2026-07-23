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
	// A 16 MiB text artifact can expand substantially when JSON-escaped. Bound
	// the wire representation without truncating it; Review applies the smaller
	// semantic artifact limit after decoding.
	r.Body = http.MaxBytesReader(w, r.Body, 128<<20)
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
		var validation roundtable.ValidationError
		if errors.As(err, &validation) {
			writeJSON(w, http.StatusBadRequest, map[string]any{"ok": false, "error": err.Error(), "roundtable": result})
			return
		}
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"ok": false, "error": err.Error(), "roundtable": result})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "roundtable": result})
}
