package families

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
)

func TestDeviceManagementRejectsInvalidAuthorityAndInputsBeforeWriting(t *testing.T) {
	for _, tc := range []struct {
		principal, action, id, name string
		code                        int
	}{
		{"cert:device", "create", testBearer, "Laptop", 403},
		{"webuser:alice", "create", testBearer, "", 400},
		{"webuser:alice", "create", testBearer, strings.Repeat("x", 65), 400},
		{"webuser:alice", "create", testBearer, "bad\nname", 400},
		{"webuser:alice", "revoke", "not-an-id", "", 400},
		{"webuser:alice", "revoke_all", "", "", 403},
		{"", "authorize", "bad-token", "", 401},
	} {
		db := newIdentDB()
		_, cells, err := remoteClientManage(context.Background(), db, []string{tc.principal, tc.action, tc.id, tc.name, "1000"})
		if err != nil {
			t.Fatal(err)
		}
		var body struct {
			Code int `json:"code"`
		}
		if err = json.Unmarshal([]byte(cells[0]), &body); err != nil {
			t.Fatal(err)
		}
		if body.Code != tc.code {
			t.Fatalf("%s got %d want %d", tc.action, body.Code, tc.code)
		}
		if len(db.executed) != 0 {
			t.Fatal("invalid request reached storage")
		}
	}
}

func TestDeviceManagementCannotReplaceAnotherOwner(t *testing.T) {
	for _, action := range []string{"list", "revoke"} {
		db := newIdentDB()
		db.owner = "webuser:alice"
		_, cells, err := remoteClientManage(context.Background(), db, []string{"webuser:bob", action, testBearer, "Laptop", "1000"})
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(cells[0], `"code":403`) {
			t.Fatalf("%s: %v", action, cells)
		}
		for _, sql := range db.executed {
			if strings.Contains(sql, "UPDATE") && !strings.Contains(sql, "FOR UPDATE") {
				t.Fatal("other owner mutated grants")
			}
		}
	}
}

func TestExpiredOrRevokedInvitationCannotBecomeAnOperatorCertificate(t *testing.T) {
	db := newIdentDB()
	db.grant = &grant{principal: "webuser:alice", bearer: testBearer, serial: "!unavailable", tier: "full"}
	_, cells := identCall(t, db, opRemoteClientBind, []string{testBearer, "AABB", "1000"})
	if len(cells) != 1 || cells[0] != "-1" {
		t.Fatalf("unavailable invitation returned %v", cells)
	}
	if wrote(db, "UPDATE remote_client_grants") {
		t.Fatal("expired invitation bound a certificate")
	}
}
