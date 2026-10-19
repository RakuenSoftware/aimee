//! An attached bus client: the attach handshake, the three mapped regions,
//! and publish / request / reply / cancel / poll over the queue pair.
//!
//! After attach the socket is no longer the data path; all traffic uses the
//! shared mappings. The client never creates regions, listens or accepts:
//! there is exactly one host, and it is the C host.

use crate::error::{AttachStatus, BusError};
use crate::region::{ARENA_HDR, CTL_BYTES, Control, QueuePair, qpair_bytes};
use crate::sys::{self, Mapping};
use crate::wire::*;
use std::os::fd::{AsFd, BorrowedFd};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

const ATTACH_REQ_MAGIC: u32 = 0x5152_4241; // "ABRQ"
const ATTACH_REPLY_MAGIC: u32 = 0x5052_4241; // "ABRP"
const ATTACH_REQ_BYTES: usize = 16;
const ATTACH_REPLY_BYTES: usize = 48;
const CONNECT_RETRY: Duration = Duration::from_millis(100);
/// Bound on the attach reply when [`Client::connect`] owns the socket.
pub const ATTACH_TIMEOUT: Duration = Duration::from_secs(30);

/// Geometry and identity the host granted at attach.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Attachment {
    pub handle: u32,
    pub wire_version: u32,
    pub slot_size: u32,
    pub inline_budget: u32,
    pub queue_capacity: u32,
    pub arena_size: u64,
    pub host_epoch: u64,
}

/// One received event. The payload is copied out of the shared slot, so it
/// stays valid after the slot is released. An arena-placed payload is not
/// readable by a client without a lease and arrives empty; the frame still
/// says what it was.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Event {
    pub frame: Frame,
    pub payload: Vec<u8>,
}

/// An attached client. It is `Send + Sync`: producers are serialised on the
/// single-producer outbound ring and pollers on the single-consumer inbound
/// ring, so callers may share it across threads.
#[derive(Debug)]
pub struct Client {
    att: Attachment,
    // Field order is drop order: views before the mappings they point into.
    qp: QueuePair,
    control: Control,
    _qp_map: Mapping,
    _arena_map: Mapping,
    _ctl_map: Mapping,
    emit_lock: Mutex<()>,
    poll_lock: Mutex<()>,
    dropped: AtomicU64,
}

impl Client {
    /// Attach over a connected `SOCK_SEQPACKET` socket as the anonymous
    /// principal. The socket is not closed here.
    pub fn attach(sock: BorrowedFd<'_>) -> Result<Client, BusError> {
        Self::attach_as(sock, 0, 0)
    }

    /// Perform an authenticated module attach. The host treats class and ref
    /// as claims only: its admission policy also binds them to the peer's
    /// credentials and canonical executable path.
    pub fn attach_as(
        sock: BorrowedFd<'_>,
        principal_class: u32,
        principal_ref: u32,
    ) -> Result<Client, BusError> {
        let mut req = [0u8; ATTACH_REQ_BYTES];
        req[0..4].copy_from_slice(&ATTACH_REQ_MAGIC.to_le_bytes());
        req[4..6].copy_from_slice(&WIRE_VERSION.to_le_bytes()); // min
        req[6..8].copy_from_slice(&WIRE_VERSION.to_le_bytes()); // max
        req[8..12].copy_from_slice(&principal_class.to_le_bytes());
        req[12..16].copy_from_slice(&principal_ref.to_le_bytes());
        sys::send_packet(sock, &req)?;

        let mut reply = [0u8; ATTACH_REPLY_BYTES + 1];
        let (n, fds, truncated) = sys::recv_with_fds(sock, &mut reply, 3)?;
        let r32 =
            |o: usize| u32::from_le_bytes([reply[o], reply[o + 1], reply[o + 2], reply[o + 3]]);
        let r64 = |o: usize| u64::from(r32(o)) | (u64::from(r32(o + 4)) << 32);
        if truncated || n != ATTACH_REPLY_BYTES || r32(0) != ATTACH_REPLY_MAGIC {
            return Err(BusError::Protocol("malformed attach reply"));
        }
        let status = AttachStatus::from_wire(r32(4));
        if status != AttachStatus::Ok {
            return Err(BusError::Denied(status));
        }
        if fds.len() != 3 {
            return Err(BusError::Protocol(
                "attach must carry exactly three descriptors",
            ));
        }
        let att = Attachment {
            handle: r32(8),
            wire_version: r32(12),
            slot_size: r32(16),
            inline_budget: r32(20),
            queue_capacity: r32(24),
            arena_size: r64(32),
            host_epoch: r64(40),
        };
        if att.wire_version != u32::from(WIRE_VERSION) {
            return Err(BusError::Protocol(
                "host negotiated an unsupported wire version",
            ));
        }
        if att.slot_size as usize <= HDR_LEN || att.inline_budget > att.slot_size {
            return Err(BusError::Protocol("attach geometry cannot carry a frame"));
        }
        let arena_len = (att.arena_size as usize)
            .checked_add(ARENA_HDR)
            .ok_or(BusError::Protocol("arena size overflows"))?;

        // fds[0] control (read-only), fds[1] arena (rw), fds[2] queue pair (rw).
        let ctl_map = Mapping::map(fds[0].as_fd(), CTL_BYTES, false)?;
        let arena_map = Mapping::map(fds[1].as_fd(), arena_len, true)?;
        let qp_map = Mapping::map(
            fds[2].as_fd(),
            qpair_bytes(att.slot_size, att.queue_capacity),
            true,
        )?;
        drop(fds); // the mappings hold their own references

        // SAFETY: each view points into a mapping stored alongside it in the
        // Client and dropped after it (field order).
        let control = unsafe { Control::attach(ctl_map.ptr(), ctl_map.len())? };
        let g = control.geometry();
        if g.slot_size != att.slot_size
            || g.inline_budget != att.inline_budget
            || g.queue_capacity != att.queue_capacity
            || g.arena_size != att.arena_size
        {
            return Err(BusError::Region(
                "control region disagrees with the attach reply",
            ));
        }
        // SAFETY: as above.
        let qp = unsafe { QueuePair::attach(qp_map.ptr(), qp_map.len())? };
        if qp.inbound.slot_size() != att.slot_size as usize
            || qp.inbound.capacity() != u64::from(att.queue_capacity)
        {
            return Err(BusError::Region(
                "queue pair disagrees with the attach reply",
            ));
        }
        Ok(Client {
            att,
            qp,
            control,
            _qp_map: qp_map,
            _arena_map: arena_map,
            _ctl_map: ctl_map,
            emit_lock: Mutex::new(()),
            poll_lock: Mutex::new(()),
            dropped: AtomicU64::new(0),
        })
    }

