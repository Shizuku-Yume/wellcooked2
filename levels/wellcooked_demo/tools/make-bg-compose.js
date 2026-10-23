// 关卡背景组合器（v2）——从底层规则出发，而不是套"墙+桌"模板。
//
// 底层原语（primitives）：
//   1) 资产画像：只用实测尺寸把素材自动分类（地面/边界/地标/家具/小件），不写死名单
//   2) 占位网格：0.6 m 一格，任何摆放先占格；与玩法物体/已有艺术件冲突就跳过（R1/R2）
//   3) 分区：室内地板 / 室外地面 / 可选材质带（不同地面材质切出"区域感"）
//   4) 边界：由边界类资产沿一条边连续铺，宽度按各自实测宽度推进（不是固定 2 格）
//   5) 地标：每关 2-3 个不对称的"大件群"，群内按 1 大 + N 中 + M 小 组合
//   6) 家具带：在边界与厨房之间的环带上排布，间距抖动、型号交替
//   7) 小件散布：家具带之间撒细节，密度随距离外扩递减
//   8) 遮挡规则：边界外的物件必须高于边界高度，否则不放（R9）
//   9) 确定性随机：每关固定种子，结果可复现
//
// 用法: node make-bg-compose.js 2 3 4
const fs = require("fs");

const ROOT = "D:/2026-2027秋季学期/操作系统课设/wellcooked2/levels/wellcooked_demo";
const SIZES_FILE = "D:/oc2-level-build/art-sizes.tsv";
const CELL = 0.6;

// ---------- 实测尺寸表 + 资产画像 ----------
const SIZE = {};
for (const line of fs.readFileSync(SIZES_FILE, "utf8").split(/\r?\n/).slice(1)) {
  const c = line.split("\t");
  if (c.length < 9) continue;
  SIZE[c[0] + "/" + c[1]] = { w: +c[3], h: +c[4], d: +c[5], minY: +c[6] };
}
const getSize = (theme, name) => SIZE[theme + "/" + name];

// 按实测尺寸给角色；注意 h 用的是"高"，w/d 是占地
function roleOf(theme, name) {
  const s = getSize(theme, name);
  if (!s || !(s.w > 0) || !(s.d > 0)) return null;
  const span = Math.max(s.w, s.d);
  if (s.h < 0.16) return "flat";                     // 地面/地毯
  if (span > 12) return null;                        // 超大件（数据异常或整片地形）
  if (s.h >= 1.55 && span <= 4.5 && s.d <= 2.2) return "boundary";  // 墙/摊位/围挡
  if (span >= 3.0) return "landmark";                // 地标大件（树、雕塑、沙堡）
  if (s.h >= 0.55 && span >= 0.8) return "furniture";// 桌椅、长椅、摊位
  if (s.h < 0.55 && span <= 2.2) return "flat";      // 薄片（井盖、地毯、贴花）
  return "detail";                                   // 小件
}

function poolOf(theme, want) {
  const out = [];
  for (const key of Object.keys(SIZE)) {
    const [t, n] = [key.slice(0, key.indexOf("/")), key.slice(key.indexOf("/") + 1)];
    if (t !== theme) continue;
    if (roleOf(theme, n) === want) out.push(n);
  }
  return out.sort();
}

// ---------- 确定性随机 ----------
function mulberry32(a) {
  return function () {
    a |= 0; a = a + 0x6D2B79F5 | 0;
    let t = Math.imul(a ^ a >>> 15, 1 | a);
    t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
    return ((t ^ t >>> 14) >>> 0) / 4294967296;
  };
}

