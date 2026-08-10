package git

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// Forge operations. Named rather than numbered on the wire for the same reason
// the CI verdict is: an enum renumbered on one side of the boundary changes
// meaning silently on the other.
const (
	OpDefaultBranch = "default_branch"
	OpPRCreate      = "pr_create"
	OpPRFindOpen    = "pr_find_open"
	OpPRListOpen    = "pr_list_open"
	OpPRInfo        = "pr_info"
	OpPREdit        = "pr_edit"
	OpPRMerge       = "pr_merge"
)

const (
	forgeAccept  = "application/vnd.github+json"
	forgeTimeout = 20 * time.Second
)

// forgeBaseURL is overridden in tests. Production talks to api.github.com; the
// value is not configurable at runtime because a forge base that can be pointed
// elsewhere is a place to send a credential.
var forgeBaseURL = "https://api.github.com"

var forgeHTTPClient = &http.Client{Timeout: forgeTimeout}

// ForgeRequest is one operation against the forge.
//
// Token travels on the bus. That is a deliberate decision, not an oversight:
// this module performs the call, so it needs the credential, and the doctrine
// permits an internally-initiated outbound call from a module until the egress
// module exists. It is never logged, never echoed into a response, and never
// placed in a URL — only in the Authorization header of the outbound request.
type ForgeRequest struct {
	Op     string `json:"op"`
	Owner  string `json:"owner"`
	Repo   string `json:"repo"`
	Token  string `json:"token"`
	Number int    `json:"number,omitempty"`
	Head   string `json:"head,omitempty"`
	Base   string `json:"base,omitempty"`
	Title  string `json:"title,omitempty"`
	Body   string `json:"body,omitempty"`
	Draft  bool   `json:"draft,omitempty"`
	Limit  int    `json:"limit,omitempty"`
	// MergeMethod is "merge", "squash" or "rebase"; empty means merge.
	MergeMethod string `json:"merge_method,omitempty"`
	// ExpectedHeadSHA refuses the merge if the head has moved since it was read.
	ExpectedHeadSHA string `json:"expected_head_sha,omitempty"`
}

// PullSummary is the subset of a pull request every caller here needs.
type PullSummary struct {
	Number  int    `json:"number"`
	State   string `json:"state"`
	Title   string `json:"title"`
	Head    string `json:"head,omitempty"`
	Base    string `json:"base,omitempty"`
	HeadSHA string `json:"head_sha,omitempty"`
	Draft   bool   `json:"draft,omitempty"`
	Merged  bool   `json:"merged,omitempty"`
	URL     string `json:"url,omitempty"`
	// MergedAt is absent when the PR was never merged (the forge sends null).
	MergedAt string `json:"merged_at,omitempty"`
	// MergeState is the forge's mergeable_state, UPPER-CASED here. REST spells it
	// lowercase while gh reported the same values upper-cased as mergeStateStatus,
	// and callers render that spelling; normalising in one place beats every
	// caller doing it and one of them forgetting.
	MergeState string `json:"merge_state,omitempty"`
	// Mergeable is THREE-valued and must stay that way. The forge answers null
	// while it is still computing the merge, which is NOT the same as "cannot
	// merge": treating null as false tells a caller a mergeable PR is conflicted,
	// and it gives up on a merge that would have succeeded a second later. A nil
	// pointer here (the field omitted on the wire) means "not known yet".
	Mergeable *bool `json:"mergeable,omitempty"`
}

// ForgeResponse reports the HTTP status alongside the parsed answer so a caller
// can tell "the forge said no" from "we never reached it": Status 0 with an
// Error is a transport failure, and conflating the two is how a network blip
// gets reported as a rejected merge.
type ForgeResponse struct {
	Status        int           `json:"status"`
	Error         string        `json:"error,omitempty"`
	DefaultBranch string        `json:"default_branch,omitempty"`
	Pull          *PullSummary  `json:"pull,omitempty"`
	Pulls         []PullSummary `json:"pulls,omitempty"`
	Merged        bool          `json:"merged,omitempty"`
	MergeSHA      string        `json:"merge_sha,omitempty"`
	AlreadyMerged bool          `json:"already_merged,omitempty"`
	// Conflict is TERMINAL, Retryable is a lost race. Both arrive as 405/409 and
	// the message is the only discriminator the forge gives, so they are reported
	// apart here rather than left for a caller to guess: retrying a conflict
	// reproduces it exactly, and giving up on a lost race abandons a merge that
	// would have succeeded.
	Conflict  bool `json:"conflict,omitempty"`
	Retryable bool `json:"retryable,omitempty"`
}

// isMergeConflict matches the forge's conflict wording, observed live as "Pull
// Request has merge conflicts". The distinctive NOUN PHRASE is matched, not the
// whole sentence, so a rewording ("has merge conflict", "merge conflicts
// detected") still classifies.
//
// Bare "conflict" is deliberately NOT matched: HTTP 409 is literally named
// Conflict and its lost-race messages carry the bare word — precisely the case
// that must stay retryable. Requiring the pair keeps this from terminating runs
// a retry would have won.
func isMergeConflict(message string) bool {
	return strings.Contains(strings.ToLower(message), "merge conflict")
}

