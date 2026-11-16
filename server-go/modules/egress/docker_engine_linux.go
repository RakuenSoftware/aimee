package egress

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"syscall"

	"golang.org/x/sys/unix"
)

// OpenDockerEngine holds the configured Unix socket inode. Redirects, proxies,
// caller URLs and TCP fallback cannot select another control destination.
func OpenDockerEngine(ctx context.Context, socket string) (*DockerEngine, error) {
	if !filepath.IsAbs(socket) {
		return nil, errors.New("absolute Docker Unix socket required")
	}
	fd, err := unix.Open(socket, unix.O_PATH|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, errors.New("Docker Unix socket unavailable")
	}
	source := os.NewFile(uintptr(fd), "docker-control")
	info, err := source.Stat()
	if err != nil || info.Mode()&os.ModeSocket == 0 {
		source.Close()
		return nil, errors.New("Docker control resource is not a socket")
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		source.Close()
		return nil, errors.New("Docker control resource owner unavailable")
	}
	target := fmt.Sprintf("/proc/%d/fd/%d", os.Getpid(), source.Fd())
	transport := &http.Transport{DisableKeepAlives: true, MaxResponseHeaderBytes: 65536}
	transport.DialContext = func(ctx context.Context, _, _ string) (net.Conn, error) {
		connection, err := (&net.Dialer{}).DialContext(ctx, "unix", target)
		if err != nil {
			return nil, err
		}
		unixConnection, ok := connection.(*net.UnixConn)
		if !ok {
			connection.Close()
			return nil, errors.New("Docker control connection is not Unix")
		}
		raw, err := unixConnection.SyscallConn()
		var peer *unix.Ucred
		if err == nil {
			var credentialErr error
			err = raw.Control(func(fd uintptr) {
				peer, credentialErr = unix.GetsockoptUcred(int(fd), unix.SOL_SOCKET, unix.SO_PEERCRED)
			})
			if err == nil {
				err = credentialErr
			}
		}
		if err != nil || peer == nil || peer.Uid != stat.Uid {
			connection.Close()
			return nil, errors.New("Docker control peer differs from socket owner")
		}
		return connection, nil
	}
	engine := &DockerEngine{source: source, transport: transport,
		client: &http.Client{Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error {
			return errors.New("Docker control redirects refused")
		}}}
	if err := engine.negotiate(ctx); err != nil {
		engine.Close()
		return nil, err
	}
	return engine, nil
}
