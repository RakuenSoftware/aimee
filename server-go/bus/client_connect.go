package bus

import (
	"context"
	"errors"
	"time"

	"golang.org/x/sys/unix"
)

// ConnectClient attaches to a daemon's module bus as a requesting principal.
//
// This is the counterpart to a module process attaching to serve: the daemon
// hosting the bus admits this client under a grant naming the kinds it may
// request, so a caller is a principal of that bus rather than a second bus.
//
// A daemon restart can leave its old socket pathname in place until the new
// host replaces it, so a refused connection is retried until ctx ends. A
// missing socket or a policy refusal fails immediately: those are configuration
// mistakes and waiting only hides them.
func ConnectClient(ctx context.Context, socketPath string, principalClass, principalRef uint32) (*Client, error) {
	if socketPath == "" {
		return nil, ErrModuleConfig
	}
	if ctx == nil {
		ctx = context.Background()
	}
	for {
		fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
		if err != nil {
			return nil, err
		}
		err = unix.Connect(fd, &unix.SockaddrUnix{Name: socketPath})
		if err == nil {
			client, attachErr := AttachAs(fd, principalClass, principalRef)
			unix.Close(fd)
			return client, attachErr
		}
		unix.Close(fd)
		if !errors.Is(err, unix.ECONNREFUSED) {
			return nil, err
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(moduleConnectRetry):
		}
	}
}
