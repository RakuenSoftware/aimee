package families

import (
	"context"
	"encoding/json"
	"strings"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const opRemoteClientManage uint32 = 5

func deviceReply(code int, fields map[string]any) (uint32, []string, error) {
	if fields == nil {
		fields = map[string]any{}
	}
	fields["code"] = code
	data, err := json.Marshal(fields)
	return store.StatusOK, []string{string(data)}, err
}

// Client management shares the first owner's authority, but each device has a
// distinct enrollment digest and certificate. Cleartext pairing tokens never
// enter the database. Only unbound invitations expire; paired devices retain
// their identity until individually revoked (or their PKI certificate expires).
func remoteClientManage(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	principal, action, id, name := f[0], f[1], f[2], strings.TrimSpace(f[3])
	now, valid := store.Atoi64(f[4])
	if !valid || now < 0 {
		return deviceReply(400, nil)
	}
	if action == "revoke_all" {
		if principal != "" {
			return deviceReply(403, nil)
		}
		_, err := db.Exec(ctx, `UPDATE remote_client_grants SET revoked_at=coalesce(revoked_at,$1)`, now)
		if err != nil {
			return 0, nil, err
		}
		return deviceReply(200, nil)
	}
	if action == "authorize" {
		if !lowercaseHex(id, bearerHashLen, bearerHashLen) {
			return deviceReply(401, nil)
		}
		var found string
		err := db.QueryRow(ctx, `SELECT bearer_sha256 FROM remote_client_grants
    WHERE bearer_sha256=$1 AND revoked_at IS NULL
    AND (cert_serial IS NOT NULL OR expires_at=0 OR expires_at>$2)`, id, now).Scan(&found)
		if store.IsNoRows(err) {
			return deviceReply(401, nil)
		}
		if err != nil {
			return 0, nil, err
		}
		return deviceReply(200, nil)
	}
	if !printableBounded(principal, 9, principalMax) || !strings.HasPrefix(principal, principalPrefix) {
		return deviceReply(403, nil)
	}
	if action != "list" && action != "create" && action != "revoke" {
		return deviceReply(400, nil)
	}
	if action != "list" && !lowercaseHex(id, bearerHashLen, bearerHashLen) {
		return deviceReply(400, nil)
	}
	if action == "create" && !printableBounded(name, 1, 64) {
		return deviceReply(400, nil)
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()
	// Serialize owner claims and device mutations across processes. A browser
	// cannot replace the owner, including before any client has been paired.
	if action == "create" {
		if _, err = tx.Exec(ctx, `INSERT INTO remote_first_user(singleton,principal,created_at)
    VALUES(true,$1,$2) ON CONFLICT(singleton) DO NOTHING`, principal, now); err != nil {
			return 0, nil, err
		}
	}
	var owner string
	err = tx.QueryRow(ctx, firstUserSQL+" FOR UPDATE").Scan(&owner)
	if store.IsNoRows(err) {
		if action == "list" {
			return deviceReply(200, map[string]any{"clients": []any{}})
		}
		return deviceReply(404, nil)
	}
	if err != nil {
		return 0, nil, err
	}
	if owner != principal {
		return deviceReply(403, nil)
	}
	switch action {
	case "create":
		var count int
		err = tx.QueryRow(ctx, `SELECT count(*) FROM remote_client_grants WHERE revoked_at IS NULL
    AND (cert_serial IS NOT NULL OR expires_at=0 OR expires_at>$1)`, now).Scan(&count)
		if err != nil {
			return 0, nil, err
		}
		if count >= 64 {
			return deviceReply(409, map[string]any{"error": "Client limit reached; revoke an unused client first"})
		}
		_, err = tx.Exec(ctx, `INSERT INTO remote_client_grants
    (bearer_sha256,principal,tier,created_at,device_name,expires_at)
    VALUES($1,$2,'full',$3,$4,$5)`, id, principal, now, name, now+900)
		if err != nil {
			return 0, nil, err
		}
		if err = tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return deviceReply(200, map[string]any{"id": id, "name": name, "expires_at": now + 900})
	case "revoke":
		var serial string
		err = tx.QueryRow(ctx, `UPDATE remote_client_grants SET revoked_at=coalesce(revoked_at,$1)
    WHERE bearer_sha256=$2 AND principal=$3 RETURNING coalesce(cert_serial,'')`, now, id, principal).Scan(&serial)
		if store.IsNoRows(err) {
			return deviceReply(404, nil)
		}
		if err != nil {
			return 0, nil, err
		}
		if err = tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return deviceReply(200, map[string]any{"serial": serial})
	default:
		rows, err := tx.Query(ctx, `SELECT bearer_sha256,device_name,coalesce(cert_serial,''),created_at,
    coalesce(bound_at,0),expires_at,coalesce(revoked_at,0) FROM remote_client_grants
    WHERE principal=$1 ORDER BY
    (revoked_at IS NULL AND (cert_serial IS NOT NULL OR expires_at=0 OR expires_at>$2)) DESC,
    created_at DESC LIMIT 256`, principal, now)
		if err != nil {
			return 0, nil, err
		}
		clients := []map[string]any{}
		for rows.Next() {
			var hash, label, serial string
			var created, bound, expires, revoked int64
			if err = rows.Scan(&hash, &label, &serial, &created, &bound, &expires, &revoked); err != nil {
				rows.Close()
				return 0, nil, err
			}
			state := "pending"
			if serial != "" {
				state = "paired"
			} else if expires > 0 && expires <= now {
				state = "expired"
			}
			if revoked > 0 {
				state = "revoked"
			}
			clients = append(clients, map[string]any{"id": hash, "name": label, "serial": serial, "state": state, "created_at": created, "bound_at": bound, "expires_at": expires})
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return 0, nil, err
		}
		if err = tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return deviceReply(200, map[string]any{"clients": clients})
	}
}