func nameOK(s string) bool {
	if s == "" || len(s) > 100 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
			c == '-' || c == '_' || c == '.' {
			continue
		}
		return false
	}
	return true
}

// call performs one request. The token is set on the header and nowhere else.
func forgeCall(method, path, token string, body any) (int, []byte, error) {
	var reader io.Reader
	if body != nil {
		encoded, err := json.Marshal(body)
		if err != nil {
			return 0, nil, err
		}
		reader = bytes.NewReader(encoded)
	}
	request, err := http.NewRequest(method, forgeBaseURL+path, reader)
	if err != nil {
		return 0, nil, err
	}
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("Accept", forgeAccept)
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	response, err := forgeHTTPClient.Do(request)
	if err != nil {
		return 0, nil, err
	}
	defer response.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(response.Body, int64(bus.ModuleMessageMaxBody)))
	if err != nil {
		return response.StatusCode, nil, err
	}
	return response.StatusCode, payload, nil
}

// forgeError extracts the forge's own message. GitHub answers a rejected
// operation with a JSON "message", and repeating it is far more useful to an
// operator than the status alone.
func forgeError(status int, payload []byte, what string) string {
	var decoded struct {
		Message string `json:"message"`
	}
	if json.Unmarshal(payload, &decoded) == nil && decoded.Message != "" {
		return fmt.Sprintf("%s: %s (HTTP %d)", what, decoded.Message, status)
	}
	return fmt.Sprintf("%s: HTTP %d", what, status)
}

func summarize(raw []byte) *PullSummary {
	var decoded struct {
		Number int    `json:"number"`
		State  string `json:"state"`
		Title  string `json:"title"`
		Draft  bool   `json:"draft"`
		Merged bool   `json:"merged"`
		URL    string `json:"html_url"`
		// Pointer, so "still computing" (null) stays distinct from false.
		Mergeable  *bool  `json:"mergeable"`
		MergedAt   string `json:"merged_at"`
		MergeState string `json:"mergeable_state"`
		Head       struct {
			Ref string `json:"ref"`
			SHA string `json:"sha"`
		} `json:"head"`
		Base struct {
			Ref string `json:"ref"`
		} `json:"base"`
	}
	if json.Unmarshal(raw, &decoded) != nil {
		return nil
	}
	return &PullSummary{
		Number: decoded.Number, State: decoded.State, Title: decoded.Title,
		Head: decoded.Head.Ref, Base: decoded.Base.Ref, HeadSHA: decoded.Head.SHA,
		Draft: decoded.Draft, Merged: decoded.Merged, URL: decoded.URL,
		MergedAt: decoded.MergedAt, MergeState: strings.ToUpper(decoded.MergeState),
		Mergeable: decoded.Mergeable,
	}
}

