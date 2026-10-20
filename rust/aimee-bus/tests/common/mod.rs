//! A scripted attach peer for client tests: it builds the three regions in
//! memfds exactly as the C host lays them out, answers one attach with a reply
//! it can corrupt, and then lets a test play the host's side of the rings.
//!
//! It is test scaffolding, not a second host: it admits nothing, routes
//! nothing and owns no lease table. Layout constants are written out here from
//! the C headers rather than imported from the crate, so a crate constant that
//! drifts is caught instead of agreed with.

#![allow(dead_code)]

use aimee_bus::wire::{Frame, HDR_LEN};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

pub const CTL_BYTES: usize = 96;
pub const VIEW_SLOTS: usize = 4096;
pub const CTL_WITH_VIEW: usize = CTL_BYTES + VIEW_SLOTS * 16;
pub const QP_BYTES: usize = 96;
pub const RING_HDR: usize = 192;
pub const ARENA_HDR: usize = 16;

pub struct Region {
    pub fd: OwnedFd,
    pub base: NonNull<u8>,
    pub len: usize,
}

impl Region {
    pub fn new(name: &str, len: usize) -> Region {
        let cname = std::ffi::CString::new(name).unwrap();
        // SAFETY: test scaffolding; every call's result is checked.
        unsafe {
            let fd = libc::memfd_create(cname.as_ptr(), libc::MFD_CLOEXEC);
            assert!(fd >= 0, "memfd_create");
            assert_eq!(libc::ftruncate(fd, len as libc::off_t), 0, "ftruncate");
            let p = libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            );
            assert_ne!(p, libc::MAP_FAILED, "mmap");
            Region {
                fd: OwnedFd::from_raw_fd(fd),
                base: NonNull::new(p as *mut u8).unwrap(),
                len,
            }
        }
    }

    fn at(&self, off: usize) -> *mut u8 {
        assert!(off < self.len);
        // SAFETY: bounds-checked above.
        unsafe { self.base.as_ptr().add(off) }
    }

    pub fn u32(&self, off: usize) -> &AtomicU32 {
        // SAFETY: aligned offsets inside the live mapping.
        unsafe { &*(self.at(off) as *const AtomicU32) }
    }

    pub fn u64(&self, off: usize) -> &AtomicU64 {
        // SAFETY: as u32.
        unsafe { &*(self.at(off) as *const AtomicU64) }
    }

    pub fn write(&self, off: usize, bytes: &[u8]) {
        assert!(off + bytes.len() <= self.len);
        // SAFETY: bounds-checked; test-only writer.
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), self.at(off), bytes.len()) }
    }

    pub fn read(&self, off: usize, len: usize) -> Vec<u8> {
        assert!(off + len <= self.len);
        let mut v = vec![0u8; len];
        // SAFETY: bounds-checked.
        unsafe { std::ptr::copy_nonoverlapping(self.at(off), v.as_mut_ptr(), len) }
        v
    }
}

impl Drop for Region {
    fn drop(&mut self) {
        // SAFETY: unmapping what new() mapped.
        unsafe { libc::munmap(self.base.as_ptr() as *mut libc::c_void, self.len) };
    }
}

fn align64(n: usize) -> usize {
    (n + 63) & !63
}

#[derive(Clone, Copy)]
pub struct Geometry {
    pub slot: u32,
    pub inline: u32,
    pub cap: u32,
    pub arena: u64,
    pub with_view: bool,
}

impl Default for Geometry {
    fn default() -> Self {
        Geometry {
            slot: 256,
            inline: 192,
            cap: 8,
            arena: 64 * 1024,
            with_view: true,
        }
    }
}

/// One ring in a queue-pair region, seen from the host side.
pub struct HostRing<'a> {
    r: &'a Region,
    off: usize,
    slot: usize,
    cap: u64,
}

impl HostRing<'_> {
    fn head(&self) -> &AtomicU64 {
        self.r.u64(self.off + 64)
    }
    fn tail(&self) -> &AtomicU64 {
        self.r.u64(self.off + 128)
    }
    fn slot_off(&self, idx: u64) -> usize {
        self.off + RING_HDR + (idx % self.cap) as usize * self.slot
    }
    /// Produce raw slot bytes; false when full.
    pub fn push_raw(&self, bytes: &[u8]) -> bool {
        let h = self.head().load(Ordering::Relaxed);
        if h - self.tail().load(Ordering::Acquire) >= self.cap {
            return false;
        }
        self.r.write(self.slot_off(h), bytes);
        self.head().store(h + 1, Ordering::Release);
        true
    }
    /// Produce an encoded frame with an inline payload.
    pub fn push(&self, f: Frame, payload: &[u8]) -> bool {
        let mut b = vec![0u8; HDR_LEN + payload.len()];
        f.encode(&mut b).expect("valid test frame");
        b[HDR_LEN..].copy_from_slice(payload);
        self.push_raw(&b)
    }
    /// Consume one frame and its inline payload.
    pub fn pop(&self) -> Option<(Frame, Vec<u8>)> {
        let t = self.tail().load(Ordering::Relaxed);
        if self.head().load(Ordering::Acquire) == t {
            return None;
        }
        let raw = self.r.read(self.slot_off(t), self.slot);
        self.tail().store(t + 1, Ordering::Release);
        let f = Frame::decode(&raw).expect("client wrote a valid frame");
        let p = if f.payload_len > 0 && f.hdr_flags & aimee_bus::wire::F_INLINE != 0 {
            raw[f.payload_ref as usize..f.payload_ref as usize + f.payload_len as usize].to_vec()
        } else {
            Vec::new()
        };
        Some((f, p))
    }
    pub fn count(&self) -> u64 {
        self.head().load(Ordering::Acquire) - self.tail().load(Ordering::Acquire)
    }
}

