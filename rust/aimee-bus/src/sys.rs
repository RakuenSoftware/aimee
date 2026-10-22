//! The few system calls the client needs: a `SOCK_SEQPACKET` connect,
//! `SCM_RIGHTS` receipt, shared mappings and the monotonic clock.

use std::io;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::ptr::NonNull;

/// Connect a close-on-exec `SOCK_SEQPACKET` socket to `path`.
pub(crate) fn connect_seqpacket(path: &Path) -> io::Result<OwnedFd> {
    let bytes = path.as_os_str().as_bytes();
    // SAFETY: sockaddr_un is plain old data; zeroed is a valid value.
    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    if bytes.is_empty() || bytes.len() >= addr.sun_path.len() || bytes.contains(&0) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "socket path empty or too long",
        ));
    }
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (dst, src) in addr.sun_path.iter_mut().zip(bytes) {
        *dst = *src as libc::c_char;
    }
    // SAFETY: plain socket(2).
    let raw = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) };
    if raw < 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: raw is a fresh descriptor we own.
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let len = (std::mem::size_of::<libc::sa_family_t>() + bytes.len() + 1) as libc::socklen_t;
    // Not retried on EINTR: a second connect on the same socket can report
    // EISCONN or EALREADY for an attempt that is still completing. The
    // caller's retry loop opens a fresh socket instead.
    // SAFETY: addr is initialised and len covers the family and path.
    let rc = unsafe {
        libc::connect(
            fd.as_raw_fd(),
            &addr as *const _ as *const libc::sockaddr,
            len,
        )
    };
    if rc == 0 {
        Ok(fd)
    } else {
        Err(io::Error::last_os_error())
    }
}

