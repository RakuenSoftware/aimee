package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"

	"github.com/JBailes/aimee/server-go/internal/workflowstore"
)

func (s *Server) workflowGate(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Decision string `json:"decision"`
		Gate     string `json:"gate,omitempty"`
	}
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil || (request.Decision != "approve" && request.Decision != "reject") {
		writeError(w, http.StatusBadRequest, errors.New("decision must be approve or reject"))
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, errors.New("request must contain one JSON value"))
		return
	}
	item, err := s.authorizedWorkItem(r, true)
	if err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	if item.PauseReason != "human_gate" {
		writeError(w, http.StatusConflict, errors.New("workflow is not waiting at a human gate"))
		return
	}
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	definition, err := registry.LoadVersion(item.WorkflowName, item.WorkflowVersion)
	if err != nil {
		writeError(w, http.StatusConflict, errors.New("pinned workflow definition is unavailable"))
		return
	}
	node, ok := definition.Node(item.Stage)
	if !ok || node.Block != "gate.human" || (request.Gate != "" && request.Gate != node.ID) {
		writeError(w, http.StatusConflict, errors.New("current stage is not the requested human gate"))
		return
	}
	if request.Decision == "approve" {
		artifact, putErr := s.artifacts.PutNodeArtifact(item.ID, node.ID, "approval", []byte("approved"))
		if putErr != nil {
			writeError(w, http.StatusInternalServerError, putErr)
			return
		}
		next := node.OnPass
		if next == "" {
			next = node.Next
		}
		if next == "" {
			err = s.db.Finish(r.Context(), item.ID, node.ID, "accepted", "human approval", artifact.Hash, 0)
		} else {
			err = s.db.ResolveGate(r.Context(), item.ID, node.ID, next, "approve", artifact.Hash)
		}
	} else {
		artifact, putErr := s.artifacts.PutNodeArtifact(item.ID, node.ID, "approval", []byte("rejected"))
		if putErr != nil {
			writeError(w, http.StatusInternalServerError, putErr)
			return
		}
		if node.OnFail == "" {
			err = s.db.RejectGate(r.Context(), item.ID, node.ID, artifact.Hash)
		} else {
			err = s.db.ResolveGate(r.Context(), item.ID, node.ID, node.OnFail, "reject", artifact.Hash)
		}
	}
	if err != nil {
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.notify != nil {
		s.notify()
	}
	s.writeItem(w, r)
}

func (s *Server) workflowPause(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if _, err := s.authorizedWorkItem(r, false); err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	if err := s.db.Pause(r.Context(), id); err != nil {
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.cancel != nil {
		s.cancel(id)
	}
	s.writeItem(w, r)
}

func (s *Server) workflowResume(w http.ResponseWriter, r *http.Request) {
	if _, err := s.authorizedWorkItem(r, false); err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	if err := s.db.Resume(r.Context(), r.PathValue("id")); err != nil {
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.notify != nil {
		s.notify()
	}
	s.writeItem(w, r)
}

func (s *Server) workflowStop(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if _, err := s.authorizedWorkItem(r, false); err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	ids, err := s.db.StopTree(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusConflict, err)
		return
	}
	if s.cancel != nil {
		for _, descendant := range ids {
			s.cancel(descendant)
		}
	}
	s.writeItem(w, r)
}

func (s *Server) workflowDelete(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if _, err := s.authorizedWorkItem(r, false); err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	ids, err := s.db.DescendantIDs(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusNotFound, err)
		return
	}
	items := make([]workflowstore.WorkItem, 0, len(ids))
	for _, descendant := range ids {
		if child, loadErr := s.db.WorkItem(r.Context(), descendant); loadErr == nil {
			items = append(items, child)
		}
	}
	for _, treeItem := range items {
		if treeItem.State == "active" {
			if err := s.db.Stop(r.Context(), treeItem.ID); err != nil {
				writeError(w, http.StatusConflict, err)
				return
			}
		}
	}
	if s.cancel != nil {
		for _, descendant := range ids {
			s.cancel(descendant)
		}
	}
	if s.cleanupWorktree != nil {
		for _, deleted := range items {
			if err := s.cleanupWorktree(r.Context(), deleted); err != nil {
				writeError(w, http.StatusConflict, err)
				return
			}
		}
	}
	for _, descendant := range ids {
		if err := s.artifacts.DeleteWorkItem(descendant); err != nil {
			writeError(w, http.StatusConflict, err)
			return
		}
	}
	if err := s.db.Delete(r.Context(), id); err != nil {
		writeError(w, http.StatusConflict, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "id": id})
}

func (s *Server) writeItem(w http.ResponseWriter, r *http.Request) {
	item, err := s.authorizedWorkItem(r, false)
	if err != nil {
		writeWorkItemAccessError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, item)
}
