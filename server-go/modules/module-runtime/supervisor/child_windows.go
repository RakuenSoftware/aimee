package supervisor

import (
	"context"
	"errors"
)

func child(context.Context, string, string, string, string) error {
	return errors.New("local module compositions require a Unix host")
}
