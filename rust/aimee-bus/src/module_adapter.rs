//! Bounded adapter from AMOD invocations to an independently owned backend.
//!
//! This is communication glue only: it owns no process, database, storage,
//! request vocabulary, application authorization policy, or retry mechanism.
//! Install [`Adapter::handler`] in the existing module runtime configuration.
use crate::module_protocol::{MODULE_MESSAGE_MAX_BODY, ModuleStatus};
use crate::module_runtime::{Handler, Invocation};
use std::collections::BTreeMap;
use std::fmt;
use std::io::{self, Write};
use std::sync::Arc;

/// A stage's assembled request and response budgets, in bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StageLimits {
    pub stage_id: u32,
    pub request_bytes: usize,
    pub response_bytes: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConfigError {
    NoStages,
    InvalidStage,
    DuplicateStage,
    InvalidLimit,
}
impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::NoStages => "adapter requires at least one stage",
            Self::InvalidStage => "adapter stage id must be nonzero",
            Self::DuplicateStage => "adapter stage id is duplicated",
            Self::InvalidLimit => "adapter limits must be between 1 and the AMOD body maximum",
        })
    }
}
impl std::error::Error for ConfigError {}

/// Application code implements this in its own repository. Invocations carry
/// the authenticated caller, trace, absolute deadline and cancellation signal.
/// Application authorization is the backend's responsibility. Calls can overlap;
/// a backend needing serialization must own that synchronization itself.
///
/// Return a non-OK AMOD status on failure. Partial output is discarded. Never
/// encode sensitive failure details in the status or transport logs. A cancelled
/// or timed-out mutation can already be durable: application retries need their
/// own stable idempotency key; the adapter does not retry backend operations.
pub trait Backend: Send + Sync + 'static {
    fn invoke(
        &self,
        invocation: &Invocation,
        request: &[u8],
        response: &mut Response,
    ) -> Result<(), ModuleStatus>;
}

/// A response writer that cannot grow beyond the configured stage budget.
/// Once a write exceeds it, the entire reply is poisoned, even if the backend
/// ignores the write error and returns success. Writes are all-or-error.
pub struct Response {
    bytes: Vec<u8>,
    limit: usize,
    failed: bool,
}
impl fmt::Debug for Response {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Response")
            .field("len", &self.bytes.len())
            .field("limit", &self.limit)
            .field("failed", &self.failed)
            .finish()
    }
}
impl Response {
    fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
            failed: false,
        }
    }
    pub fn limit(&self) -> usize {
        self.limit
    }
    pub fn remaining(&self) -> usize {
        self.limit - self.bytes.len()
    }
    fn finish(self) -> Result<Vec<u8>, ModuleStatus> {
        if self.failed {
            Err(ModuleStatus::Internal)
        } else {
            Ok(self.bytes)
        }
    }
}
impl Write for Response {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if self.failed || bytes.len() > self.remaining() {
            self.failed = true;
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "adapter response limit exceeded",
            ));
        }
        if self.bytes.try_reserve_exact(bytes.len()).is_err() {
            self.failed = true;
            return Err(io::Error::new(
                io::ErrorKind::OutOfMemory,
                "adapter response allocation failed",
            ));
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        if self.failed {
            Err(io::Error::other("adapter response is invalid"))
        } else {
            Ok(())
        }
    }
}

#[derive(Clone)]
pub struct Adapter {
    backend: Arc<dyn Backend>,
    stages: BTreeMap<u32, StageLimits>,
}
impl fmt::Debug for Adapter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Adapter")
            .field("stages", &self.stages)
            .finish_non_exhaustive()
    }
}
impl Adapter {
    pub fn new(
        backend: Arc<dyn Backend>,
        limits: impl IntoIterator<Item = StageLimits>,
    ) -> Result<Self, ConfigError> {
        let mut stages = BTreeMap::new();
        for stage in limits {
            if stage.stage_id == 0 {
                return Err(ConfigError::InvalidStage);
            }
            if stage.request_bytes == 0
                || stage.response_bytes == 0
                || stage.request_bytes > MODULE_MESSAGE_MAX_BODY as usize
                || stage.response_bytes > MODULE_MESSAGE_MAX_BODY as usize
            {
                return Err(ConfigError::InvalidLimit);
            }
            if stages.insert(stage.stage_id, stage).is_some() {
                return Err(ConfigError::DuplicateStage);
            }
        }
        if stages.is_empty() {
            return Err(ConfigError::NoStages);
        }
        Ok(Self { backend, stages })
    }
    /// The existing runtime still owns authentication, assembly, scheduling,
    /// panic containment, deadlines, cancellation and fragmented replies.
    /// Error replies carry no body, so backend diagnostics cannot leak through
    /// the runtime's logging of non-OK handler bodies.
    pub fn handler(self) -> Handler {
        Arc::new(move |invocation, request| {
            let result = self.dispatch(invocation, request);
            match result {
                Ok(body) => (body, ModuleStatus::Ok),
                Err(status) => (Vec::new(), status),
            }
        })
    }
    fn dispatch(&self, invocation: &Invocation, request: &[u8]) -> Result<Vec<u8>, ModuleStatus> {
        if invocation.cancelled() {
            return Err(ModuleStatus::Cancelled);
        }
        let limits = self
            .stages
            .get(&invocation.stage_id)
            .ok_or(ModuleStatus::InvalidRequest)?;
        if request.len() > limits.request_bytes {
            return Err(ModuleStatus::InvalidRequest);
        }
        let mut response = Response::new(limits.response_bytes);
        match self.backend.invoke(invocation, request, &mut response) {
            Ok(()) => response.finish(),
            Err(ModuleStatus::Ok) => Err(ModuleStatus::Internal),
            Err(status) => Err(status),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn response_budget_is_sticky_and_never_returns_partial_success() {
        let mut response = Response::new(4);
        response.write_all(b"abcd").unwrap();
        assert_eq!(response.remaining(), 0);
        assert!(response.write_all(b"e").is_err());
        assert!(response.write(b"").is_err());
        assert!(response.flush().is_err());
        assert_eq!(response.finish(), Err(ModuleStatus::Internal));
    }
    #[test]
    fn response_accepts_empty_and_exact_budget() {
        let mut response = Response::new(4);
        response.write_all(b"").unwrap();
        response.write_all(b"ab").unwrap();
        response.write_all(b"cd").unwrap();
        assert_eq!(response.finish().unwrap(), b"abcd");
    }
}
