package families

import (
	"context"
	"encoding/json"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strings"
	"testing"
)

func TestSessionIRSearchRequiresHostBoundActorBeforeDatabase(t *testing.T) {
	db := newSessDB()
	for _, fields := range [][]string{{"", `{"_principal":""}`}, {"alice", `{"_principal":"bob"}`}, {"alice", `{"_principal":"alice","limit":1e30}`}, {"alice", `{"_principal":"alice","offset":-1}`}} {
		status, _, err := sessionIRSearch(context.Background(), db, fields)
		if err != nil || status != store.StatusInvalid || len(db.executed) != 0 {
			t.Fatal("invalid actor/query reached database", status, err, db.executed)
		}
	}
	frame, err := wire.EncodeFields(opSessionIRSearch, []string{"alice", `{"_principal":"alice"}`})
	if err != nil {
		t.Fatal(err)
	}
	_, status := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: 9}, frame)
	if status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("external principal field granted source access")
	}
}
func TestSessionIRSearchBoundedSnippetRetainsMatch(t *testing.T) {
	text := strings.Repeat("界", 6000) + "needle"
	clipped, truncated := searchClip(text, "needle")
	if !truncated || !strings.Contains(clipped, "needle") || len([]rune(clipped)) != 4096 {
		t.Fatal("bounded snippet lost actual match")
	}
	if got, truncated := searchClip("ordinary", "ordinary"); got != "ordinary" || truncated {
		t.Fatal("ordinary display changed")
	}
}

func TestSessionIRSearchCanonicalDisplayRespectsContext(t *testing.T) {
	var message canonical.Message
	encoded := `{"role":"tool","blocks":[{"type":"text","text":"visible"},{"type":"tool_result","tool_id":"x","content":"tool output"},{"type":"tool_result","tool_id":"y","content":{"text":"opaque structured"}},{"type":"text","text":"hidden","context":{"origin":0,"authority":0,"trust":0,"sensitivity":0,"model_visible":false,"revision_domain":"","revision_scope":"","revision_epoch":"0"}},{"type":"text","text":"private","context":{"origin":0,"authority":0,"trust":0,"sensitivity":2,"model_visible":true,"revision_domain":"","revision_scope":"","revision_epoch":"0"}},{"type":"thinking","text":"reasoning"}]}`
	if err := json.Unmarshal([]byte(encoded), &message); err != nil {
		t.Fatal(err)
	}
	if got := searchText(message); got != "visible\ntool output" {
		t.Fatal("context or opaque values leaked", got)
	}
}
