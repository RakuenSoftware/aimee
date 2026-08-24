package postgres

import (
	"crypto/sha256"
	"encoding/hex"
	"strconv"
)

// checksumOf hashes a migration's statements, as applied, in order.
//
// MIRRORED FROM store_wire.go's StoreChecksum, deliberately, rather than
// imported. The two modules must be able to version independently, and an
// import would make this module's behaviour change when aimee's file changed --
// which is the one thing a contract between two processes may not do quietly.
//
// The cost of mirroring is that the two can drift, and that is what the
// published vectors are for: store_wire.go states four known answers so either
// implementation can be checked against the CONTRACT without the other one
// running. checksum_test.go asserts all four here.
//
// Each statement is written as `len(statement) NUL statement NUL`. The length
// prefix is what keeps a split from colliding with a join -- ["A","B"] and
// ["AB"] must not hash the same, and with only a separator they would.
func checksumOf(statements []string) string {
	h := sha256.New()
	for _, s := range statements {
		h.Write([]byte(strconv.Itoa(len(s))))
		h.Write([]byte{0})
		h.Write([]byte(s))
		h.Write([]byte{0})
	}
	return hex.EncodeToString(h.Sum(nil))
}
