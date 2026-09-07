package aimee

import "github.com/JBailes/aimee/server-go/db"

// Domain families retain their existing API while the database implementation
// and error identity have one owner. New database consumers import db directly;
// these aliases do not open a pool, register a stage, or duplicate the wire.
type (
	Row              = db.Row
	Rows             = db.Rows
	Tag              = db.Tag
	RowsAffected     = db.RowsAffected
	Queryer          = db.Queryer
	Tx               = db.Tx
	DB               = db.DB
	Store            = db.Store
	StoreError       = db.StoreError
	MigrationRequest = db.MigrationRequest
)

var (
	ErrNoRows           = db.ErrNoRows
	ErrTxClosed         = db.ErrTxClosed
	ErrStoreUnavailable = db.ErrStoreUnavailable
	ErrResultTooLarge   = db.ErrResultTooLarge
)

func IsNoRows(err error) bool                  { return db.IsNoRows(err) }
func IsUniqueViolation(err error) bool         { return db.IsUniqueViolation(err) }
func IsForeignKeyViolation(err error) bool     { return db.IsForeignKeyViolation(err) }
func IsCheckViolation(err error) bool          { return db.IsCheckViolation(err) }
func IsNotNullViolation(err error) bool        { return db.IsNotNullViolation(err) }
func StoreChecksum(statements []string) string { return db.StoreChecksum(statements) }
