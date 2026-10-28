// WellCooked 控制台前端：原生 JS，SSE 收快照，所有文本走 textContent，不拼 HTML。
"use strict";
const $ = (id) => document.getElementById(id);
const ORDER_LABEL = { fcfs: "FCFS 先来先做", sjf: "SJF 最短优先", llf: "LLF 最小松弛", hrrn: "HRRN 响应比", edf: "EDF 最早截止" };
const TASK_LABEL = { cost: "cost 代价模型", fifo: "fifo 按顺序", cp: "cp 关键路径" };
const lifetimes = new Map(); // 订单 id -> 见过的最大剩余秒数，当作总时长画计时条

function el(tag, props, ...kids) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === undefined || v === null) continue;
    if (k === "class") node.className = v;
    else if (k === "text") node.textContent = v;
    else if (k === "style") node.style.cssText = v;
    else node.setAttribute(k, v);
  }
  for (const kid of kids) if (kid !== null && kid !== undefined) node.append(kid);
  return node;
}
function fill(parent, nodes, emptyText) {
  parent.replaceChildren(...(nodes.length ? nodes : [el("li", { class: "empty", text: emptyText })]));
}
const fmtS = (ms) => (ms === undefined || ms === null ? "-" : (ms / 1000).toFixed(1) + "s");
function fmtClock(sec) {
  if (sec === undefined || sec === null) return "--:--";
  const s = Math.max(0, Math.round(sec));
  return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
}
function toast(text, isErr) {
  const t = $("toast");
  t.textContent = text;
  t.classList.toggle("err", !!isErr);
}
async function api(method, path, body) {
  const res = await fetch(path, {
    method, headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || "HTTP " + res.status);
  return data;
}

// ---------- 选项与标签页 ----------
async function loadConfig() {
  const cfg = await api("GET", "/api/config");
  const scene = $("f-scene");
  scene.replaceChildren(...cfg.scenes.map((s) => el("option", { value: s, text: s })));
  const demo = cfg.scenes.find((s) => s.includes("demo_3")) || cfg.scenes[0];
  if (demo) scene.value = demo;
  for (const sel of document.querySelectorAll(".sel-order"))
    sel.replaceChildren(...cfg.orders.map((o) => el("option", { value: o, text: ORDER_LABEL[o] || o })));
  for (const sel of document.querySelectorAll(".sel-task"))
    sel.replaceChildren(...cfg.tasks.map((t) => el("option", { value: t, text: TASK_LABEL[t] || t })));
}
function showTab(which) {
  for (const name of ["live", "runs"]) {
    const on = name === which;
    $("tab-" + name).setAttribute("aria-selected", String(on));
    $("view-" + name).hidden = !on;
  }
  if (which === "runs") loadRuns();
}
// ---------- 实时面板 ----------
function renderTickets(orders) {
  $("order-count").textContent = String(orders.length);
  fill($("tickets"), orders.map((o) => {
    const left = Number(o.remaining_s) || 0;
    const total = Math.max(lifetimes.get(o.id) || 0, left, 1);
    lifetimes.set(o.id, total);
    const ratio = Math.max(0, Math.min(1, left / total));
    const level = ratio < 0.25 ? " low" : ratio < 0.5 ? " mid" : "";
    const ings = el("div", { class: "ings" }, ...(o.ingredients || []).map((i) => el("span", { class: "ing", text: i })));
    return el("li", { class: "ticket", "aria-label": `${o.dish}，剩 ${Math.round(left)} 秒` },
      el("div", { class: "dish", text: o.dish || o.id }),
      el("div", { class: "meta" }, el("span", { text: o.id }), el("span", { text: o.score != null ? "★" + o.score : "" })),
      ings,
      el("div", { class: "timer" + level, role: "progressbar", "aria-valuemin": "0", "aria-valuemax": String(Math.round(total)),
        "aria-valuenow": String(Math.round(left)), "aria-label": "剩余时间" }, el("i", { style: `width:${(ratio * 100).toFixed(1)}%` })),
      el("div", { class: "meta" }, el("span", { text: "剩余" }), el("span", { text: left.toFixed(1) + "s" })));
  }), "暂时没有订单");
}
function renderChefs(chefs) {
  fill($("chefs"), chefs.map((c) => {
    const n = (Number(c.id) || 0) + 1;
    const hold = c.holding ? c.holding + (c.holding_contents && c.holding_contents.length ? "（" + c.holding_contents.join("+") + "）" : "") : "空手";
    return el("li", { class: "chef" },
      el("span", { class: "badge p" + Math.min(n, 4), text: "P" + n }),
      el("span", { class: "line1", text: `${c.state || "-"} · 朝 ${c.facing || "-"}` }),
      el("span", { class: "line2", text: `手上：${hold} · (${fmtNum(c.x)}, ${fmtNum(c.z)})` }));
  }), "还没有厨师数据");
}
const fmtNum = (v) => (typeof v === "number" ? v.toFixed(1) : "-");
function renderStations(stations) {
  fill($("stations"), stations.map((s) => {
    const burn = s.burning || s.fire;
    const busy = s.item || s.chef_id !== undefined && s.chef_id !== null && s.chef_id >= 0;
    const bits = [];
    if (s.state) bits.push(s.state);
    if (s.item) bits.push("物品 " + s.item);
    if (s.contents && s.contents.length) bits.push("内含 " + s.contents.join("+"));
    if (s.step) bits.push("步骤 " + s.step);
    if (s.count !== undefined && s.count !== null) bits.push("数量 " + s.count);
    if (s.chef_id !== undefined && s.chef_id !== null && s.chef_id >= 0) bits.push("P" + (s.chef_id + 1) + " 在用");
    if (s.cooked) bits.push("已熟");
    if (typeof s.time_left === "number") bits.push("剩 " + s.time_left.toFixed(1) + "s");
    if (typeof s.burn_in === "number") bits.push(s.burn_in.toFixed(1) + "s 后糊");
    if (s.burning) bits.push("正在糊");
    if (s.fire) bits.push("着火");
    const li = el("li", { class: "station" + (burn ? " burn" : busy ? " busy" : "") },
      el("div", { class: "kind", text: `${s.id}（${s.kind}）` }), el("div", { text: bits.join(" · ") || "空闲" }));
    if (typeof s.progress === "number") {
      const p = Math.max(0, Math.min(1, s.progress));
      li.append(el("div", { class: "prog", role: "progressbar", "aria-valuemin": "0", "aria-valuemax": "100",
        "aria-valuenow": String(Math.round(p * 100)), "aria-label": s.id + " 进度" }, el("i", { style: `width:${p * 100}%` })));
    }
    return li;
  }), "还没有工位数据");
}
const SVG = "http://www.w3.org/2000/svg";
function svg(tag, attrs, text) {
  const n = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  if (text !== undefined) n.textContent = text;
  return n;
}
function renderMap(chefs, stations) {
  const pts = [...stations.filter((s) => typeof s.x === "number"), ...chefs.filter((c) => typeof c.x === "number")];
  const map = $("map");
  if (!pts.length) { map.replaceChildren(svg("text", { x: 50, y: 30, "text-anchor": "middle" }, "等待坐标数据")); return; }
  const xs = pts.map((p) => p.x), zs = pts.map((p) => p.z);
  const minX = Math.min(...xs), maxX = Math.max(...xs), minZ = Math.min(...zs), maxZ = Math.max(...zs);
  const sx = 90 / Math.max(maxX - minX, 1), sz = 50 / Math.max(maxZ - minZ, 1), k = Math.min(sx, sz);
  const px = (x) => 5 + (x - minX) * k, pz = (z) => 5 + (maxZ - z) * k; // z 轴朝上
  const nodes = [];
  for (const s of stations) {
    if (typeof s.x !== "number") continue;
    const fillc = s.burning || s.fire ? "var(--red)" : s.item ? "var(--yellow)" : "#c98a52";
    nodes.push(svg("rect", { class: "st", x: px(s.x) - 2, y: pz(s.z) - 2, width: 4, height: 4, rx: 1, fill: fillc }));
    nodes.push(svg("title", {}, `${s.id} ${s.kind}`));
  }
  const colors = ["var(--p1)", "var(--p2)", "var(--p3)", "var(--p4)"];
  for (const c of chefs) {
    if (typeof c.x !== "number") continue;
    const n = (Number(c.id) || 0);
    nodes.push(svg("circle", { cx: px(c.x), cy: pz(c.z), r: 2.6, fill: colors[n % 4], stroke: "#111", "stroke-width": 0.5 }));
    nodes.push(svg("text", { x: px(c.x), y: pz(c.z) + 0.8, "text-anchor": "middle", fill: n % 4 === 3 ? "#111" : "#fff" }, "P" + (n + 1)));
  }
  map.replaceChildren(...nodes);
}
function renderProcs(status) {
  const kids = (status && status.children) || [];
  $("procs").replaceChildren(...(kids.length ? kids.map((c) => el("tr", {},
    el("td", { text: c.role }), el("td", { text: c.pid ? String(c.pid) : "-" }),
    el("td", { class: c.state === "running" ? "ok-text" : "bad-text", text: c.state }),
    el("td", { text: String(c.spawns) }),
    el("td", { text: c.exits ? `${c.exits}（${c.last_signal ? "sig " + c.last_signal : "code " + c.last_code}）` : "0" })))
    : [el("tr", {}, el("td", { colspan: "5", class: "empty", text: "Supervisor 还没起" }))]));
}
function renderMetrics(m) {
  const box = $("metrics");
  if (!m) { box.replaceChildren(el("p", { class: "empty", text: "还没有 metrics.json" })); return; }
  const o = m.orders || {};
  const rows = [["订单总数", o.total], ["已完成", o.completed], ["失败", o.failed], ["进行中", o.active],
    ["Worker 重启", m.worker_restarts], ["统计时刻", fmtS(m.t_ms)]];
  for (const [k, v] of Object.entries(m.dispatch || {})) rows.push(["派工 " + k.toUpperCase(), v]);
  for (const r of m.resources || []) rows.push([`${r.kind} 利用率`, ((r.utilization_bp || 0) / 100).toFixed(1) + "%（等 " + fmtS(r.wait_ms) + "）"]);
  box.replaceChildren(el("div", { class: "metric-grid" },
    ...rows.flatMap(([k, v]) => [el("span", { text: k }), el("b", { text: v === undefined || v === null ? "-" : String(v) })])));
}
function feedItem(t, text, cls) {
  return el("li", { class: cls }, el("span", { class: "t", text: fmtS(t) }), text);
}
function decisionText(d) {
  if (d.kind === "strategy") return `切换策略 → ${d.policy} / ${d.task_policy || "-"}`;
  const parts = [d.kind, d.policy, d.order_id, d.node, d.chef_id !== undefined ? "P" + (d.chef_id + 1) : null,
    d.station, d.cost !== undefined ? "cost " + d.cost : null, d.reason];
  return parts.filter((p) => p !== undefined && p !== null && p !== "").join(" · ");
}
function eventText(e) {
  const skip = new Set(["t_ms", "event"]);
  const rest = Object.entries(e).filter(([k]) => !skip.has(k)).map(([k, v]) => `${k}=${typeof v === "object" ? JSON.stringify(v) : v}`);
  return `${e.event || "?"} ${rest.join(" ")}`;
}
function renderFeeds(f) {
  fill($("decisions"), (f.decisions || []).slice().reverse().map((d) =>
    feedItem(d.t_ms, decisionText(d), d.kind === "strategy" ? "ok-text" : "")), "还没有决策");
  const notes = (f.notes || []).map((n) => ({ t_ms: null, text: "控制台：" + n.text, at: n.t }));
  fill($("events"), [...notes.reverse().map((n) => feedItem(null, n.text, "ok-text")),
    ...(f.events || []).slice().reverse().map((e) => feedItem(e.t_ms, eventText(e)))], "还没有事件");
  const results = (f.live && f.live.ok && f.live.results) || [];
  fill($("results"), results.slice().reverse().map((r) => feedItem(r.t_ms,
    `${r.dish || r.id} ${r.outcome}${r.score != null ? " ★" + r.score : ""}`,
    /deliver|complete|success/i.test(r.outcome || "") ? "ok-text" : "bad-text")), "还没有结束的订单");
  $("output").textContent = (f.output || []).join("\n");
}
let running = false;
function onFrame(f) {
  running = !!f.running;
  const conn = $("conn");
  if (f.error) { conn.textContent = "读盘出错：" + f.error; conn.className = "pill bad"; }
  else if (running) { conn.textContent = "对局进行中" + (f.runtime ? " · " + f.runtime : ""); conn.className = "pill ok"; }
  else { conn.textContent = f.exit_code !== null && f.exit_code !== undefined ? `已结束（退出码 ${f.exit_code}）` : "空闲"; conn.className = "pill idle"; }
  $("btn-start").disabled = running;
  $("btn-stop").disabled = !running;
  $("btn-kill").disabled = !running;
  for (const b of $("switch-form").querySelectorAll("button, select")) b.disabled = !running;
  const live = f.live && f.live.ok ? f.live : null;
  $("score").textContent = live && live.score != null ? String(live.score) : "0";
  const left = live && live.timer ? live.timer.left_s : null;
  $("clock").textContent = fmtClock(left);
  $("clock").parentElement.classList.toggle("low", left !== null && left < 30);
  const p = f.policy || {};
  $("now-order").textContent = p.order || (f.params ? f.params.strategy.toUpperCase() : "-");
  $("now-task").textContent = p.task || (f.params ? f.params.task_policy : "-");
  renderTickets(live ? live.orders || [] : []);
  renderChefs(live ? live.chefs || [] : []);
  renderStations(live ? live.stations || [] : []);
  renderMap(live ? live.chefs || [] : [], live ? live.stations || [] : []);
  renderProcs(f.status);
  renderMetrics(f.metrics);
  renderFeeds(f);
}
function connect() {
  const es = new EventSource("/api/stream");
  es.onmessage = (ev) => { try { onFrame(JSON.parse(ev.data)); } catch (e) { toast("快照解析失败：" + e.message, true); } };
  es.onerror = () => { $("conn").textContent = "和控制台断开，重连中…"; $("conn").className = "pill bad"; };
}
// ---------- 控制 ----------
async function act(fn, okText) {
  try { const r = await fn(); toast(typeof okText === "function" ? okText(r) : okText); }
  catch (e) { toast(e.message, true); }
}
$("run-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const d = Object.fromEntries(new FormData(ev.target));
  d.chefs = Number(d.chefs); d.game_seconds = Number(d.game_seconds);
  d.continuous = ev.target.elements.continuous.checked;
  lifetimes.clear();
  act(() => api("POST", "/api/run", d), "已开局，游戏窗口会自己弹出");
});
$("btn-stop").addEventListener("click", () => act(() => api("POST", "/api/stop"), "已请求停止"));
$("switch-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const d = Object.fromEntries(new FormData(ev.target));
  act(() => api("POST", "/api/strategy", d), (r) => "已发送：" + r.request);
});
$("btn-kill").addEventListener("click", () => {
  if (!confirm("确定 kill -9 Scheduler？Supervisor 会重启它。")) return;
  act(() => api("POST", "/api/kill-scheduler"), (r) => `已 kill pid ${r.killed}`);
});

