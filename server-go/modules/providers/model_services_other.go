//go:build !linux

package providers

import (
	"context"
	"errors"
)

func modelServicesRootIsVolatile(string) error {
	return errors.New("local model containers require Linux")
}

func modelServicesLock(context.Context, string) (func(), error) {
	return nil, errors.New("local model containers require Linux")
}
