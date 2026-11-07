package families

import (
	"context"
	"errors"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"strings"
	"unicode/utf8"
)

const legacyPrimaryTextMaxBytes = 16 << 20
const legacyPrimaryTextChunkCharacters = 65536
const legacyPrimaryTextPageChunks = 32

const legacyPrimaryTextInfoSQL = `SELECT octet_length(messages_json),length(messages_json)
 FROM primary_sessions WHERE session_id=$1 AND agent_name=$2 AND provider=$3 FOR SHARE`
const legacyPrimaryTextChunksSQL = `SELECT part,substring(messages_json FROM (part*65536+1)::integer FOR 65536)
 FROM primary_sessions CROSS JOIN generate_series($4::bigint,$5::bigint) AS part
 WHERE session_id=$1 AND agent_name=$2 AND provider=$3 ORDER BY part`

var errLegacyPrimaryReplyTooLong = errors.New("legacy primary reply exceeds module frame")

// One raw source field plus all other output fields, each length-prefixed.
// Storage capacity does not remove the existing complete-reply frame overhead.
func legacyPrimaryReplyCapacity(otherFields ...string) int {
	budget := int(ring.ModuleMessageMaxBody) - 8 - 4*(len(otherFields)+1)
	for _, field := range otherFields {
		budget -= len(field)
	}
	return budget
}

func legacyPrimaryText(ctx context.Context, q store.Queryer, session, agent, provider string) (string, error) {
	return legacyPrimaryTextBounded(ctx, q, session, agent, provider, legacyPrimaryTextMaxBytes)
}

// The caller owns a transaction. Hold the exact legacy tuple across metadata
// and chunk reads, so a concurrent replacement cannot splice different source
// revisions. Character slices preserve PostgreSQL UTF-8 boundaries; each cell
// is at most 256KiB and each 32-cell query fits below the SQL reply ceiling.
func legacyPrimaryTextBounded(ctx context.Context, q store.Queryer, session, agent, provider string, replyCapacity int) (string, error) {
	var size, characters int64
	if err := q.QueryRow(ctx, legacyPrimaryTextInfoSQL, session, agent, provider).Scan(&size, &characters); err != nil {
		return "", err
	}
	if size < 0 || size > legacyPrimaryTextMaxBytes || characters < 0 || characters > size {
		return "", fmt.Errorf("legacy primary source exceeds admitted text budget")
	}
	if size > int64(replyCapacity) {
		return "", errLegacyPrimaryReplyTooLong
	}
	var text strings.Builder
	text.Grow(int(size))
	parts := (characters + legacyPrimaryTextChunkCharacters - 1) / legacyPrimaryTextChunkCharacters
	for first := int64(0); first < parts; first += legacyPrimaryTextPageChunks {
		last := first + legacyPrimaryTextPageChunks - 1
		if last >= parts {
			last = parts - 1
		}
		rows, err := q.Query(ctx, legacyPrimaryTextChunksSQL, session, agent, provider, first, last)
		if err != nil {
			return "", err
		}
		expected := first
		for rows.Next() {
			var index int64
			var piece string
			if err = rows.Scan(&index, &piece); err != nil {
				rows.Close()
				return "", err
			}
			if index != expected || piece == "" || len(piece) > 4*legacyPrimaryTextChunkCharacters || !utf8.ValidString(piece) || text.Len() > int(size)-len(piece) {
				rows.Close()
				return "", fmt.Errorf("invalid legacy primary source chunk")
			}
			text.WriteString(piece)
			expected++
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return "", err
		}
		if expected != last+1 {
			return "", fmt.Errorf("incomplete legacy primary source chunks")
		}
	}
	if text.Len() != int(size) || int64(utf8.RuneCountInString(text.String())) != characters {
		return "", fmt.Errorf("legacy primary source length changed")
	}
	return text.String(), nil
}
