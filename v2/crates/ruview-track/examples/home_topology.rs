//! ADR-307 跨房间追踪的拓扑配置可运行样例。
//!
//! 场景为一居室户型：玄关和每个房间都只通过走廊相连，卧室只能经客厅到达。
//! 样例演示一个人从玄关 → 走廊 → 厨房（全程同一假名，逐帧 matched），
//! 然后展示一次拓扑上不可能的"厨房 → 卧室"跳变被**拒绝**
//! （改为新开一个 tentative 假名，而不是错误关联——under-linking 是
//! 隐私安全的失败模式），最后演示第二个人出现在走廊。
//!
//! 运行方式：`cargo run -p ruview-track --example home_topology`

use ruview_ontology::{Container, SpaceId};
use ruview_track::{
    Association, CoarseFeature, Detection, Topology, TrackManager, TrackerConfig, UnknownReason,
};

/// 户型布局（每个房间只经走廊相连）：
///
/// ```text
/// entry ── hallway ── kitchen
///            │
///            └──── living_room ── bedroom
/// ```
const DOORWAYS: [(&str, &str); 4] = [
    ("entry", "hallway"),
    ("hallway", "kitchen"),
    ("hallway", "living_room"),
    ("living_room", "bedroom"),
];

/// 构造一个空间容器（id 会在边界处校验：非空、限长、无控制字符）。
fn space(id: &str) -> Result<Container, Box<dyn std::error::Error>> {
    Ok(Container::Space { id: SpaceId::new(id)? })
}

/// 取容器的显示名（用于输出）。
fn name(c: &Container) -> &str {
    match c {
        Container::Space { id } => id.as_str(),
        Container::Zone { id } => id.as_str(),
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. 构建拓扑：每扇门一条无向边。没有列出的连接一律**不可通行**——
    //    空拓扑只允许同容器内的连续性，这是隐私安全的默认行为。
    let mut topo = Topology::new();
    for (a, b) in DOORWAYS {
        topo.connect(&space(a)?, &space(b)?);
    }
    println!("门廊: {DOORWAYS:?}");
    println!(
        "kitchen↔hallway adjacent = {} (必须为 true)",
        topo.adjacent(&space("kitchen")?, &space("hallway")?)
    );
    println!(
        "kitchen↔bedroom adjacent = {} (必须为 false：没有直通门)\n",
        topo.adjacent(&space("kitchen")?, &space("bedroom")?)
    );

    // 2. 默认策略：gate_position 2.0、gate_feature 6.0、confirm_after 2、
    //    max_coast_ms 5000。position 是各空间自己的局部坐标；同一个人的
    //    相邻两次检测必须落在彼此 `gate_position` 之内（即门口附近），
    //    才能通过数值门限。
    let mut mgr = TrackManager::new(TrackerConfig::default(), topo);

    // 粗粒度、不可逆的外观描述子（量化到 3-bit 桶）。
    // 同一个人 → 相同粗特征；不同的人 → 距离很远。
    let feat_a = CoarseFeature::quantize(&[0.8, 0.2, 0.5])?;
    let feat_b = CoarseFeature::quantize(&[0.1, 0.9, 0.3])?;

    // 3. 甲从玄关走进来：玄关 → 走廊 → 厨房，全程同一个假名。
    println!("--- person A: entry -> hallway -> kitchen ---");
    for (at, room, pos) in [
        (0, "entry", [2.0, 1.0]),
        (500, "hallway", [2.0, 2.0]),
        (1_000, "kitchen", [2.0, 1.0]),
    ] {
        let d = Detection::new(space(room)?, pos, feat_a.clone(), at)?;
        let o = mgr.ingest(d)?;
        report(&o, at, room);
    }

    // 4. 拓扑上不可能的跳变：厨房检测之后紧接着出现一个卧室检测。
    //    两间房没有直通门，tracker 绝不能把它关联到甲的轨迹上，
    //    而是新开一个 tentative 假名（UnknownReason::TopologyBlocked）
    //    —— 宁可欠关联，也绝不错误关联。
    println!("\n--- impossible jump: kitchen -> bedroom ---");
    let d = Detection::new(space("bedroom")?, [2.0, 1.0], feat_a.clone(), 1_500)?;
    let o = mgr.ingest(d)?;
    report(&o, 1_500, "bedroom");
    match &o.association {
        Association::Unknown { reason, .. } => assert!(*reason == UnknownReason::TopologyBlocked),
        Association::Matched { .. } => panic!("拓扑上不可能的关联不允许 matched"),
    }

    // 5. 乙出现在走廊，距离所有轨迹的最近位置都很远。
    println!("\n--- person B appears in the hallway ---");
    for (at, pos) in [(2_000, [1.0, 4.0]), (2_500, [1.0, 4.2])] {
        let d = Detection::new(space("hallway")?, pos, feat_b.clone(), at)?;
        let o = mgr.ingest(d)?;
        report(&o, at, "hallway");
    }

    // 6. 轨迹历史：甲的持久化轨迹横跨三个房间。
    println!("\n--- persistent histories ---");
    for t in mgr.track_ids() {
        let traj = mgr
            .trajectory(&t)
            .unwrap_or_default()
            .iter()
            .map(name)
            .collect::<Vec<_>>()
            .join(" -> ");
        println!(
            "{} ({}): {}",
            mgr.person_of(&t).map(|p| p.as_str()).unwrap_or("?"),
            mgr.state(&t).map(|s| format!("{s:?}")).unwrap_or_default(),
            traj
        );
    }

    // 7. 隐私操作面：假名可以轮换，且不丢失轨迹及其历史。
    //    实际部署可按周期或按用户请求轮换。
    let bedroom_track = mgr.track_ids()[1].clone();
    let rotated = mgr.rotate_pseudonym(&bedroom_track)?;
    println!("\nrotated bedroom pseudonym -> {rotated}");

    Ok(())
}

/// 打印单次 ingest 的结果（matched 带衰减置信度；unknown 带原因）。
fn report(o: &ruview_track::IngestOutcome, at: i64, room: &str) {
    match &o.association {
        Association::Matched { confidence, .. } => println!(
            "[t={at:>4}ms] {room:<11} -> {} matched (confidence {confidence:.2})",
            o.person.as_str()
        ),
        Association::Unknown { reason, .. } => println!(
            "[t={at:>4}ms] {room:<11} -> {} NEW tentative track ({reason:?})",
            o.person.as_str()
        ),
    }
}
