//! ADR-115 P9 — MQTT pipeline throughput micro-benchmark.
//!
//! Measures the hot-path cost of:
//!   - Building a HA discovery payload (`DiscoveryBuilder::build`)
//!   - Encoding a numeric state message (`StateEncoder::numeric`)
//!   - Rate-limit decision (`RateLimiter::allow`)
//!   - Privacy filter (`privacy::decide`)
//!   - Full bus tick across all 10 semantic primitives
//!
//! Targets (laptop-class, single-threaded, release build):
//!   - discovery payload: < 5 µs
//!   - state encode:      < 2 µs
//!   - rate limit:        < 100 ns
//!   - privacy decide:    < 50 ns
//!   - bus tick (10 prim):< 10 µs
//!
//! The bench is intentionally feature-gated so the default workspace
//! build doesn't pull `criterion` in (it has a big-ish dep tree).
//!
//! Run with:
//!   cargo bench -p wifi-densepose-sensing-server --bench mqtt_throughput
//! ADR-115 P9 — MQTT 管道吞吐量微基准测试。
//!
//! 测量以下操作的热点路径成本：
//!   - 构建高可用性发现负载（`DiscoveryBuilder::build`）
//!   - 编码数值状态消息（`StateEncoder::numeric`）
//!   - 率限决策（`RateLimiter::allow`）
//!   - 隐私过滤器（`privacy::decide`）
//!   - 所有10个语义原语的完整总线时钟
//!
//! 目标（笔记本电脑级，单线程，发布版）：
//!   - 发现负载：< 5 微秒
//!   - 状态编码：      < 2 微秒
//!   - 速率限制：        < 100 纳秒
//!   - 隐私决策：    < 50 纳秒
//!   - 总线时钟（10 个基本单位）：< 10 微秒
//!
//! 为了便于管理，该工作区特意设置了功能限制，因此默认的构建不会拉取 `criterion`（它依赖树较大）。
//!
//! 使用以下命令运行：
//!   cargo bench -p wifi-densepose-sensing-server --bench mqtt_throughput
//! 只有编译时开启 `mqtt` feature，这个文件里所有代码才会被编译器纳入编译；如果没开 mqtt，整个文件直接被忽略，完全不参与编译。
#![cfg(feature = "mqtt")]

/// 本基准测试测量了 MQTT 管道的吞吐量，包括发现负载、状态编码、速率限制、隐私决策和总线时钟。
use std::time::Duration;
use criterion::{black_box, criterion_group, criterion_main, BatchSize, Criterion};

use wifi_densepose_sensing_server::mqtt::{
    config::PublishRates,
    discovery::{DiscoveryBuilder, EntityKind},
    privacy::decide,
    state::{RateLimiter, StateEncoder, VitalsSnapshot},
};
use wifi_densepose_sensing_server::semantic::{PrimitiveConfig, RawSnapshot, SemanticBus};

fn builder() -> DiscoveryBuilder<'static> {
    DiscoveryBuilder {
        discovery_prefix: "homeassistant",
        node_id: "aabbccddeeff",
        node_friendly_name: Some("Bedroom"),
        sw_version: "v0.7.0",
        model: "ESP32-S3 CSI node",
        via_device: Some("cognitum_seed_1"),
    }
}

fn snap() -> VitalsSnapshot {
    VitalsSnapshot {
        node_id: "aabbccddeeff".into(),
        timestamp_ms: 1779_512_400_000,
        presence: true,
        fall_detected: false,
        motion: 0.35,
        motion_energy: 1234.5,
        presence_score: 0.91,
        breathing_rate_bpm: Some(14.2),
        heartrate_bpm: Some(68.2),
        n_persons: 1,
        rssi_dbm: Some(-52.0),
        vital_confidence: 0.87,
    }
}

