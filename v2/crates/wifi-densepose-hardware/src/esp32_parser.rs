//! ESP32 CSI frame parser (ADR-018 binary format).
//!
//! Parses binary CSI data as produced by ADR-018 compliant firmware,
//! typically streamed over UDP from ESP32/ESP32-S3 nodes.
//!
//! # ADR-018 Binary Frame Format
//!
//! ```text
//! Offset  Size  Field
//! ------  ----  -----
//! 0       4     Magic: 0xC5110001
//! 4       1     Node ID
//! 5       1     Number of antennas
//! 6       2     Number of subcarriers (LE u16)
//! 8       4     Frequency MHz (LE u32)
//! 12      4     Sequence number (LE u32)
//! 16      1     RSSI (i8)
//! 17      1     Noise floor (i8)
//! 18      1     PPDU type (ADR-110: 0=HT/legacy, 1=HE-SU, 2=HE-MU, 3=HE-TB)
//! 19      1     Flags (ADR-110: bit0 bw40, bit2 STBC, bit3 LDPC, bit4 15.4-sync)
//! 20      N*2   I/Q pairs (n_antennas * n_subcarriers * 2 bytes)
//! ```
//!
//! Each I/Q pair is 2 signed bytes: I then Q.
//!
//! # No-Mock Guarantee
//!
//! This parser either successfully parses real bytes or returns a specific
//! `ParseError`. It never generates synthetic data.

use byteorder::{LittleEndian, ReadBytesExt};
use chrono::Utc;
use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::io::Cursor;

use crate::csi_frame::{
    Adr018Flags, AntennaConfig, Bandwidth, CsiFrame, CsiMetadata, PpduType, SubcarrierData,
};
use crate::error::ParseError;

/// ESP32 CSI binary frame magic number (ADR-018).
pub const ESP32_CSI_MAGIC: u32 = 0xC5110001;

/// ESP32 CSI authenticated binary frame magic number (ADR-032).
pub const ESP32_CSI_AUTHENTICATED_MAGIC: u32 = 0xC5110008;

// ── Sibling EchoPose wire packets ──────────────────────────────────────────────
// The ESP32 firmware multiplexes several packet types onto the same UDP port
// as ADR-018 raw CSI frames. A CSI-only consumer will therefore see these
// interleaved with CSI frames. They are *not* corruption — they just need a
// different decoder (or can be skipped). See firmware `rv_feature_state.h`.

/// ADR-039 edge vitals packet (32 bytes: HR/BR/presence).
pub const ECHOPOSE_VITALS_MAGIC: u32 = 0xC5110002;
/// ADR-069 feature-vector packet.
pub const ECHOPOSE_FEATURE_MAGIC: u32 = 0xC5110003;
/// ADR-063 fused-vitals packet (multi-sensor fusion).
pub const ECHOPOSE_FUSED_VITALS_MAGIC: u32 = 0xC5110004;
/// ADR-039 compressed-CSI packet.
pub const ECHOPOSE_COMPRESSED_CSI_MAGIC: u32 = 0xC5110005;
/// ADR-081 compact feature-state packet (the default upstream payload).
pub const ECHOPOSE_FEATURE_STATE_MAGIC: u32 = 0xC5110006;
/// ADR-095 / #513 on-device temporal-classification packet.
pub const ECHOPOSE_TEMPORAL_MAGIC: u32 = 0xC5110007;

/// If `magic` is a recognized EchoPose wire packet other than the ADR-018 raw
/// CSI frame, return a human-readable name for it; otherwise `None`.
///
/// Used by CSI consumers to distinguish "a sibling packet I should route or
/// skip" from "genuine garbage on the wire".
pub fn echopose_sibling_packet_name(magic: u32) -> Option<&'static str> {
    match magic {
        ECHOPOSE_VITALS_MAGIC => Some("ADR-039 edge vitals"),
        ECHOPOSE_FEATURE_MAGIC => Some("ADR-069 feature vector"),
        ECHOPOSE_FUSED_VITALS_MAGIC => Some("ADR-063 fused vitals"),
        ECHOPOSE_COMPRESSED_CSI_MAGIC => Some("ADR-039 compressed CSI"),
        ECHOPOSE_FEATURE_STATE_MAGIC => Some("ADR-081 feature state"),
        ECHOPOSE_TEMPORAL_MAGIC => Some("ADR-095 temporal classification"),
        _ => None,
    }
}

