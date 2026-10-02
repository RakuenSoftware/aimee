package postgres

import (
	"errors"
	"os"
	"strings"

	"github.com/jackc/pgx/v5/pgxpool"
)

// Use the parsed driver's TLS policy, never substring-match a credential. A
// verify-ca connection is insufficient: the hardened tier requires hostname
// verification as well, including every fallback address.
func validateRuntimeTLS(config *pgxpool.Config) error {
	value := strings.ToLower(os.Getenv("AIMEE_KB_HARDENED"))
	if value == "" || !strings.ContainsRune("1ty", rune(value[0])) {
		return nil
	}
	fail := errors.New("postgres: hardened runtime requires sslmode=verify-full")
	if config.ConnConfig.TLSConfig == nil || config.ConnConfig.TLSConfig.InsecureSkipVerify {
		return fail
	}
	for _, fallback := range config.ConnConfig.Fallbacks {
		if fallback.TLSConfig == nil || fallback.TLSConfig.InsecureSkipVerify {
			return fail
		}
	}
	return nil
}
