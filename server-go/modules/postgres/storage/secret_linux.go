//go:build linux

package storage

import (
	"errors"
	"golang.org/x/sys/unix"
)

// secret owns a locked, non-dumpable mapping; key bytes never enter a Go string
// or an ordinary heap buffer. Close erases the mapping before releasing it.
type secret struct{ page []byte }

func newSecret() (*secret, error) {
	page, err := unix.Mmap(-1, 0, unix.Getpagesize(), unix.PROT_READ|unix.PROT_WRITE, unix.MAP_PRIVATE|unix.MAP_ANON)
	if err != nil {
		return nil, errors.New("postgres storage: secret memory unavailable")
	}
	if unix.Mlock(page) != nil || unix.Madvise(page, unix.MADV_DONTDUMP) != nil {
		clear(page)
		_ = unix.Munmap(page)
		return nil, errors.New("postgres storage: cannot protect secret memory")
	}
	return &secret{page: page}, nil
}
func (s *secret) bytes() []byte { return s.page[:32] }
func (s *secret) close() {
	clear(s.page)
	_ = unix.Munlock(s.page)
	_ = unix.Munmap(s.page)
	s.page = nil
}
