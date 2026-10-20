//! The Rust codec against the committed, implementation-independent wire
//! vectors. Every positive row must decode to its listed fields and
//! re-encode to the exact bytes; every negative row must be refused with its
//! listed result name.

use aimee_bus::wire::{Frame, HDR_LEN};
use std::path::PathBuf;

fn vectors_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../src/tests/fixtures/bus/wire_vectors.tsv")
}

fn unhex(s: &str) -> Vec<u8> {
    assert!(s.len() % 2 == 0, "odd hex length");
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex"))
        .collect()
}

fn number(v: &str) -> u64 {
    match v.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16).expect("hex field"),
        None => v.parse().expect("decimal field"),
    }
}

fn expected_frame(name: &str, fields: &str) -> Frame {
    let mut f = Frame::default();
    for kv in fields.split(';') {
        let (k, v) = kv
            .split_once('=')
            .unwrap_or_else(|| panic!("{name}: field {kv}"));
        let n = number(v);
        match k {
            "flags" => f.hdr_flags = n as u16,
            "ver" => f.wire_version = n as u16,
            "kind" => f.event_kind = n as u32,
            "principal" => f.principal_ref = n as u32,
            "corr" => f.correlation_id = n,
            "seq" => f.seq = n,
            "lts" => f.logical_ts = n,
            "pref" => f.payload_ref = n,
            "plen" => f.payload_len = n as u32,
            "src" => f.src_handle = n as u32,
            "dst" => f.dst_handle = n as u32,
            "gen" => f.generation = n as u32,
            other => panic!("{name}: unknown field {other}; the vector format changed"),
        }
    }
    f
}

#[test]
fn committed_wire_vectors() {
    let text = std::fs::read_to_string(vectors_path()).expect("read wire_vectors.tsv");
    let (mut positive, mut negative) = (0, 0);
    for line in text.lines() {
        if line.starts_with('#') || line.trim().is_empty() {
            continue;
        }
        let cols: Vec<&str> = line.split('\t').collect();
        assert_eq!(cols.len(), 4, "malformed vector row: {line}");
        let (name, expect, hex, fields) = (cols[0], cols[1], cols[2], cols[3]);
        let bytes = unhex(hex);
        assert_eq!(bytes.len(), HDR_LEN, "{name}: vector is not one header");
        let decoded = Frame::decode(&bytes);
        if expect == "OK" {
            let got = decoded.unwrap_or_else(|e| panic!("{name}: refused with {e}"));
            let want = expected_frame(name, fields);
            assert_eq!(got, want, "{name}: decoded fields");
            let mut out = [0xa5u8; HDR_LEN];
            want.encode(&mut out)
                .unwrap_or_else(|e| panic!("{name}: encode {e}"));
            assert_eq!(out.to_vec(), bytes, "{name}: encoded bytes");
            positive += 1;
        } else {
            let err = decoded.expect_err(name);
            assert_eq!(err.name(), expect, "{name}: refusal");
            negative += 1;
        }
    }
    assert!(positive > 0 && negative > 0, "no vectors were checked");
    println!("wire vectors: {positive} positive, {negative} negative");
}