// ---------- 占位网格 ----------
class Grid {
  constructor() { this.used = new Set(); }
  key(x, z) { return x + "," + z; }
  blocked(x, z) { return this.used.has(this.key(x, z)); }
  // 以 (x,z) 为中心、按实测占地占格
  footprint(theme, name, x, z, yaw) {
    const s = getSize(theme, name) || { w: 1.2, d: 1.2 };
    let w = Math.max(1, Math.ceil(s.w / CELL)), d = Math.max(1, Math.ceil(s.d / CELL));
    if (yaw === 90 || yaw === 270) { const t = w; w = d; d = t; }
    const x0 = Math.round(x - w / 2), x1 = x0 + w - 1;
    const z0 = Math.round(z - d / 2), z1 = z0 + d - 1;
    return { w, d, x0, x1, z0, z1 };
  }
  fits(fp) {
    for (let x = fp.x0; x <= fp.x1; x++) for (let z = fp.z0; z <= fp.z1; z++) if (this.blocked(x, z)) return false;
    return true;
  }
  take(fp) {
    for (let x = fp.x0; x <= fp.x1; x++) for (let z = fp.z0; z <= fp.z1; z++) this.used.add(this.key(x, z));
  }
  reserve(x, z, r = 1) {
    for (let dx = -r; dx <= r; dx++) for (let dz = -r; dz <= r; dz++) this.used.add(this.key(x + dx, z + dz));
  }
}

// ---------- 每关的美术指导（只写"想要什么"，不写"放哪"） ----------
const LEVELS = {
  2: {
    name: "海滩", theme: "dlc02_beach", seed: 202,
    ground: null, floor: null,                       // City_path 有黄线、不无缝：地板整体删掉，用模板自带地面
    accent: null,
    boundaryPreferred: ["Citywall_brick_01a"],
    landmarkPool: ["palmtree_01", "palmtree_02", "pineapple_plant_01", "m_dlc2_sandcastle_01"],
    midPool: ["dlc2_Planter_01", "City_restaurant_table&chairs_01", "sunbed_table_01"],
    furniturePool: ["City_restaurant_table&chairs_01", "table_01"],
    detailPool: ["palmtree_coconut_01", "sunbed_table_01"],
    streetPool: ["Fire_hydrant"],
    wallDecorPool: ["sign_01", "SignPost_01"],
  },
  3: {
    name: "夏日祭", theme: "dlc11_summer", seed: 303,
    ground: "dlc11_exterior_path_01", floor: "dlc11_exterior_path_side_01",
    accent: null,
    boundaryPreferred: ["dlc11_marketstall_blank_01"],
    landmarkPool: ["dlc11_pinata_eggplant_01", "dlc11_pinata_grapes_01", "dlc11_pinata_orange_01", "dlc11_pinata_strawberry_01"],
    midPool: ["restaurant_plant_02", "walkway_pillars_02", "table_02"],
    furniturePool: ["table_02"],
    chairFront: "x",                 // dlc11_chair_01 默认朝 +x（实测）
    detailPool: ["dlc11_chair_01"],
    streetPool: ["dlc11_exterior_street_bench_01", "dlc11_exterior_fire_hydrant_01", "dlc11_city_tree_01"],
    wallDecorPool: ["dlc11_sign_fireworks_01"],
  },
  4: {
    name: "欧式老城", theme: "dlc07_horde/city", sizeKey: "city", seed: 404,
    ground: "p_dlc07_pavement_01", floor: "p_dlc07_pavement_02",
    accent: null,
    boundaryPreferred: ["p_dlc07_StandardWall", "p_dlc07_RestaurantWall", "p_dlc07_WideWall_01"],
    landmarkPool: ["p_dlc07_curtain_01", "p_dlc07_vent_01"],
    midPool: ["p_dlc7_city_bench_01", "p_dlc07_restaurant_carpet_01"],
    furniturePool: ["p_dlc7_city_bench_01"],
    detailPool: ["p_dlc07_vent_01"],
    streetPool: ["p_dlc7_city_bench_01", "p_dlc07_fire_hydrant_01"],
    wallDecorPool: ["p_dlc07_curtain_01"],
    backRow: ["p_dlc07_WideWall_01", "p_dlc07_RestaurantWall", "p_dlc07_StandardWall"],  // 边界后面的"街区"第二层
    backRowGap: 4,
  },
};