/// ADR-018 header size in bytes (before I/Q data).
const HEADER_SIZE: usize = 20;

/// ADR-032 authenticated header size in bytes (before I/Q data).
const AUTHENTICATED_HEADER_SIZE: usize = 28;

/// Maximum valid subcarrier count for ESP32 (80 MHz bandwidth).
const MAX_SUBCARRIERS: usize = 256;

/// Maximum antenna count for ESP32.
const MAX_ANTENNAS: u8 = 4;

// ── ADR-032 Security Helpers ──────────────────────────────────────────────────

fn u8to64_le(p: &[u8]) -> u64 {
    (p[0] as u64) |
    ((p[1] as u64) << 8) |
    ((p[2] as u64) << 16) |
    ((p[3] as u64) << 24) |
    ((p[4] as u64) << 32) |
    ((p[5] as u64) << 40) |
    ((p[6] as u64) << 48) |
    ((p[7] as u64) << 56)
}

macro_rules! rotl {
    ($x:expr, $b:expr) => {
        (($x) << ($b)) | (($x) >> (64 - ($b)))
    };
}

macro_rules! sipround {
    ($v0:expr, $v1:expr, $v2:expr, $v3:expr) => {
        $v0 = $v0.wrapping_add($v1);
        $v2 = $v2.wrapping_add($v3);
        $v1 = rotl!($v1, 13);
        $v3 = rotl!($v3, 16);
        $v1 ^= $v0;
        $v3 ^= $v2;
        $v0 = rotl!($v0, 32);
        $v2 = $v2.wrapping_add($v1);
        $v0 = $v0.wrapping_add($v3);
        $v1 = rotl!($v1, 17);
        $v3 = rotl!($v3, 21);
        $v1 ^= $v2;
        $v3 ^= $v0;
        $v2 = rotl!($v2, 32);
    };
}

