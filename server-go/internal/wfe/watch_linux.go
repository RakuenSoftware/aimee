package wfe

import (
	"context"
	"errors"

	"golang.org/x/sys/unix"
)

// Events a caller treats as "the directory's contents may differ now": a file
// written and closed, created, removed, or renamed into or out of it.
const watchEvents = unix.IN_CLOSE_WRITE | unix.IN_CREATE | unix.IN_DELETE |
	unix.IN_MOVED_TO | unix.IN_MOVED_FROM

// WatchDefinitions calls changed after each batch of changes to the workflow
// definitions in dir until ctx ends, from the kernel (inotify), so the trigger
// scan does not re-read them on a timer. It does not descend into
// subdirectories, which the registry does not read either. changed runs on
// this goroutine; a burst of edits may arrive as one call.
func WatchDefinitions(ctx context.Context, dir string, changed func()) error {
	notify, err := unix.InotifyInit1(unix.IN_CLOEXEC | unix.IN_NONBLOCK)
	if err != nil {
		return err
	}
	defer unix.Close(notify)
	if _, err := unix.InotifyAddWatch(notify, dir, watchEvents); err != nil {
		return err
	}
	stop, err := unix.Eventfd(0, unix.EFD_CLOEXEC|unix.EFD_NONBLOCK)
	if err != nil {
		return err
	}
	defer unix.Close(stop)
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			var one [8]byte
			one[0] = 1
			_, _ = unix.Write(stop, one[:])
		case <-done:
		}
	}()

	buffer := make([]byte, 64*(unix.SizeofInotifyEvent+unix.NAME_MAX+1))
	for {
		fds := []unix.PollFd{{Fd: int32(notify), Events: unix.POLLIN},
			{Fd: int32(stop), Events: unix.POLLIN}}
		if _, err := unix.Poll(fds, -1); err != nil && !errors.Is(err, unix.EINTR) {
			return err
		}
		if ctx.Err() != nil {
			return nil
		}
		read := false
		for {
			n, err := unix.Read(notify, buffer)
			if errors.Is(err, unix.EAGAIN) || errors.Is(err, unix.EINTR) {
				break
			}
			if err != nil {
				return err
			}
			if n > 0 {
				read = true
			}
		}
		if read {
			changed()
		}
	}
}