function compose(levelNo) {
  const cfg = LEVELS[levelNo];
  const T = cfg.theme;                 // layout 行里的主题目录
  const SK = cfg.sizeKey || cfg.theme; // 实测尺寸表里的主题 key
  const getSize = (_t, name) => SIZE[SK + "/" + name] || null;
  const FILE = `${ROOT}/layout_${levelNo}.tsv`;
  const rnd = mulberry32(cfg.seed);
  const grid = new Grid();
  const out = [];
  const art = (theme, name, x, z, y, dir, note) =>
    ["art", theme, name, String(y), String(x), String(z), note, String(dir)].join("\t");
  const ground = (theme, name) => {
    const s = getSize(theme, name);
    const y = Number.isFinite(s && s.minY) ? -s.minY : 0;
    return y;
  };
  const put = (name, x, z, opts = {}) => {
    const yaw = opts.yaw || 0;
    const s = getSize(T, name);
    if (!s) return false;
    const fp = grid.footprint(T, name, x, z, yaw);
    if (!grid.fits(fp)) return false;
    grid.take(fp);
    const y = opts.y !== undefined ? opts.y : ground(T, name);
    out.push(art(T, name, x, z, y, yaw, opts.note || "decor"));
    return true;
  };

  // ---- 读玩法行，拿房间范围 + 预留玩法占位 ----
  const lines = fs.readFileSync(FILE, "utf8").split(/\r?\n/).filter(l => l && l.split("\t")[0] !== "art");
  const rows = lines.filter(l => !l.startsWith("#")).map(l => l.split("\t"))
    .filter(f => f.length >= 6 && f[0] !== "kind");
  const solid = rows.filter(f => f[0] !== "spawn");
  const xs = solid.map(f => Number(f[4])), zs = solid.map(f => Number(f[5]));
  const X0 = Math.min(...xs), X1 = Math.max(...xs), Z0 = Math.min(...zs), Z1 = Math.max(...zs);
  const WZ = Z1 + 3;                                  // 边界线（台面外 1.8 m，避开预留净空）
  const FAR = 46;                                     // 边界延伸到画面外

  // 玩法物体占位（工位 2×2，外扩 1 格净空）
  for (const f of rows) {
    const x = Number(f[4]), z = Number(f[5]);
    grid.reserve(x, z, f[0] === "spawn" ? 1 : 2);
  }

  // ---- 1) 地面：整片铺，再叠室内地板，再切一条材质带 ----
  const szAny = (name) => {
    const key = (cfg.crossSize && cfg.crossSize[name]) || SK;
    return SIZE[key + "/" + name] || null;
  };
  const gs = cfg.ground ? szAny(cfg.ground) : null, fs2 = cfg.floor ? szAny(cfg.floor) : null;
  const step = (s) => Math.max(2, Math.round((s && s.w ? s.w : 2.4) / CELL));
  const gStep = step(gs), fStep = fs2 ? step(fs2) : 4;
  if (cfg.ground) {
    for (let x = X0 - 2 - 24; x <= X1 + 2 + 24; x += gStep) {
      for (let z = Z0 - 2 - 24; z <= WZ + 24; z += gStep) {
        out.push(art(T, cfg.ground, x, z, ground(T, cfg.ground), 0, "ground"));
      }
    }
  }
  if (cfg.floor) {
    for (let x = X0 - 4; x <= X1 + 4; x += fStep) {
      for (let z = Z0 - 4; z <= WZ + 2; z += fStep) {
        out.push(art(T, cfg.floor, x, z, 0.02, 0, "floor"));
      }
    }
  }
  if (cfg.accent && getSize(T, cfg.accent.tile)) {
    const aS = Math.max(2, Math.round(getSize(T, cfg.accent.tile).w / CELL));
    for (let x = X0 - 16; x <= X1 + 16; x += aS) {
      for (let z = Z0 + cfg.accent.zFrom; z <= Z0 + cfg.accent.zTo; z += aS) {
        out.push(art(T, cfg.accent.tile, x, z, 0.03, 0, "accent"));
      }
    }
  }

  // ---- 2) 边界：按资产实测宽度推进，高度可变化（不同墙件交替） ----
  const boundaryPool = (cfg.boundaryPreferred || []).filter(n => getSize(T, n));
  if (boundaryPool.length) {
    let x = X0 - FAR;
    while (x < X1 + FAR) {
      const name = boundaryPool[Math.floor(rnd() * boundaryPool.length) % boundaryPool.length];
      const sB = getSize(T, name);
      const wCells = Math.max(2, Math.round(sB.w / CELL));
      const dCells = Math.max(1, Math.round(sB.d / CELL));
      const bz = Z1 + 3 + Math.ceil(dCells / 2);   // 厚件往后站，完全避开台面净空
      const cx = x + wCells / 2;
      const fp = grid.footprint(T, name, cx, bz, 0);
      if (grid.fits(fp)) { grid.take(fp); out.push(art(T, name, cx, bz, ground(T, name), 0, "boundary")); }
      x += wCells;
    }
  }
  const boundaryH = boundaryPool.length ? Math.min(...boundaryPool.map(n => getSize(T, n).h)) : 2;

  // 2b) 边界之后的第二层：制造纵深（高低、宽窄都混）
  if (cfg.backRow && cfg.backRow.length) {
    let bx = X0 - FAR + 2;
    while (bx < X1 + FAR) {
      const name = cfg.backRow[Math.floor(rnd() * cfg.backRow.length)];
      const sB = getSize(T, name);
      const wCells = Math.max(2, Math.round(sB.w / CELL));
      const cx = bx + wCells / 2;
      const bz = WZ + (cfg.backRowGap || 4) + Math.round(rnd() * 2);
      const fp = grid.footprint(T, name, cx, bz, 0);
      if (grid.fits(fp)) { grid.take(fp); out.push(art(T, name, cx, bz, ground(T, name), 0, "back-row")); }
      bx += wCells + (rnd() < 0.3 ? wCells : 0);   // 偶尔留空档，别铺成一条直线
    }
  }

  // 边界上的装饰（贴内侧 1 格，朝屋）
  for (let i = 0; i < 3 && cfg.wallDecorPool.length; i++) {
    const name = cfg.wallDecorPool[Math.floor(rnd() * cfg.wallDecorPool.length)];
    const x = X0 + Math.round((i + 1) * (X1 - X0) / 4);
    put(name, x, WZ - 1, { yaw: 180, note: "boundary-decor" });
  }

  // ---- 3) 地标群：1 大 + 2~4 中 + 1~3 小，位置不对称 ----
  const landmarkCount = 2 + Math.floor(rnd() * 2);      // 2~3 群
  const anchors = [];
  for (let i = 0; i < landmarkCount; i++) {
    const side = i % 2 === 0 ? -1 : 1;
    let ax = 0, az = 0, big = null, ok = false;
    for (let tries = 0; tries < 8 && !ok; tries++) {
      ax = side < 0 ? X0 - 10 - Math.round(rnd() * 14) : X1 + 10 + Math.round(rnd() * 14);
      az = Z0 - 4 + Math.round(rnd() * (Z1 - Z0 + 8));
      big = cfg.landmarkPool[Math.floor(rnd() * cfg.landmarkPool.length)];
      ok = put(big, ax, az, { note: "landmark" });
    }
    if (ok) {
      anchors.push([ax, az]);
      const mids = 2 + Math.floor(rnd() * 3);
      for (let k = 0; k < mids; k++) {
        const name = cfg.midPool[Math.floor(rnd() * cfg.midPool.length)];
        const ang = rnd() * Math.PI * 2, r = 4 + rnd() * 6;
        put(name, Math.round(ax + Math.cos(ang) * r), Math.round(az + Math.sin(ang) * r), { note: "landmark-mid" });
      }
      const dets = 1 + Math.floor(rnd() * 3);
      for (let k = 0; k < dets; k++) {
        const name = cfg.detailPool[Math.floor(rnd() * cfg.detailPool.length)];
        const ang = rnd() * Math.PI * 2, r = 2 + rnd() * 4;
        const yaw = [0, 90, 180, 270][Math.floor(rnd() * 4)];
        put(name, Math.round(ax + Math.cos(ang) * r), Math.round(az + Math.sin(ang) * r), { note: "landmark-detail", yaw });
      }
    }
  }

  // ---- 4) 家具带：左右环带各 2 排，间距抖动、型号交替 ----
  const rowsX = [X0 - 9, X0 - 18, X1 + 9, X1 + 18];
  rowsX.forEach((rx, idx) => {
    let z = Z0 - 3 + Math.floor(rnd() * 3);
    while (z <= WZ - 4) {
      const name = cfg.furniturePool[Math.floor(rnd() * cfg.furniturePool.length)];
      const s = getSize(T, name);
      if (s) {
        // 主排被地标占了就横移几格再试，别整段空掉
        let placed = false;
        for (const dx of [0, 1, -2, 3, -4]) {
          if (put(name, rx + dx, z, { note: "furniture", yaw: idx < 2 ? 0 : 180 })) { placed = true; break; }
        }
        if (placed && cfg.detailPool.length) {
          const dname = cfg.detailPool[Math.floor(rnd() * cfg.detailPool.length)];
          // 目标朝向 -> yaw：北(+z) / 南(-z) / 东(+x) / 西(-x)
          const faceYaw = (want) => {
            const front = cfg.chairFront || "z";   // 模型默认朝向
            const base = { z: { N: 0, S: 180, E: 90, W: 270 }, x: { N: 90, S: 270, E: 0, W: 180 } }[front];
            return base[want];
          };
          const seats = [
            [rx, z - 3, faceYaw("N")], [rx, z + 3, faceYaw("S")],   // 南北两侧，正对桌子
            [rx - 3, z, faceYaw("E")], [rx + 3, z, faceYaw("W")],   // 东西两侧
          ];
          for (const [px, pz, py] of seats) {
            put(dname, px, pz, { note: "furniture-side", yaw: py });
          }
        }
      }
      z += 7 + Math.floor(rnd() * 4);                  // 5.4~8.4 m 抖动
    }
  });

  // ---- 5) 街景：边界南侧、画面边缘 ----
  for (let i = 0; i < 4; i++) {
    const name = cfg.streetPool[Math.floor(rnd() * cfg.streetPool.length)];
    const x = (i % 2 === 0 ? X0 - 12 - Math.round(rnd() * 8) : X1 + 12 + Math.round(rnd() * 8));
    const z = Z0 - 6 - Math.round(rnd() * 10);
    put(name, x, z, { note: "street" });
  }

  // ---- 6) 边界之外：只放比边界高的东西（遮挡规则） ----
  for (let i = 0; i < 3; i++) {
    const name = cfg.streetPool[Math.floor(rnd() * cfg.streetPool.length)];
    const s = getSize(T, name);
    if (!s || s.h <= boundaryH) continue;             // 被墙挡住，不放
    const x = X0 + Math.round(rnd() * (X1 - X0));
    put(name, x, WZ + 4 + Math.round(rnd() * 4), { note: "beyond-boundary" });
  }

  fs.writeFileSync(FILE, lines.concat(out).join("\n"), "utf8");
  const counts = {};
  for (const l of out) { const k = l.split("\t")[6]; counts[k] = (counts[k] || 0) + 1; }
  const pool = {
    boundary: poolOf(SK, "boundary").length,
    landmark: poolOf(SK, "landmark").length,
    furniture: poolOf(SK, "furniture").length,
    detail: poolOf(SK, "detail").length,
    flat: poolOf(SK, "flat").length,
  };
  console.log(`L${levelNo} ${cfg.name}（${T}） 房间 x${X0}..${X1} z${Z0}..${Z1} 边界z=${WZ} 资产池=${JSON.stringify(pool)}`);
  console.log(`   生成: ${JSON.stringify(counts)}  地标群=${anchors.length}`);
}

const args = process.argv.slice(2).map(Number).filter(Boolean);
for (const n of (args.length ? args : [2, 3, 4])) compose(n);
