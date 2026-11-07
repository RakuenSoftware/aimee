// aimee-attribution-check is the read-only CI entry point for the forge's policy.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/modules/git/attribution"
)

// Retry only authenticated reads after transient transport/server failures. No
// incomplete response reaches policy evaluation, and exhausted reads still deny.
func readAttributionAPI(ctx context.Context, client *http.Client, endpoint, token string) ([]byte, error) {
	var failure error
	for attempt := 0; attempt < 3; attempt++ {
		if attempt > 0 {
			timer := time.NewTimer(time.Duration(attempt) * time.Second)
			select {
			case <-ctx.Done():
				timer.Stop()
				return nil, ctx.Err()
			case <-timer.C:
			}
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
		if err != nil {
			return nil, err
		}
		req.Header.Set("Authorization", "Bearer "+token)
		req.Header.Set("Accept", "application/vnd.github+json")
		req.Header.Set("X-GitHub-Api-Version", "2022-11-28")
		response, err := client.Do(req)
		if err != nil {
			failure = fmt.Errorf("attribution API transport failed")
			continue
		}
		status := response.StatusCode
		// GitHub owns the first-hop job-log URL. Follow exactly one signed
		// HTTPS download without forwarding the forge token or a cookie jar.
		if status == http.StatusFound && regexp.MustCompile(`^https://api\.github\.com/repos/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/actions/jobs/[1-9][0-9]*/logs$`).MatchString(endpoint) {
			location, parseErr := url.Parse(response.Header.Get("Location"))
			response.Body.Close()
			if parseErr != nil || location.Scheme != "https" || location.Hostname() == "" || location.User != nil || location.Fragment != "" || location.Port() != "" && location.Port() != "443" {
				return nil, fmt.Errorf("invalid attribution log download")
			}
			download, requestErr := http.NewRequestWithContext(ctx, http.MethodGet, location.String(), nil)
			if requestErr != nil {
				return nil, requestErr
			}
			download.Header.Set("Accept", "text/plain")
			owned := *client
			owned.Jar = nil
			owned.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
			response, err = owned.Do(download)
			if err != nil {
				return nil, fmt.Errorf("attribution log download failed")
			}
			status = response.StatusCode
		}
		if status != http.StatusOK {
			response.Body.Close()
			failure = fmt.Errorf("attribution API returned HTTP %d", status)
			if status == 500 || status == 502 || status == 503 || status == 504 {
				continue
			}
			return nil, failure
		}
		const limit = 16 << 20
		body, err := io.ReadAll(io.LimitReader(response.Body, limit+1))
		response.Body.Close()
		if err != nil || len(body) > limit {
			return nil, fmt.Errorf("incomplete attribution API response")
		}
		return body, nil
	}
	return nil, failure
}

func check(ctx context.Context, verdict *attribution.Verdict) error {
	repo, token := os.Getenv("GITHUB_REPOSITORY"), os.Getenv("GH_TOKEN")
	if !regexp.MustCompile(`^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$`).MatchString(repo) || token == "" {
		return fmt.Errorf("repository and authenticated read access are required")
	}
	client := &http.Client{Timeout: 20 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	c := attribution.Checker{Repository: repo, Get: func(ctx context.Context, path string) ([]byte, error) {
		body, err := readAttributionAPI(ctx, client, "https://api.github.com/repos/"+repo+"/"+path, token)
		if err != nil {
			return nil, &attribution.ExecutionError{Code: "api_read", Message: err.Error()}
		}
		return body, nil
	}}
	path := os.Getenv("GITHUB_EVENT_PATH")
	raw, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("cannot read GitHub event")
	}
	var event struct {
		Number int `json:"number"`
		Pull   struct {
			Head struct {
				SHA string `json:"sha"`
			} `json:"head"`
		} `json:"pull_request"`
		Group struct {
			Base string `json:"base_sha"`
			Head string `json:"head_sha"`
		} `json:"merge_group"`
		Before     string `json:"before"`
		Created    bool   `json:"created"`
		Ref        string `json:"ref"`
		Repository struct {
			FullName      string `json:"full_name"`
			DefaultBranch string `json:"default_branch"`
		} `json:"repository"`
		After  string `json:"after"`
		Inputs struct {
			Base string `json:"base_sha"`
			Head string `json:"head_sha"`
		} `json:"inputs"`
	}
	if json.Unmarshal(raw, &event) != nil {
		return fmt.Errorf("malformed GitHub event")
	}
	switch os.Getenv("GITHUB_EVENT_NAME") {
	case "pull_request":
		verdict.HeadSHA = event.Pull.Head.SHA
		if event.Number <= 0 || !attribution.ValidSHA(event.Pull.Head.SHA) {
			return fmt.Errorf("incomplete PR event")
		}
		_, err = c.CheckPull(ctx, event.Number, event.Pull.Head.SHA)
		return err
	case "merge_group":
		verdict.HeadSHA, verdict.BaseSHA = event.Group.Head, event.Group.Base
		return c.CheckRange(ctx, event.Group.Base, event.Group.Head)
	case "push":
		verdict.HeadSHA, verdict.BaseSHA = event.After, event.Before
		if event.Repository.FullName != repo {
			return fmt.Errorf("push repository identity mismatch")
		}
		base, err := c.PushBase(ctx, event.Before, event.After, event.Repository.DefaultBranch, event.Ref, event.Created)
		if err != nil {
			return err
		}
		verdict.BaseSHA = base
		return c.CheckRange(ctx, base, event.After)
	case "workflow_dispatch":
		verdict.HeadSHA, verdict.BaseSHA = event.Inputs.Head, event.Inputs.Base
		return c.CheckRange(ctx, event.Inputs.Base, event.Inputs.Head)
	default:
		return fmt.Errorf("unsupported attribution event")
	}
}

func main() {
	started := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Minute)
	defer cancel()
	verdict := attribution.Verdict{Version: 1, Repository: os.Getenv("GITHUB_REPOSITORY"), Event: os.Getenv("GITHUB_EVENT_NAME"), Outcome: "allowed", Code: "evaluated"}
	verdict.RunID, _ = strconv.ParseInt(os.Getenv("GITHUB_RUN_ID"), 10, 64)
	verdict.RunAttempt, _ = strconv.Atoi(os.Getenv("GITHUB_RUN_ATTEMPT"))
	err := check(ctx, &verdict)
	if err != nil {
		verdict.Outcome, verdict.Code = "denied", "policy_denied_or_unknown"
		var execution *attribution.ExecutionError
		if errors.As(err, &execution) {
			verdict.Outcome, verdict.Code = "execution_error", execution.Code
		}
	}
	if verdict.RunID > 0 && verdict.RunAttempt > 0 && attribution.ValidSHA(verdict.HeadSHA) {
		encoded, _ := json.Marshal(verdict)
		message := strings.NewReplacer("%", "%25", "\r", "%0D", "\n", "%0A").Replace(string(encoded))
		fmt.Printf("::notice title=%s::%s\n", attribution.VerdictTitle, message)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "attribution denied after", time.Since(started).Round(time.Millisecond), ":", err)
		os.Exit(1)
	}
	fmt.Println("all incoming commits and PR provenance satisfy the attribution restriction; verification took", time.Since(started).Round(time.Millisecond))
}