// ---------- 结果对比 ----------
async function loadRuns() {
  let runs;
  try { runs = await api("GET", "/api/runs"); } catch (e) { toast(e.message, true); return; }
  const max = Math.max(1, ...runs.map((r) => r.score || 0));
  $("runs-chart").replaceChildren(...runs.slice(0, 20).map((r) => el("div", { class: "bar" },
    el("span", { text: `${r.order || "?"}/${r.task || "?"} · ${r.name}` }),
    el("div", { class: "track" }, el("div", { class: "fill", style: `width:${((r.score || 0) / max) * 100}%` })),
    el("b", { text: r.score != null ? String(r.score) : "-" }))));
  const tbody = $("runs");
  if (!runs.length) { tbody.replaceChildren(el("tr", {}, el("td", { colspan: "9", class: "empty", text: "还没有跑完的对局" }))); return; }
  tbody.replaceChildren(...runs.map((r) => {
    const tr = el("tr", { tabindex: "0", "aria-selected": "false" },
      el("td", { text: r.name }), el("td", { text: r.order || "-" }), el("td", { text: r.task || "-" }),
      el("td", { text: r.dispatch || "-" }), el("td", { text: r.chefs != null ? String(r.chefs) : "-" }),
      el("td", { text: r.score != null ? String(r.score) : "-" }),
      el("td", { text: r.arrived != null ? `${r.completed}/${r.arrived}` : "-" }),
      el("td", { text: r.avg_turnaround_s != null ? String(r.avg_turnaround_s) : "-" }),
      el("td", { class: r.result === "PASS" ? "ok-text" : "bad-text", text: r.result || "-" }));
    const open = () => showRun(r.name, tr);
    tr.addEventListener("click", open);
    tr.addEventListener("keydown", (ev) => { if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); open(); } });
    return tr;
  }));
}
async function showRun(name, tr) {
  for (const row of $("runs").children) row.setAttribute("aria-selected", String(row === tr));
  $("run-name").textContent = name;
  try { const d = await api("GET", "/api/runs/" + encodeURIComponent(name)); $("run-summary").textContent = d.summary || "（没有 summary.txt）"; }
  catch (e) { $("run-summary").textContent = e.message; }
}
$("btn-refresh").addEventListener("click", loadRuns);
$("tab-live").addEventListener("click", () => showTab("live"));
$("tab-runs").addEventListener("click", () => showTab("runs"));

loadConfig().catch((e) => toast("读配置失败：" + e.message, true)).finally(connect);
