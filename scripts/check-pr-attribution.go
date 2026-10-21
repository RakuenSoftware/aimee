// aimee-attribution-check is the read-only CI entry point for the forge's policy.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
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

func check(ctx context.Context) error {
	repo, token := os.Getenv("GITHUB_REPOSITORY"), os.Getenv("GH_TOKEN")
	if !regexp.MustCompile(`^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$`).MatchString(repo) || token == "" {
		return fmt.Errorf("repository and authenticated read access are required")
	}
	client := &http.Client{Timeout: 20 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	c := attribution.Checker{Get: func(ctx context.Context, path string) ([]byte, error) {
		return readAttributionAPI(ctx, client, "https://api.github.com/repos/"+repo+"/"+path, token)
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
		Before string `json:"before"`
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
		if event.Number <= 0 || !attribution.ValidSHA(event.Pull.Head.SHA) {
			return fmt.Errorf("incomplete PR event")
		}
		_, err = c.CheckPull(ctx, event.Number, event.Pull.Head.SHA)
		return err
	case "merge_group":
		return c.CheckRange(ctx, event.Group.Base, event.Group.Head)
	case "push":
		return c.CheckRange(ctx, event.Before, event.After)
	case "workflow_dispatch":
		return c.CheckRange(ctx, event.Inputs.Base, event.Inputs.Head)
	default:
		return fmt.Errorf("unsupported attribution event")
	}
}

func main() {
	started := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Minute)
	defer cancel()
	if err := check(ctx); err != nil {
		fmt.Fprintln(os.Stderr, "attribution denied after", time.Since(started).Round(time.Millisecond), ":", err)
		os.Exit(1)
	}
	fmt.Println("all incoming commits and PR provenance satisfy the attribution restriction; verification took", time.Since(started).Round(time.Millisecond))
}