fn siphash24_two_parts(p1: &[u8], p2: &[u8], key: &[u8; 16]) -> u64 {
    let k0 = u8to64_le(&key[0..8]);
    let k1 = u8to64_le(&key[8..16]);

    let mut v0 = 0x736f6d6570736575 ^ k0;
    let mut v1 = 0x646f6f676e6f7465 ^ k1;
    let mut v2 = 0x6c7967656e657261 ^ k0;
    let mut v3 = 0x7465646279746573 ^ k1;

    let total_len = p1.len() + p2.len();
    let mut b = (total_len as u64) << 56;

    let mut processed = 0;
    let mut m = 0u64;
    let mut m_bytes = 0;

    // We process each byte one by one
    for &val in p1 {
        m |= (val as u64) << (m_bytes * 8);
        m_bytes += 1;
        if m_bytes == 8 {
            v3 ^= m;
            sipround!(v0, v1, v2, v3);
            sipround!(v0, v1, v2, v3);
            v0 ^= m;
            m = 0;
            m_bytes = 0;
        }
        processed += 1;
    }
    for &val in p2 {
        m |= (val as u64) << (m_bytes * 8);
        m_bytes += 1;
        if m_bytes == 8 {
            v3 ^= m;
            sipround!(v0, v1, v2, v3);
            sipround!(v0, v1, v2, v3);
            v0 ^= m;
            m = 0;
            m_bytes = 0;
        }
        processed += 1;
    }

    let mut last_block = 0u64;
    let remaining = total_len - processed;
    for i in 0..remaining {
        let idx = processed + i;
        let val = if idx < p1.len() { p1[idx] } else { p2[idx - p1.len()] };
        last_block |= (val as u64) << (i * 8);
    }

    b |= last_block;
    v3 ^= b;
    sipround!(v0, v1, v2, v3);
    sipround!(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    sipround!(v0, v1, v2, v3);
    sipround!(v0, v1, v2, v3);
    sipround!(v0, v1, v2, v3);
    sipround!(v0, v1, v2, v3);
    v0 ^ v1 ^ v2 ^ v3
}

/// Derive the SipHash key from the pre-shared mesh key (ADR-032 §2.8).
pub fn derive_siphash_key(mesh_key: &[u8; 16]) -> [u8; 16] {
    type HmacSha256 = Hmac<Sha256>;
    let mut mac = HmacSha256::new_from_slice(mesh_key).expect("HMAC should init");
    mac.update(b"csi-frame-siphash");
    let result = mac.finalize().into_bytes();
    let mut derived = [0u8; 16];
    derived.copy_from_slice(&result[..16]);
    derived
}

/// Parser for ESP32 CSI binary frames (ADR-018 format).
pub struct Esp32CsiParser;

impl Esp32CsiParser {
    /// Parse a single CSI frame from a byte buffer.
    ///
    /// The buffer must contain at least the header (20 bytes) plus the I/Q data.
    /// Returns the parsed frame and the number of bytes consumed.
    pub fn parse_frame(data: &[u8]) -> Result<(CsiFrame, usize), ParseError> {
        Self::parse_frame_with_key(data, None)
    }

    /// Parse a single CSI frame from a byte buffer and optionally verify the SipHash tag.
    pub fn parse_frame_with_key(
        data: &[u8],
        siphash_key: Option<&[u8; 16]>,
    ) -> Result<(CsiFrame, usize), ParseError> {
        // A recognized sibling packet (ADR-039 vitals, ADR-081 feature state, …)
        // multiplexed onto the CSI UDP port should be reported as such.
        if data.len() >= 4 {
            let magic = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            if let Some(kind) = echopose_sibling_packet_name(magic) {
                return Err(ParseError::NonCsiPacket { magic, kind });
            }
        }

        if data.len() < 4 {
            return Err(ParseError::InsufficientData {
                needed: 4,
                got: data.len(),
            });
        }

        let magic = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
        let is_auth = magic == ESP32_CSI_AUTHENTICATED_MAGIC;

        if magic != ESP32_CSI_MAGIC && !is_auth {
            return Err(ParseError::InvalidMagic {
                expected: ESP32_CSI_MAGIC,
                got: magic,
            });
        }

        let header_size = if is_auth { AUTHENTICATED_HEADER_SIZE } else { HEADER_SIZE };

        if data.len() < header_size {
            return Err(ParseError::InsufficientData {
                needed: header_size,
                got: data.len(),
            });
        }

        let mut cursor = Cursor::new(data);
        let _magic = cursor.read_u32::<LittleEndian>().unwrap();

        // Node ID (offset 4, 1 byte)
        let node_id = cursor.read_u8().map_err(|_| ParseError::ByteError {
            offset: 4,
            message: "Failed to read node ID".into(),
        })?;

        // Number of antennas (offset 5, 1 byte)
        let n_antennas = cursor.read_u8().map_err(|_| ParseError::ByteError {
            offset: 5,
            message: "Failed to read antenna count".into(),
        })?;

        if n_antennas == 0 || n_antennas > MAX_ANTENNAS {
            return Err(ParseError::InvalidAntennaCount { count: n_antennas });
        }

        // Number of subcarriers (offset 6, 2 bytes LE)
        let n_subcarriers =
            cursor
                .read_u16::<LittleEndian>()
                .map_err(|_| ParseError::ByteError {
                    offset: 6,
                    message: "Failed to read subcarrier count".into(),
                })? as usize;

        if n_subcarriers > MAX_SUBCARRIERS {
            return Err(ParseError::InvalidSubcarrierCount {
                count: n_subcarriers,
                max: MAX_SUBCARRIERS,
            });
        }

        // Frequency MHz (offset 8, 4 bytes LE)
        let channel_freq_mhz =
            cursor
                .read_u32::<LittleEndian>()
                .map_err(|_| ParseError::ByteError {
                    offset: 8,
                    message: "Failed to read frequency".into(),
                })?;

        // Sequence number (offset 12, 4 bytes LE)
        let sequence = cursor
            .read_u32::<LittleEndian>()
            .map_err(|_| ParseError::ByteError {
                offset: 12,
                message: "Failed to read sequence number".into(),
            })?;

        // RSSI (offset 16, 1 byte signed)
        let rssi_dbm = cursor.read_i8().map_err(|_| ParseError::ByteError {
            offset: 16,
            message: "Failed to read RSSI".into(),
        })?;

        // Noise floor (offset 17, 1 byte signed)
        let noise_floor_dbm = cursor.read_i8().map_err(|_| ParseError::ByteError {
            offset: 17,
            message: "Failed to read noise floor".into(),
        })?;

        let ppdu_byte = cursor.read_u8().map_err(|_| ParseError::ByteError {
            offset: 18,
            message: "Failed to read PPDU type byte".into(),
        })?;
        let flags_byte = cursor.read_u8().map_err(|_| ParseError::ByteError {
            offset: 19,
            message: "Failed to read flags byte".into(),
        })?;
        let ppdu_type = PpduType::from_byte(ppdu_byte);
        let adr018_flags = Adr018Flags::from_byte(flags_byte);

        // Read and verify the SipHash tag if authenticated and key is provided
        if is_auth {
            let mut expected_tag_bytes = [0u8; 8];
            use std::io::Read;
            cursor.read_exact(&mut expected_tag_bytes).map_err(|_| ParseError::ByteError {
                offset: 20,
                message: "Failed to read SipHash tag".into(),
            })?;
            let expected_tag = u64::from_le_bytes(expected_tag_bytes);

            if let Some(key) = siphash_key {
                let iq_pair_count = n_antennas as usize * n_subcarriers;
                let iq_byte_count = iq_pair_count * 2;
                let total_frame_size = header_size + iq_byte_count;

                if data.len() < total_frame_size {
                    return Err(ParseError::InsufficientData {
                        needed: total_frame_size,
                        got: data.len(),
                    });
                }

                let iq_data = &data[header_size..total_frame_size];
                let computed_tag = siphash24_two_parts(&data[0..20], iq_data, key);

                if computed_tag != expected_tag {
                    return Err(ParseError::VerificationFailed);
                }
            }
        }

        // I/Q data: n_antennas * n_subcarriers * 2 bytes
        let iq_pair_count = n_antennas as usize * n_subcarriers;
        let iq_byte_count = iq_pair_count * 2;
        let total_frame_size = header_size + iq_byte_count;

        if data.len() < total_frame_size {
            return Err(ParseError::InsufficientData {
                needed: total_frame_size,
                got: data.len(),
            });
        }

        // Parse I/Q pairs — stored as [ant0_sc0_I, ant0_sc0_Q, ant0_sc1_I, ant0_sc1_Q, ..., ant1_sc0_I, ...]
        let iq_start = header_size;
        let mut subcarriers = Vec::with_capacity(iq_pair_count);

        let half = n_subcarriers as i16 / 2;

        for ant in 0..n_antennas as usize {
            for sc_idx in 0..n_subcarriers {
                let byte_offset = iq_start + (ant * n_subcarriers + sc_idx) * 2;
                let i_val = data[byte_offset] as i8 as i16;
                let q_val = data[byte_offset + 1] as i8 as i16;

                let index = if (sc_idx as i16) < half {
                    -(half - sc_idx as i16)
                } else {
                    sc_idx as i16 - half + 1
                };

                subcarriers.push(SubcarrierData {
                    i: i_val,
                    q: q_val,
                    index,
                });
            }
        }

        // Determine bandwidth from PPDU type + subcarrier count (ADR-110).
        let bandwidth = if ppdu_type.is_he() {
            if adr018_flags.bw40 || n_subcarriers > 256 {
                Bandwidth::Bw40
            } else {
                Bandwidth::Bw20
            }
        } else {
            match n_subcarriers {
                0..=64 => Bandwidth::Bw20,
                65..=128 => Bandwidth::Bw40,
                129..=242 => Bandwidth::Bw80,
                _ => Bandwidth::Bw160,
            }
        };

        let frame = CsiFrame {
            metadata: CsiMetadata {
                timestamp: Utc::now(),
                node_id,
                n_antennas,
                n_subcarriers: n_subcarriers as u16,
                channel_freq_mhz,
                rssi_dbm,
                noise_floor_dbm,
                bandwidth,
                antenna_config: AntennaConfig {
                    tx_antennas: 1,
                    rx_antennas: n_antennas,
                },
                sequence,
                ppdu_type,
                adr018_flags,
            },
            subcarriers,
        };

        Ok((frame, total_frame_size))
    }

    /// Parse multiple frames from a byte buffer (e.g., from a UDP read), optionally verifying signatures.
    ///
    /// Returns all successfully parsed frames and the total bytes consumed.
    pub fn parse_stream_with_key(
        data: &[u8],
        siphash_key: Option<&[u8; 16]>,
    ) -> (Vec<CsiFrame>, usize) {
        let mut frames = Vec::new();
        let mut offset = 0;

        while offset < data.len() {
            match Self::parse_frame_with_key(&data[offset..], siphash_key) {
                Ok((frame, consumed)) => {
                    frames.push(frame);
                    offset += consumed;
                }
                Err(_) => {
                    // Try to find next magic number for resync
                    offset += 1;
                    while offset + 4 <= data.len() {
                        let candidate = u32::from_le_bytes([
                            data[offset],
                            data[offset + 1],
                            data[offset + 2],
                            data[offset + 3],
                        ]);
                        if candidate == ESP32_CSI_MAGIC || candidate == ESP32_CSI_AUTHENTICATED_MAGIC {
                            break;
                        }
                        offset += 1;
                    }
                }
            }
        }

        (frames, offset)
    }

    /// Parse multiple frames from a byte buffer (e.g., from a UDP read).
    ///
    /// Returns all successfully parsed frames and the total bytes consumed.
    pub fn parse_stream(data: &[u8]) -> (Vec<CsiFrame>, usize) {
        Self::parse_stream_with_key(data, None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a valid ADR-018 ESP32 CSI frame with known parameters.
    /// PPDU type + flags bytes (offset 18-19) are zero — pre-ADR-110 default,
    /// which round-trips as PpduType::HtLegacy + Adr018Flags::default().
    fn build_test_frame(node_id: u8, n_antennas: u8, subcarrier_pairs: &[(i8, i8)]) -> Vec<u8> {
        build_test_frame_with_he(node_id, n_antennas, subcarrier_pairs, 0, 0)
    }

    /// ADR-110-aware variant: explicit byte 18 (PPDU type) and byte 19 (flags).
    fn build_test_frame_with_he(
        node_id: u8,
        n_antennas: u8,
        subcarrier_pairs: &[(i8, i8)],
        ppdu_byte: u8,
        flags_byte: u8,
    ) -> Vec<u8> {
        let n_subcarriers = if n_antennas == 0 {
            subcarrier_pairs.len()
        } else {
            subcarrier_pairs.len() / n_antennas as usize
        };

        let mut buf = Vec::new();
        buf.extend_from_slice(&ESP32_CSI_MAGIC.to_le_bytes());
        buf.push(node_id);
        buf.push(n_antennas);
        buf.extend_from_slice(&(n_subcarriers as u16).to_le_bytes());
        buf.extend_from_slice(&2437u32.to_le_bytes());
        buf.extend_from_slice(&1u32.to_le_bytes());
        buf.push((-50i8) as u8);
        buf.push((-95i8) as u8);
        buf.push(ppdu_byte);
        buf.push(flags_byte);
        for (i, q) in subcarrier_pairs {
            buf.push(*i as u8);
            buf.push(*q as u8);
        }

        buf
    }

    // ── ADR-110: byte 18-19 round-trip tests ─────────────────────────────────

    #[test]
    fn adr110_pre_adr110_firmware_round_trips_as_ht_legacy_default_flags() {
        // Pre-ADR-110 firmware writes zeros to bytes 18-19. The parser must
        // surface that as HtLegacy + default flags so old aggregators see
        // identical behavior to before the extension.
        let data = build_test_frame(1, 1, &[(0, 0); 56]);
        let (frame, _) = Esp32CsiParser::parse_frame(&data).unwrap();
        assert_eq!(frame.metadata.ppdu_type, PpduType::HtLegacy);
        assert_eq!(frame.metadata.adr018_flags, Adr018Flags::default());
        assert!(!frame.metadata.ppdu_type.is_he());
    }

    #[test]
    fn adr110_he_su_ppdu_decodes() {
        let data = build_test_frame_with_he(2, 1, &[(0, 0); 56], /*PPDU*/ 1, /*flags*/ 0);
        let (frame, _) = Esp32CsiParser::parse_frame(&data).unwrap();
        assert_eq!(frame.metadata.ppdu_type, PpduType::HeSu);
        assert!(frame.metadata.ppdu_type.is_he());
    }

    #[test]
    fn adr110_he_mu_he_tb_decode() {
        let mu = build_test_frame_with_he(3, 1, &[(0, 0); 56], 2, 0);
        let tb = build_test_frame_with_he(4, 1, &[(0, 0); 56], 3, 0);
        let (mu_frame, _) = Esp32CsiParser::parse_frame(&mu).unwrap();
        let (tb_frame, _) = Esp32CsiParser::parse_frame(&tb).unwrap();
        assert_eq!(mu_frame.metadata.ppdu_type, PpduType::HeMu);
        assert_eq!(tb_frame.metadata.ppdu_type, PpduType::HeTb);
    }

    #[test]
    fn adr110_unknown_ppdu_byte_decodes_as_unknown() {
        let data = build_test_frame_with_he(5, 1, &[(0, 0); 56], 0xFF, 0);
        let (frame, _) = Esp32CsiParser::parse_frame(&data).unwrap();
        assert_eq!(frame.metadata.ppdu_type, PpduType::Unknown);
    }

    #[test]
    fn adr110_flags_round_trip_all_bits() {
        // All known flag bits set: bw40 (0x01) + STBC (0x04) + LDPC (0x08) + 15.4-sync (0x10) = 0x1D
        let data = build_test_frame_with_he(6, 1, &[(0, 0); 56], 1, 0x1D);
        let (frame, _) = Esp32CsiParser::parse_frame(&data).unwrap();
        assert!(frame.metadata.adr018_flags.bw40);
        assert!(frame.metadata.adr018_flags.stbc);
        assert!(frame.metadata.adr018_flags.ldpc);
        assert!(frame.metadata.adr018_flags.ieee802154_sync_valid);
        // Round-trip the encoder
        assert_eq!(frame.metadata.adr018_flags.to_byte(), 0x1D);
    }

    #[test]
    fn adr110_ppdu_byte_round_trips_for_known_variants() {
        for v in [PpduType::HtLegacy, PpduType::HeSu, PpduType::HeMu, PpduType::HeTb, PpduType::Unknown] {
            assert_eq!(PpduType::from_byte(v.to_byte()), v, "round-trip failed for {v:?}");
        }
    }

    #[test]
    fn test_parse_valid_frame() {
        // 1 antenna, 56 subcarriers
        let pairs: Vec<(i8, i8)> = (0..56).map(|i| (i as i8, (i * 2 % 127) as i8)).collect();
        let data = build_test_frame(1, 1, &pairs);

        let (frame, consumed) = Esp32CsiParser::parse_frame(&data).unwrap();

        assert_eq!(consumed, HEADER_SIZE + 56 * 2);
        assert_eq!(frame.subcarrier_count(), 56);
        assert_eq!(frame.metadata.node_id, 1);
        assert_eq!(frame.metadata.n_antennas, 1);
        assert_eq!(frame.metadata.n_subcarriers, 56);
        assert_eq!(frame.metadata.rssi_dbm, -50);
        assert_eq!(frame.metadata.channel_freq_mhz, 2437);
        assert_eq!(frame.metadata.bandwidth, Bandwidth::Bw20);
        assert!(frame.is_valid());
    }

    #[test]
    fn test_parse_insufficient_data() {
        let data = &[0u8; 10];
        let result = Esp32CsiParser::parse_frame(data);
        assert!(matches!(result, Err(ParseError::InsufficientData { .. })));
    }

    #[test]
    fn test_parse_invalid_magic() {
        let mut data = build_test_frame(1, 1, &[(10, 20)]);
        // Corrupt magic to a value that isn't any known EchoPose packet.
        data[0..4].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
        let result = Esp32CsiParser::parse_frame(&data);
        assert!(matches!(result, Err(ParseError::InvalidMagic { .. })));
    }

    #[test]
    fn test_sibling_vitals_packet_is_not_invalid_magic() {
        // EchoPose#517: a 32-byte ADR-039 vitals packet (magic 0xC5110002)
        // arrives on the same UDP port as CSI frames. It must be reported as
        // a recognized sibling packet, not a corrupt CSI frame.
        let mut data = vec![0u8; 32];
        data[0..4].copy_from_slice(&ECHOPOSE_VITALS_MAGIC.to_le_bytes());
        match Esp32CsiParser::parse_frame(&data) {
            Err(ParseError::NonCsiPacket { magic, kind }) => {
                assert_eq!(magic, ECHOPOSE_VITALS_MAGIC);
                assert_eq!(kind, "ADR-039 edge vitals");
            }
            other => panic!("expected NonCsiPacket, got {other:?}"),
        }
    }

    #[test]
    fn test_all_sibling_magics_classified() {
        for m in [
            ECHOPOSE_VITALS_MAGIC,
            ECHOPOSE_FEATURE_MAGIC,
            ECHOPOSE_FUSED_VITALS_MAGIC,
            ECHOPOSE_COMPRESSED_CSI_MAGIC,
            ECHOPOSE_FEATURE_STATE_MAGIC,
            ECHOPOSE_TEMPORAL_MAGIC,
        ] {
            assert!(
                echopose_sibling_packet_name(m).is_some(),
                "{m:#010x} unclassified"
            );
            let mut data = vec![0u8; 24];
            data[0..4].copy_from_slice(&m.to_le_bytes());
            assert!(
                matches!(
                    Esp32CsiParser::parse_frame(&data),
                    Err(ParseError::NonCsiPacket { .. })
                ),
                "{m:#010x} should parse as NonCsiPacket"
            );
        }
        // The CSI magic itself is not a "sibling".
        assert!(echopose_sibling_packet_name(ESP32_CSI_MAGIC).is_none());
    }

    #[test]
    fn test_amplitude_phase_from_known_iq() {
        let pairs = vec![(100i8, 0i8), (0, 50), (30, 40)];
        let data = build_test_frame(1, 1, &pairs);
        let (frame, _) = Esp32CsiParser::parse_frame(&data).unwrap();

        let (amps, _phases) = frame.to_amplitude_phase();
        assert_eq!(amps.len(), 3);

        // I=100, Q=0 -> amplitude=100
        assert!((amps[0] - 100.0).abs() < 0.01);
        // I=0, Q=50 -> amplitude=50
        assert!((amps[1] - 50.0).abs() < 0.01);
        // I=30, Q=40 -> amplitude=50
        assert!((amps[2] - 50.0).abs() < 0.01);
    }

    #[test]
    fn test_parse_stream_with_multiple_frames() {
        let pairs: Vec<(i8, i8)> = (0..4).map(|i| (10 + i, 20 + i)).collect();
        let frame1 = build_test_frame(1, 1, &pairs);
        let frame2 = build_test_frame(2, 1, &pairs);

        let mut combined = Vec::new();
        combined.extend_from_slice(&frame1);
        combined.extend_from_slice(&frame2);

        let (frames, _consumed) = Esp32CsiParser::parse_stream(&combined);
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].metadata.node_id, 1);
        assert_eq!(frames[1].metadata.node_id, 2);
    }

    #[test]
    fn test_parse_stream_with_garbage() {
        let pairs: Vec<(i8, i8)> = (0..4).map(|i| (10 + i, 20 + i)).collect();
        let frame = build_test_frame(1, 1, &pairs);

        let mut data = Vec::new();
        data.extend_from_slice(&[0xFF, 0xFF, 0xFF]); // garbage
        data.extend_from_slice(&frame);

        let (frames, _) = Esp32CsiParser::parse_stream(&data);
        assert_eq!(frames.len(), 1);
    }

    #[test]
    fn test_multi_antenna_frame() {
        // 3 antennas, 4 subcarriers each = 12 I/Q pairs total
        let mut pairs = Vec::new();
        for ant in 0..3u8 {
            for sc in 0..4u8 {
                pairs.push(((ant * 10 + sc) as i8, ((ant * 10 + sc) * 2) as i8));
            }
        }

        let data = build_test_frame(5, 3, &pairs);
        let (frame, consumed) = Esp32CsiParser::parse_frame(&data).unwrap();

        assert_eq!(consumed, HEADER_SIZE + 12 * 2);
        assert_eq!(frame.metadata.node_id, 5);
        assert_eq!(frame.metadata.n_antennas, 3);
        assert_eq!(frame.metadata.n_subcarriers, 4);
        assert_eq!(frame.subcarrier_count(), 12); // 3 antennas * 4 subcarriers
        assert_eq!(frame.metadata.antenna_config.rx_antennas, 3);
    }

    #[test]
    fn test_authenticated_frame_parsing() {
        let mesh_key = [0x55u8; 16];
        let derived_key = derive_siphash_key(&mesh_key);

        // Build standard raw CSI header + payload
        let pairs: Vec<(i8, i8)> = (0..56).map(|i| (i as i8, (i * 2 % 127) as i8)).collect();
        let raw_frame = build_test_frame(1, 1, &pairs);
        let header_20 = &raw_frame[0..20];
        let iq_data = &raw_frame[20..];

        // Construct authenticated frame: 20 bytes header + 8 bytes tag + IQ data
        let mut auth_frame = Vec::new();
        auth_frame.extend_from_slice(header_20);
        // Replace magic at start of header with ESP32_CSI_AUTHENTICATED_MAGIC
        auth_frame[0..4].copy_from_slice(&ESP32_CSI_AUTHENTICATED_MAGIC.to_le_bytes());

        // Calculate tag
        let tag = siphash24_two_parts(&auth_frame[0..20], iq_data, &derived_key);
        auth_frame.extend_from_slice(&tag.to_le_bytes());
        auth_frame.extend_from_slice(iq_data);

        // 1. Parse without key (should succeed/fallback in transitional mode)
        let (frame, consumed) = Esp32CsiParser::parse_frame(&auth_frame).unwrap();
        assert_eq!(consumed, auth_frame.len());
        assert_eq!(frame.metadata.node_id, 1);

        // 2. Parse with correct key (should succeed)
        let (frame, consumed) = Esp32CsiParser::parse_frame_with_key(&auth_frame, Some(&derived_key)).unwrap();
        assert_eq!(consumed, auth_frame.len());
        assert_eq!(frame.metadata.node_id, 1);

        // 3. Parse with incorrect key (should fail verification)
        let wrong_key = [0xAAu8; 16];
        let result = Esp32CsiParser::parse_frame_with_key(&auth_frame, Some(&wrong_key));
        assert!(matches!(result, Err(ParseError::VerificationFailed)));

        // 4. Parse with corrupted signature (should fail verification)
        let mut corrupted_frame = auth_frame.clone();
        corrupted_frame[20] ^= 0x01; // Corrupt a bit of the tag
        let result = Esp32CsiParser::parse_frame_with_key(&corrupted_frame, Some(&derived_key));
        assert!(matches!(result, Err(ParseError::VerificationFailed)));
    }
}
