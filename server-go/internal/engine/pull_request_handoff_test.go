package engine

import (
	"strings"
	"testing"
)

func TestPullRequestTitleUsesProposalMeaningInsteadOfWorkItemID(t *testing.T) {
	for _, test := range []struct {
		name    string
		request string
		want    string
	}{
		{name: "proposal heading", request: "# Proposal: appliance state-recovery runbook\n\n- **State:** pending", want: "Appliance state-recovery runbook"},
		{name: "plain request", request: "document automatic proposal admission in three implementation slices", want: "Document automatic proposal admission in three implementation slices"},
		{name: "slice packet", request: `{"packet_id":"p1","summary":"add the admission identity test"}`, want: "Add the admission identity test"},
	} {
		t.Run(test.name, func(t *testing.T) {
			got, err := pullRequestTitle(test.request)
			if err != nil || got != test.want {
				t.Fatalf("pullRequestTitle() = %q, %v; want %q", got, err, test.want)
			}
		})
	}
}

func TestPullRequestTitleRejectsBookkeepingAndBoundsProse(t *testing.T) {
	if _, err := pullRequestTitle("wi_42ca22355e8a97e3ab6bbe9f8de7702c"); err == nil {
		t.Fatal("work-item identifier was accepted as a PR title")
	}
	got, err := pullRequestTitle(strings.Repeat("meaningful words ", 30))
	if err != nil {
		t.Fatal(err)
	}
	if len([]rune(got)) > maxPullRequestTitleRunes || !strings.HasSuffix(got, "…") {
		t.Fatalf("long title was not bounded cleanly: %q", got)
	}
}