/// Bound blocking receives on `fd`.
pub(crate) fn set_recv_timeout(fd: BorrowedFd<'_>, timeout: std::time::Duration) -> io::Result<()> {
    let tv = libc::timeval {
        tv_sec: timeout.as_secs() as libc::time_t,
        tv_usec: timeout.subsec_micros() as libc::suseconds_t,
    };
    // SAFETY: tv is a valid timeval for SO_RCVTIMEO.
    let rc = unsafe {
        libc::setsockopt(
            fd.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_RCVTIMEO,
            &tv as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::timeval>() as libc::socklen_t,
        )
    };
    if rc == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// Send one datagram.
pub(crate) fn send_packet(fd: BorrowedFd<'_>, buf: &[u8]) -> io::Result<()> {
    loop {
        // SAFETY: buf is valid for its length.
        let n = unsafe {
            libc::send(
                fd.as_raw_fd(),
                buf.as_ptr() as *const libc::c_void,
                buf.len(),
                libc::MSG_NOSIGNAL,
            )
        };
        if n >= 0 {
            if n as usize != buf.len() {
                return Err(io::Error::new(io::ErrorKind::WriteZero, "short send"));
            }
            return Ok(());
        }
        let err = io::Error::last_os_error();
        if err.kind() != io::ErrorKind::Interrupted {
            return Err(err);
        }
    }
}

/// Receive one datagram and every descriptor attached to it. Descriptors are
/// owned on return, so an early error closes them.
pub(crate) fn recv_with_fds(
    fd: BorrowedFd<'_>,
    buf: &mut [u8],
    max_fds: usize,
) -> io::Result<(usize, Vec<OwnedFd>, bool)> {
    // Room for one more descriptor than expected, so an over-delivery is
    // seen rather than silently truncated.
    let space = (max_fds + 1) * std::mem::size_of::<RawFd>();
    // SAFETY: CMSG_SPACE is a pure size computation.
    let cap = unsafe { libc::CMSG_SPACE(space as u32) } as usize;
    let mut cbuf = vec![0u64; cap.div_ceil(8)];
    let mut iov = libc::iovec {
        iov_base: buf.as_mut_ptr() as *mut libc::c_void,
        iov_len: buf.len(),
    };
    // SAFETY: msghdr is plain old data; zeroed is a valid value.
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf.as_mut_ptr() as *mut libc::c_void;
    msg.msg_controllen = cap as _;
    let n = loop {
        // SAFETY: msg points at live buffers sized above.
        let n = unsafe { libc::recvmsg(fd.as_raw_fd(), &mut msg, libc::MSG_CMSG_CLOEXEC) };
        if n >= 0 {
            break n as usize;
        }
        let err = io::Error::last_os_error();
        if err.kind() != io::ErrorKind::Interrupted {
            return Err(err);
        }
    };
    let mut fds = Vec::new();
    // SAFETY: walking the control buffer the kernel just filled, with the
    // libc CMSG helpers that bound every step by msg_controllen.
    unsafe {
        let mut c = libc::CMSG_FIRSTHDR(&msg);
        while !c.is_null() {
            if (*c).cmsg_level == libc::SOL_SOCKET && (*c).cmsg_type == libc::SCM_RIGHTS {
                let data = libc::CMSG_DATA(c) as *const RawFd;
                let count = ((*c).cmsg_len as usize).saturating_sub(libc::CMSG_LEN(0) as usize)
                    / std::mem::size_of::<RawFd>();
                for i in 0..count {
                    fds.push(OwnedFd::from_raw_fd(std::ptr::read_unaligned(data.add(i))));
                }
            }
            c = libc::CMSG_NXTHDR(&msg, c);
        }
    }
    let truncated = msg.msg_flags & (libc::MSG_CTRUNC | libc::MSG_TRUNC) != 0;
    Ok((n, fds, truncated))
}

/// The size of the object behind `fd`.
pub(crate) fn fd_size(fd: BorrowedFd<'_>) -> io::Result<u64> {
    // SAFETY: stat is plain old data.
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    // SAFETY: st is a valid out-pointer for a descriptor we hold.
    if unsafe { libc::fstat(fd.as_raw_fd(), &mut st) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(st.st_size.max(0) as u64)
}

/// A `MAP_SHARED` mapping, unmapped on drop.
#[derive(Debug)]
pub(crate) struct Mapping {
    ptr: NonNull<u8>,
    len: usize,
}

// SAFETY: the mapping is process-wide memory; all concurrent access to its
// contents goes through atomics or is serialised by the owning Client.
unsafe impl Send for Mapping {}
// SAFETY: see Send.
unsafe impl Sync for Mapping {}

impl Mapping {
    pub(crate) fn map(fd: BorrowedFd<'_>, len: usize, writable: bool) -> io::Result<Mapping> {
        if len == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "empty mapping"));
        }
        let prot = if writable {
            libc::PROT_READ | libc::PROT_WRITE
        } else {
            libc::PROT_READ
        };
        // A region shorter than the geometry the host announced would fault
        // on first touch; refuse it here instead.
        // SAFETY: fstat on a descriptor we hold.
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        // SAFETY: st is a valid out-pointer.
        if unsafe { libc::fstat(fd.as_raw_fd(), &mut st) } != 0 {
            return Err(io::Error::last_os_error());
        }
        if (st.st_size as u64) < len as u64 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "region smaller than its geometry",
            ));
        }
        // SAFETY: mapping a descriptor we hold; the result is checked.
        let p = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                prot,
                libc::MAP_SHARED,
                fd.as_raw_fd(),
                0,
            )
        };
        if p == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        Ok(Mapping {
            ptr: NonNull::new(p as *mut u8).expect("mmap returned null"),
            len,
        })
    }

    pub(crate) fn ptr(&self) -> NonNull<u8> {
        self.ptr
    }

    pub(crate) fn len(&self) -> usize {
        self.len
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        // SAFETY: unmapping exactly what map() mapped.
        unsafe { libc::munmap(self.ptr.as_ptr() as *mut libc::c_void, self.len) };
    }
}

/// `CLOCK_MONOTONIC` in nanoseconds, or zero if the clock is unreadable. The
/// host's stale-client checks and AMOD deadlines use the same clock.
pub fn monotonic_ns() -> u64 {
    // SAFETY: timespec is plain old data.
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    // SAFETY: ts is a valid out-pointer.
    if unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) } != 0 {
        return 0;
    }
    (ts.tv_sec as u64)
        .saturating_mul(1_000_000_000)
        .saturating_add(ts.tv_nsec as u64)
}