    /// Connect to a daemon's bus socket and attach. A refused connection is
    /// retried until `stop` is set, because a restarting daemon can leave its
    /// old pathname in place until the new host replaces it. A missing socket
    /// or a policy refusal fails immediately: those are configuration
    /// mistakes and waiting only hides them.
    ///
    /// The attach handshake is bounded by [`ATTACH_TIMEOUT`], so a host that
    /// accepts but never answers cannot hang the caller. The returned client
    /// does not heartbeat on its own; a requester that idles between calls
    /// should hold [`Client::keep_alive`], or the host will reap it.
    pub fn connect(
        path: &Path,
        principal_class: u32,
        principal_ref: u32,
        stop: &AtomicBool,
    ) -> Result<Option<Client>, BusError> {
        loop {
            match sys::connect_seqpacket(path) {
                Ok(fd) => {
                    sys::set_recv_timeout(fd.as_fd(), ATTACH_TIMEOUT)?;
                    return Self::attach_as(fd.as_fd(), principal_class, principal_ref).map(Some);
                }
                Err(e) if e.raw_os_error() == Some(libc::ECONNREFUSED) => {}
                Err(e) => return Err(e.into()),
            }
            if stop.load(Ordering::Acquire) {
                return Ok(None);
            }
            std::thread::sleep(CONNECT_RETRY);
        }
    }

    /// What the host granted.
    pub fn attachment(&self) -> Attachment {
        self.att
    }

    /// The handle the host assigned to this client.
    pub fn handle(&self) -> u32 {
        self.att.handle
    }

    /// The inline budget the host announced.
    pub fn inline_budget(&self) -> u32 {
        self.att.inline_budget
    }

    /// Largest payload one inline event can actually carry: the inline
    /// budget, bounded by what fits in a slot after the frame header. A host
    /// may announce a budget as large as the slot itself.
    pub fn max_inline_payload(&self) -> u32 {
        self.att
            .inline_budget
            .min(self.att.slot_size - HDR_LEN as u32)
    }

    /// Whether the host restarted since attach.
    pub fn epoch_changed(&self) -> bool {
        self.control.epoch() != self.att.host_epoch
    }

    /// Whether the host shed a control-class event to this client (sticky).
    pub fn control_lost(&self) -> bool {
        self.qp.control_lost()
    }

    /// Write a liveness value the host reads; a slot whose heartbeat stops
    /// advancing is reaped.
    pub fn heartbeat(&self, now: u64) {
        self.qp.set_heartbeat(now);
    }

    /// Heartbeat with the monotonic clock the host's stale checks use.
    pub fn heartbeat_now(&self) {
        let now = sys::monotonic_ns();
        if now != 0 {
            self.heartbeat(now);
        }
    }

    /// Frames refused on receipt since attach (malformed or out of bounds).
    pub fn dropped_frames(&self) -> u64 {
        self.dropped.load(Ordering::Relaxed)
    }

