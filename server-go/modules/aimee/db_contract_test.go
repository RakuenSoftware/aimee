package aimee

import (
	"errors"
	"fmt"
	"testing"

	"github.com/JBailes/aimee/server-go/db"
)

func TestDatabaseErrorsHaveOneIdentityAcrossDomains(t *testing.T) {
	for _, pair := range []struct{ domain, shared error }{
		{ErrNoRows, db.ErrNoRows},
		{ErrTxClosed, db.ErrTxClosed},
		{ErrStoreUnavailable, db.ErrStoreUnavailable},
		{ErrResultTooLarge, db.ErrResultTooLarge},
	} {
		if pair.domain != pair.shared || !errors.Is(fmt.Errorf("operation: %w", pair.shared), pair.domain) {
			t.Fatal("domain and shared database errors diverged")
		}
	}
	err := fmt.Errorf("operation: %w", &db.StoreError{SQLState: "23505", Message: "duplicate"})
	var domain *StoreError
	if !errors.As(err, &domain) || !IsUniqueViolation(err) {
		t.Fatal("shared SQLSTATE lost its domain classification")
	}
}
