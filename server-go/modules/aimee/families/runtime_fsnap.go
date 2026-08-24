package families

import (
	"context"
	"errors"
	"os"
	"path/filepath"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The two file-snapshot operations that touch the disk.
//
// Everything else in this store moves rows around. These read and write actual
// files, because that is what a snapshot IS: recording one reads the file, and
// restoring one puts it back. The paths are the caller's -- an agent snapshots
// the files it is about to edit -- so there is no directory to confine them to
// the way the ensemble templates have one.

const (
	fsnapRecordFileSQL = `INSERT INTO file_snapshot_entries (snapshot_id, path, existed, content)
	                      VALUES ($1, $2, $3, $4)
	                      ON CONFLICT (snapshot_id, path) DO UPDATE SET
	                          existed = EXCLUDED.existed,
	                          content = EXCLUDED.content`

	fsnapRestoreReadSQL = `SELECT path, existed, content
	                         FROM file_snapshot_entries WHERE snapshot_id = $1
	                         ORDER BY id`

	fsnapExistsSQL = `SELECT 1 FROM file_snapshots WHERE id = $1`
)

// fsnapRecordFile stores what a file looks like now, so it can be put back.
//
// A file that does not exist is recorded as not existing, with no content. That
// is not an error and not an omission: restoring the snapshot has to DELETE it
// again, and a missing row would silently leave it behind.
//
// The C deleted any previous entry and inserted a new one as two statements
// with nothing between them. ON CONFLICT is the same intent in one, and the
// uniqueness constraint is what makes "one row per path" true rather than
// hoped for.
func fsnapRecordFile(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	snapID, ok := store.Atoi64(f[0])
	if !ok || snapID <= 0 || f[1] == "" {
		return store.StatusInvalid, nil, nil
	}

	content, err := os.ReadFile(f[1])
	switch {
	case errors.Is(err, os.ErrNotExist):
		// Recorded as absent, with no content.
		if _, err := q.Exec(ctx, fsnapRecordFileSQL, snapID, f[1], false, nil); err != nil {
			return store.StatusFailed, nil, err
		}
		return store.StatusOK, nil, nil
	case err != nil:
		// A file that exists but cannot be read is NOT recorded as absent:
		// doing that would have the restore delete a file whose contents were
		// never captured.
		return store.StatusFailed, nil, err
	}

	if _, err := q.Exec(ctx, fsnapRecordFileSQL, snapID, f[1], true, content); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// fsnapRestore puts a snapshot's files back, and answers with how many it wrote
// and how many it removed.
//
// A file recorded as absent is deleted, because that is what the directory
// looked like. A partial restore is a FAILURE rather than a partial success:
// half a snapshot is a state the workspace was never in, and telling the caller
// "restored 3 of 5" invites it to carry on as though the rollback worked.
func fsnapRestore(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	snapID, ok := store.Atoi64(f[0])
	if !ok || snapID <= 0 {
		return store.StatusInvalid, nil, nil
	}

	var one int
	err := q.QueryRow(ctx, fsnapExistsSQL, snapID).Scan(&one)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}

	rows, err := q.Query(ctx, fsnapRestoreReadSQL, snapID)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	type entry struct {
		path    string
		existed bool
		content []byte
	}
	var entries []entry
	for rows.Next() {
		var e entry
		if err := rows.Scan(&e.path, &e.existed, &e.content); err != nil {
			rows.Close()
			return store.StatusFailed, nil, err
		}
		entries = append(entries, e)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return store.StatusFailed, nil, err
	}

	restored, deleted := 0, 0
	for _, e := range entries {
		if !e.existed {
			if err := os.Remove(e.path); err != nil && !errors.Is(err, os.ErrNotExist) {
				return store.StatusFailed, nil, err
			}
			deleted++
			continue
		}
		// The directory may have gone away with the file.
		if err := os.MkdirAll(filepath.Dir(e.path), 0o755); err != nil {
			return store.StatusFailed, nil, err
		}
		if err := os.WriteFile(e.path, e.content, 0o644); err != nil {
			return store.StatusFailed, nil, err
		}
		restored++
	}

	return store.StatusOK, []string{
		store.Itoa(restored), store.Itoa(deleted),
	}, nil
}