// PerformForge runs one forge operation. Exported for the handler and the tests;
// the decisions — which endpoint, which method, what the answer means — all live
// here rather than in the caller.
func PerformForge(request ForgeRequest) ForgeResponse {
	if !nameOK(request.Owner) || !nameOK(request.Repo) {
		return ForgeResponse{Error: "forge: owner/repo is not a valid name"}
	}
	if request.Token == "" {
		return ForgeResponse{Error: "forge: no credential"}
	}
	repo := "/repos/" + request.Owner + "/" + request.Repo

	switch request.Op {
	case OpDefaultBranch:
		// The bare form: GitHub 404s a trailing slash on this endpoint, and a
		// caller must never fall back to guessing "main" — a repo whose default
		// is "testing" would get its PR opened against the wrong branch.
		status, payload, err := forgeCall(http.MethodGet, repo, request.Token, nil)
		if err != nil {
			return ForgeResponse{Error: "forge: " + err.Error()}
		}
		if status < 200 || status > 299 {
			return ForgeResponse{Status: status, Error: forgeError(status, payload, "default branch")}
		}
		var decoded struct {
			DefaultBranch string `json:"default_branch"`
		}
		if json.Unmarshal(payload, &decoded) != nil || decoded.DefaultBranch == "" {
			return ForgeResponse{Status: status, Error: "default branch: unreadable response"}
		}
		return ForgeResponse{Status: status, DefaultBranch: decoded.DefaultBranch}

	case OpPRCreate:
		body := map[string]any{
			"title": request.Title, "head": request.Head,
			"base": request.Base, "body": request.Body,
		}
		if request.Draft {
			body["draft"] = true
		}
		status, payload, err := forgeCall(http.MethodPost, repo+"/pulls", request.Token, body)
		if err != nil {
			return ForgeResponse{Error: "forge: " + err.Error()}
		}
		if status < 200 || status > 299 {
			return ForgeResponse{Status: status, Error: forgeError(status, payload, "pr create")}
		}
		return ForgeResponse{Status: status, Pull: summarize(payload)}

	case OpPRFindOpen, OpPRListOpen:
		query := url.Values{"state": {"open"}}
		if request.Op == OpPRFindOpen {
			if request.Head != "" {
				// GitHub wants owner-qualified head for this filter.
				query.Set("head", request.Owner+":"+request.Head)
			}
			// The BASE matters as much as the head. "Is there already an open PR
			// for this branch?" is only answered correctly per target branch: the
			// same head can have an open PR into one base and none into another,
			// and filtering by head alone answers yes for the wrong one.
			if request.Base != "" {
				query.Set("base", request.Base)
			}
		}
		if request.Op == OpPRListOpen {
			// Most recently updated first. Without this the forge returns
			// creation order, so a "latest open PRs" listing silently shows the
			// OLDEST ones once there are more than one page's worth.
			query.Set("sort", "updated")
			query.Set("direction", "desc")
		}
		if request.Limit > 0 {
			query.Set("per_page", fmt.Sprint(request.Limit))
		}
		status, payload, err := forgeCall(http.MethodGet, repo+"/pulls?"+query.Encode(),
			request.Token, nil)
		if err != nil {
			return ForgeResponse{Error: "forge: " + err.Error()}
		}
		if status < 200 || status > 299 {
			return ForgeResponse{Status: status, Error: forgeError(status, payload, "pr list")}
		}
		var raw []json.RawMessage
		if json.Unmarshal(payload, &raw) != nil {
			return ForgeResponse{Status: status, Error: "pr list: unreadable response"}
		}
		out := ForgeResponse{Status: status}
		for _, item := range raw {
			if summary := summarize(item); summary != nil {
				out.Pulls = append(out.Pulls, *summary)
			}
		}
		if request.Op == OpPRFindOpen && len(out.Pulls) > 0 {
			first := out.Pulls[0]
			out.Pull = &first
		}
		return out

	case OpPRInfo:
		if request.Number <= 0 {
			return ForgeResponse{Error: "pr info: a positive number is required"}
		}
		status, payload, err := forgeCall(http.MethodGet,
			fmt.Sprintf("%s/pulls/%d", repo, request.Number), request.Token, nil)
		if err != nil {
			return ForgeResponse{Error: "forge: " + err.Error()}
		}
		if status < 200 || status > 299 {
			return ForgeResponse{Status: status, Error: forgeError(status, payload, "pr info")}
		}
		return ForgeResponse{Status: status, Pull: summarize(payload)}

	case OpPREdit:
		if request.Number <= 0 {
			return ForgeResponse{Error: "pr edit: a positive number is required"}
		}
		body := map[string]any{}
		if request.Title != "" {
			body["title"] = request.Title
		}
		if request.Body != "" {
			body["body"] = request.Body
		}
		if request.Base != "" {
			body["base"] = request.Base
		}
		if len(body) == 0 {
			return ForgeResponse{Error: "pr edit: nothing to change"}
		}
		status, payload, err := forgeCall(http.MethodPatch,
			fmt.Sprintf("%s/pulls/%d", repo, request.Number), request.Token, body)
		if err != nil {
			return ForgeResponse{Error: "forge: " + err.Error()}
		}
		if status < 200 || status > 299 {
			return ForgeResponse{Status: status, Error: forgeError(status, payload, "pr edit")}
		}
		return ForgeResponse{Status: status, Pull: summarize(payload)}

	case OpPRMerge:
		if request.Number <= 0 {
			return ForgeResponse{Error: "pr merge: a positive number is required"}
		}
		method := request.MergeMethod
		if method == "" {
			method = "merge"
		}
		body := map[string]any{"merge_method": method}
		// Only a squash synthesises a body from the child commits, which is where
		// the attribution trailers protected branches reject come back in. A merge
		// or rebase has nothing to synthesise.
		if method == "squash" {
			body["commit_message"] = ""
		}
		if request.ExpectedHeadSHA != "" {
			body["sha"] = request.ExpectedHeadSHA
		}
		status, payload, err := forgeCall(http.MethodPut,
			fmt.Sprintf("%s/pulls/%d/merge", repo, request.Number), request.Token, body)
		if err != nil {
			return ForgeResponse{Error: "forge: " + err.Error()}
		}
		if status >= 200 && status <= 299 {
			var decoded struct {
				Merged bool   `json:"merged"`
				SHA    string `json:"sha"`
			}
			_ = json.Unmarshal(payload, &decoded)
			// The 2xx body carries the merge commit, so the caller needs no
			// second lookup.
			return ForgeResponse{Status: status, Merged: true, MergeSHA: decoded.SHA}
		}
		message := forgeError(status, payload, "pr merge")
		if status == 405 && strings.Contains(strings.ToLower(string(payload)), "already merged") {
			return ForgeResponse{Status: status, AlreadyMerged: true}
		}
		if status == 405 || status == 409 {
			out := ForgeResponse{Status: status, Error: message}
			if isMergeConflict(message) {
				out.Conflict = true
			} else {
				out.Retryable = true
			}
			return out
		}
		return ForgeResponse{Status: status, Error: message}
	}
	return ForgeResponse{Error: "forge: unsupported operation " + strings.TrimSpace(request.Op)}
}

// handleForgeRequest serves git-forge-request (stage 4).
func handleForgeRequest(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded ForgeRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	encoded, err := json.Marshal(PerformForge(decoded))
	if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
