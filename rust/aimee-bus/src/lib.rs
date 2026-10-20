//! Rust reference client for the Aimee shared-memory event bus, and a
//! module-process runtime on top of it.
//!
//! This is the third independent client of the single in-source C host,
//! beside the C client (`src/core/event_bus/bus_client.c`) and the pure-Go
//! client (`server-go/bus`). It links, imports and generates nothing from
//! either: the frame codec is held to the committed wire vectors, and the
//! region layouts follow the C public headers that freeze them.
//!
//! Linux only, as the bus is (`memfd` regions passed with `SCM_RIGHTS`).

#![cfg(target_os = "linux")]

pub mod client;
pub mod error;
mod layout;
pub mod module_caller;
pub mod module_protocol;
pub mod module_runtime;
mod region;
mod ring;
mod sys;
pub mod wire;

pub use client::{Attachment, Client, Event, KeepAlive};
pub use error::{AttachStatus, BusError};
pub use sys::monotonic_ns;