fn raw_snap() -> RawSnapshot {
    RawSnapshot {
        node_id: "aabbccddeeff".into(),
        since_start: Duration::from_secs(120),
        timestamp_ms: 1779_512_400_000,
        presence: true,
        fall_detected: false,
        motion: 0.35,
        motion_energy: 1234.5,
        breathing_rate_bpm: Some(14.2),
        heart_rate_bpm: Some(68.2),
        n_persons: 1,
        rssi_dbm: Some(-52.0),
        vital_confidence: 0.87,
        active_zones: vec!["bathroom".into()],
        bed_zones: vec!["bedroom".into()],
        local_seconds_since_midnight: 2 * 3600,
    }
}

fn rates() -> PublishRates {
    PublishRates::default()
}

fn bench_discovery_payload(c: &mut Criterion) {
    let b = builder();
    c.bench_function("discovery::build_presence", |bench| {
        bench.iter(|| {
            let cfg = b.build(black_box(EntityKind::Presence));
            black_box(serde_json::to_string(&cfg).unwrap())
        });
    });
    c.bench_function("discovery::build_heart_rate", |bench| {
        bench.iter(|| {
            let cfg = b.build(black_box(EntityKind::HeartRate));
            black_box(serde_json::to_string(&cfg).unwrap())
        });
    });
    c.bench_function("discovery::build_fall_event", |bench| {
        bench.iter(|| {
            let cfg = b.build(black_box(EntityKind::FallDetected));
            black_box(serde_json::to_string(&cfg).unwrap())
        });
    });
}

fn bench_state_encode(c: &mut Criterion) {
    let b = builder();
    let s = snap();
    let enc = StateEncoder { builder: &b };
    c.bench_function("state::numeric_heart_rate", |bench| {
        bench.iter(|| {
            black_box(enc.numeric(EntityKind::HeartRate, &s).unwrap())
        });
    });
    c.bench_function("state::boolean_presence", |bench| {
        bench.iter(|| {
            black_box(enc.boolean(EntityKind::Presence, true).unwrap())
        });
    });
    c.bench_function("state::event_fall", |bench| {
        bench.iter(|| {
            black_box(enc.event(EntityKind::FallDetected, "fall_detected", 0, Some(0.87)).unwrap())
        });
    });
}

fn bench_rate_limit(c: &mut Criterion) {
    let r = rates();
    c.bench_function("rate_limiter::allow_first", |bench| {
        bench.iter_batched(
            RateLimiter::new,
            |mut rl| {
                black_box(rl.allow(
                    black_box("bench-node"),
                    black_box(EntityKind::HeartRate),
                    Duration::from_secs(0),
                    &r,
                ))
            },
            BatchSize::SmallInput,
        );
    });
    c.bench_function("rate_limiter::allow_within_gap", |bench| {
        bench.iter_batched(
            || {
                let mut rl = RateLimiter::new();
                rl.allow("bench-node", EntityKind::HeartRate, Duration::from_secs(0), &r);
                rl
            },
            |mut rl| {
                black_box(rl.allow(
                    black_box("bench-node"),
                    black_box(EntityKind::HeartRate),
                    Duration::from_secs(1),
                    &r,
                ))
            },
            BatchSize::SmallInput,
        );
    });
}

fn bench_privacy(c: &mut Criterion) {
    c.bench_function("privacy::decide_hr_strip", |bench| {
        bench.iter(|| black_box(decide(EntityKind::HeartRate, true)));
    });
    c.bench_function("privacy::decide_presence_keep", |bench| {
        bench.iter(|| black_box(decide(EntityKind::Presence, true)));
    });
}

fn bench_semantic_bus(c: &mut Criterion) {
    c.bench_function("semantic::bus_tick_all_10_primitives", |bench| {
        bench.iter_batched(
            || (SemanticBus::new(PrimitiveConfig::default()), raw_snap()),
            |(mut bus, s)| black_box(bus.tick(&s)),
            BatchSize::SmallInput,
        );
    });
}

criterion_group!(
    benches,
    bench_discovery_payload,
    bench_state_encode,
    bench_rate_limit,
    bench_privacy,
    bench_semantic_bus,
);
criterion_main!(benches);
