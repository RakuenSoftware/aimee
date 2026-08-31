//go:build tools

// Package tools pins build-time modules that are compiled as standalone
// executables by the container build rather than linked into an Aimee package.
package tools

import _ "github.com/RakuenSoftware/aimee-module-config/server-go/modules/config"
