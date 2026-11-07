// Package memoryadapter reports the absence of a bundled memory implementation.
// The role supervisor selects independently installed executables; no datastore,
// memory policy, Handler, bus attachment or serving readiness is provided here.
package memoryadapter

import "errors"

var errUnavailable = errors.New("memory capability unavailable: install an external memory implementation and select its absolute executable in the role module manifest; no built-in memory owner is provided")

func Unavailable() error { return errUnavailable }