    fn emit(
        &self,
        flags: u16,
        kind: u32,
        correlation: u64,
        payload: &[u8],
    ) -> Result<(), BusError> {
        if payload.len() > self.att.inline_budget as usize
            || HDR_LEN + payload.len() > self.att.slot_size as usize
        {
            return Err(BusError::Payload);
        }
        let mut frame = Frame {
            hdr_flags: flags,
            wire_version: WIRE_VERSION,
            event_kind: kind,
            correlation_id: correlation,
            src_handle: self.att.handle,
            ..Frame::default()
        };
        if !payload.is_empty() {
            frame.hdr_flags |= F_INLINE;
            frame.payload_len = payload.len() as u32;
            frame.payload_ref = HDR_LEN as u64;
        }
        frame
            .validate()
            .map_err(|_| BusError::Protocol("frame refused by its own encoder"))?;
        // The outbound ring has exactly one producer. Two concurrent emitters
        // would claim the same slot and advance head twice: one message lost,
        // another delivered twice.
        let _guard = self.emit_lock.lock().unwrap_or_else(|p| p.into_inner());
        if self.epoch_changed() {
            return Err(BusError::Epoch);
        }
        let mut header = [0u8; HDR_LEN];
        frame.encode(&mut header).expect("validated frame encodes");
        let written = self.qp.outbound.produce(&[&header, payload]);
        if written {
            Ok(())
        } else {
            Err(BusError::WouldBlock)
        }
    }

    /// Send a one-way notification.
    pub fn publish(&self, kind: u32, payload: &[u8]) -> Result<(), BusError> {
        self.emit(F_NOTIFICATION, kind, 0, payload)
    }

    /// Send a correlated request; the reply arrives via [`Client::poll`].
    pub fn request(&self, kind: u32, correlation: u64, payload: &[u8]) -> Result<(), BusError> {
        self.request_fragment(kind, correlation, payload, false)
    }

    /// Send one ordered request fragment; `more` keeps the correlation open.
    pub fn request_fragment(
        &self,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError> {
        self.correlated(F_REQUEST, kind, correlation, payload, more)
    }

    /// Answer a request (a serving module).
    pub fn reply(&self, kind: u32, correlation: u64, payload: &[u8]) -> Result<(), BusError> {
        self.reply_fragment(kind, correlation, payload, false)
    }

    /// Send one ordered reply fragment; `more` keeps the correlation pending.
    pub fn reply_fragment(
        &self,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError> {
        self.correlated(F_REPLY, kind, correlation, payload, more)
    }

    fn correlated(
        &self,
        pattern: u16,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError> {
        if correlation == 0 || (more && payload.is_empty()) {
            return Err(BusError::Protocol(
                "correlated frame needs a correlation and, if fragmented, a payload",
            ));
        }
        self.emit(
            pattern | if more { F_MORE } else { 0 },
            kind,
            correlation,
            payload,
        )
    }

    /// Cancel an outstanding request. Delivery is best-effort and idempotent.
    pub fn cancel(&self, kind: u32, correlation: u64) -> Result<(), BusError> {
        if correlation == 0 {
            return Err(BusError::Protocol("cancel needs a correlation"));
        }
        self.emit(F_CANCEL, kind, correlation, &[])
    }

    /// Take the next inbound event, or `None` when the ring is empty. A frame
    /// that fails decoding or placement checks is released, counted, and
    /// skipped rather than handed to the caller.
    pub fn poll(&self) -> Result<Option<Event>, BusError> {
        let _guard = self.poll_lock.lock().unwrap_or_else(|p| p.into_inner());
        if self.epoch_changed() {
            return Err(BusError::Epoch);
        }
        let mut slot = vec![0u8; self.att.slot_size as usize];
        loop {
            if !self.qp.inbound.consume(&mut slot) {
                return Ok(None);
            }
            let Some(frame) = Frame::decode(&slot).ok().filter(|f| {
                f.check_placement(self.att.slot_size, self.att.arena_size)
                    .is_ok()
            }) else {
                self.dropped.fetch_add(1, Ordering::Relaxed);
                continue;
            };
            let mut payload = Vec::new();
            if frame.hdr_flags & F_INLINE != 0 && frame.payload_len > 0 {
                let start = frame.payload_ref as usize;
                payload.extend_from_slice(&slot[start..start + frame.payload_len as usize]);
            }
            return Ok(Some(Event { frame, payload }));
        }
    }

    /// Advance this client's heartbeat on a background thread until the
    /// returned guard is dropped. A requesting client is idle between calls
    /// and would otherwise be reaped shortly after attaching.
    pub fn keep_alive(self: &Arc<Self>, interval: Duration) -> KeepAlive {
        let stop = Arc::new(AtomicBool::new(false));
        let (client, flag) = (Arc::clone(self), Arc::clone(&stop));
        let thread = std::thread::Builder::new()
            .name("aimee-bus-keepalive".into())
            .spawn(move || {
                while !flag.load(Ordering::Acquire) {
                    client.heartbeat_now();
                    std::thread::park_timeout(interval);
                }
            })
            .expect("spawn keep-alive thread");
        KeepAlive {
            stop,
            thread: Some(thread),
        }
    }
}

/// Stops the keep-alive thread when dropped.
#[derive(Debug)]
pub struct KeepAlive {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Drop for KeepAlive {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(t) = self.thread.take() {
            t.thread().unpark();
            let _ = t.join();
        }
    }
}