pub struct FakeHost {
    pub g: Geometry,
    pub control: Region,
    pub arena: Region,
    pub qpair: Region,
    pub in_off: usize,
    pub out_off: usize,
}

impl FakeHost {
    pub fn new(g: Geometry) -> FakeHost {
        let ctl_len = if g.with_view {
            CTL_WITH_VIEW
        } else {
            CTL_BYTES
        };
        let control = Region::new("test-control", ctl_len);
        control.u32(4).store(1, Ordering::Relaxed); // spec_version
        control.u32(8).store(1, Ordering::Relaxed); // layout_version
        control.u32(16).store(g.slot, Ordering::Relaxed);
        control.u32(20).store(g.inline, Ordering::Relaxed);
        control.u32(24).store(g.cap, Ordering::Relaxed);
        control.u64(32).store(g.arena, Ordering::Relaxed);
        control.u64(40).store(1, Ordering::Relaxed); // host_epoch
        if g.with_view {
            control.u32(12).store(1, Ordering::Relaxed); // F_LEASE_VIEW
            control.u32(28).store(VIEW_SLOTS as u32, Ordering::Relaxed);
            control.u64(56).store(CTL_BYTES as u64, Ordering::Relaxed);
        }
        control.u32(0).store(0x4c54_4342, Ordering::Release); // "BCTL"

        let arena = Region::new("test-arena", ARENA_HDR + g.arena as usize);
        arena.u64(8).store(g.arena, Ordering::Relaxed);
        arena.u32(0).store(0x4e52_4142, Ordering::Release); // "BARN"

        let ring = RING_HDR + g.slot as usize * g.cap as usize;
        let in_off = align64(QP_BYTES);
        let out_off = align64(in_off + ring);
        let qpair = Region::new("test-qpair", out_off + ring);
        qpair.u32(4).store(g.slot, Ordering::Relaxed);
        qpair.u32(8).store(g.cap, Ordering::Relaxed);
        qpair.u32(24).store(in_off as u32, Ordering::Relaxed);
        qpair.u32(28).store(out_off as u32, Ordering::Relaxed);
        for off in [in_off, out_off] {
            qpair.u32(off + 4).store(g.slot, Ordering::Relaxed);
            qpair.u32(off + 8).store(g.cap, Ordering::Relaxed);
            qpair.u32(off + 12).store(g.cap - 1, Ordering::Relaxed);
            qpair
                .u32(off + 16)
                .store(RING_HDR as u32, Ordering::Relaxed);
            qpair.u32(off).store(0x474e_4952, Ordering::Release); // "RING"
        }
        qpair.u32(0).store(0x5250_4251, Ordering::Release); // "QPBR"
        FakeHost {
            g,
            control,
            arena,
            qpair,
            in_off,
            out_off,
        }
    }

    /// The client's inbound ring (host produces).
    pub fn inbound(&self) -> HostRing<'_> {
        HostRing {
            r: &self.qpair,
            off: self.in_off,
            slot: self.g.slot as usize,
            cap: u64::from(self.g.cap),
        }
    }

    /// The client's outbound ring (host consumes).
    pub fn outbound(&self) -> HostRing<'_> {
        HostRing {
            r: &self.qpair,
            off: self.out_off,
            slot: self.g.slot as usize,
            cap: u64::from(self.g.cap),
        }
    }

    /// The attach reply the host would send.
    pub fn reply(&self, status: u32, handle: u32) -> [u8; 48] {
        let mut r = [0u8; 48];
        r[0..4].copy_from_slice(&0x5052_4241u32.to_le_bytes()); // "ABRP"
        r[4..8].copy_from_slice(&status.to_le_bytes());
        r[8..12].copy_from_slice(&handle.to_le_bytes());
        r[12..16].copy_from_slice(&3u32.to_le_bytes());
        r[16..20].copy_from_slice(&self.g.slot.to_le_bytes());
        r[20..24].copy_from_slice(&self.g.inline.to_le_bytes());
        r[24..28].copy_from_slice(&self.g.cap.to_le_bytes());
        r[32..40].copy_from_slice(&self.g.arena.to_le_bytes());
        r[40..48].copy_from_slice(&1u64.to_le_bytes());
        r
    }

    pub fn fds(&self) -> [RawFd; 3] {
        [
            self.control.fd.as_raw_fd(),
            self.arena.fd.as_raw_fd(),
            self.qpair.fd.as_raw_fd(),
        ]
    }

    /// Set a lease-view entry as the host does: placement, then generation.
    pub fn set_lease(&self, lease: u32, generation: u32, len: u32, offset: u64) {
        let e = CTL_BYTES + lease as usize * 16;
        self.control.u32(e + 4).store(len, Ordering::Relaxed);
        self.control.u64(e + 8).store(offset, Ordering::Relaxed);
        self.control.u32(e).store(generation, Ordering::Release);
    }

    pub fn write_arena(&self, offset: usize, bytes: &[u8]) {
        self.arena.write(ARENA_HDR + offset, bytes);
    }

    pub fn bump_epoch(&self) {
        self.control.u64(40).fetch_add(1, Ordering::AcqRel);
    }

    pub fn client_heartbeat(&self) -> u64 {
        self.qpair.u64(32).load(Ordering::Acquire)
    }

    pub fn set_control_lost(&self) {
        self.qpair.u32(20).store(1, Ordering::Release);
    }
}

/// A connected SOCK_SEQPACKET pair.
pub fn socketpair() -> (OwnedFd, OwnedFd) {
    let mut sv = [0 as RawFd; 2];
    // SAFETY: sv is a valid out-array.
    let rc = unsafe {
        libc::socketpair(
            libc::AF_UNIX,
            libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC,
            0,
            sv.as_mut_ptr(),
        )
    };
    assert_eq!(rc, 0, "socketpair");
    // SAFETY: fresh descriptors we own.
    unsafe { (OwnedFd::from_raw_fd(sv[0]), OwnedFd::from_raw_fd(sv[1])) }
}

/// Read the client's attach request; returns (min, max, class, ref).
pub fn read_request(sock: &OwnedFd) -> (u16, u16, u32, u32) {
    let mut b = [0u8; 16];
    // SAFETY: b is a valid buffer.
    let n = unsafe {
        libc::recv(
            sock.as_raw_fd(),
            b.as_mut_ptr() as *mut libc::c_void,
            b.len(),
            0,
        )
    };
    assert_eq!(n, 16, "attach request size");
    assert_eq!(
        u32::from_le_bytes(b[0..4].try_into().unwrap()),
        0x5152_4241,
        "ABRQ"
    );
    (
        u16::from_le_bytes([b[4], b[5]]),
        u16::from_le_bytes([b[6], b[7]]),
        u32::from_le_bytes(b[8..12].try_into().unwrap()),
        u32::from_le_bytes(b[12..16].try_into().unwrap()),
    )
}

/// Send a reply carrying `fds` with SCM_RIGHTS.
pub fn send_reply(sock: &OwnedFd, reply: &[u8], fds: &[RawFd]) {
    let mut iov = libc::iovec {
        iov_base: reply.as_ptr() as *mut libc::c_void,
        iov_len: reply.len(),
    };
    // SAFETY: building a msghdr over live buffers; CMSG macros bound the writes.
    unsafe {
        let space = libc::CMSG_SPACE((fds.len() * 4) as u32) as usize;
        let mut cbuf = vec![0u64; space.div_ceil(8).max(1)];
        let mut msg: libc::msghdr = std::mem::zeroed();
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;
        if !fds.is_empty() {
            msg.msg_control = cbuf.as_mut_ptr() as *mut libc::c_void;
            msg.msg_controllen = space as _;
            let c = libc::CMSG_FIRSTHDR(&msg);
            (*c).cmsg_level = libc::SOL_SOCKET;
            (*c).cmsg_type = libc::SCM_RIGHTS;
            (*c).cmsg_len = libc::CMSG_LEN((fds.len() * 4) as u32) as _;
            let data = libc::CMSG_DATA(c) as *mut RawFd;
            for (i, fd) in fds.iter().enumerate() {
                data.add(i).write_unaligned(*fd);
            }
        }
        let n = libc::sendmsg(sock.as_raw_fd(), &msg, 0);
        assert_eq!(n as usize, reply.len(), "sendmsg");
    }
}

/// Attach a client to `host` with the host's honest reply.
pub fn attach(host: &FakeHost) -> aimee_bus::Client {
    use std::os::fd::AsFd;
    let (client_sock, host_sock) = socketpair();
    let fds = host.fds();
    let reply = host.reply(0, 3);
    let t = std::thread::spawn(move || {
        read_request(&host_sock);
        send_reply(&host_sock, &reply, &fds);
        host_sock
    });
    let c = aimee_bus::Client::attach(client_sock.as_fd()).expect("attach to the fake host");
    drop(t.join().unwrap());
    c
}
