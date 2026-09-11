"use strict";

// ---- state ---------------------------------------------------------------

const state = {
  values: new Map(),   // "node/Endpoint" -> {node, endpoint, value, ts}
  nodes: [],           // from /api/nodes
  main: null,
  uplinkUp: false,
  floors: [],
  placements: [],
};

const WRITABLE = ["RoomSetpoint", "DamperTarget", "DamperMode", "RoomMode", "SystemControl"];
const MODE_ENUM = { 0: "closed", 1: "open", 2: "auto", 3: "manual" };

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const key = (node, ep) => `${node}/${ep}`;

function api(path, opts) {
  return fetch(path, opts).then(async (r) => {
    const body = r.headers.get("content-type")?.includes("json") ? await r.json() : await r.text();
    if (!r.ok) throw new Error((body && body.error) || r.statusText);
    return body;
  });
}

// ---- websocket ----------------------------------------------------------

let ws;
function connectWS() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onmessage = (ev) => handleEvent(JSON.parse(ev.data));
  ws.onclose = () => {
    setLinkState(false);
    setTimeout(connectWS, 2000);
  };
}

function handleEvent(msg) {
  switch (msg.type) {
    case "snapshot":
      state.values.clear();
      for (const v of msg.values || []) state.values.set(key(v.node, v.endpoint), v);
      state.main = msg.main && msg.main.type ? msg.main : state.main;
      state.uplinkUp = msg.uplinkUp;
      setLinkState(msg.uplinkUp);
      refreshCurrentView();
      break;
    case "value":
      state.values.set(key(msg.node, msg.endpoint), msg);
      onValue(msg);
      break;
    case "presence":
      loadNodes().then(refreshCurrentView);
      if (currentView() === "firmware") loadFirmware();
      break;
    case "thermostat":
      if (currentView() === "firmware") loadFirmware();
      break;
    case "main":
      state.main = msg;
      state.uplinkUp = msg.online;
      setLinkState(msg.online);
      // Master up/down flips every node between online and offline.
      loadNodes().then(refreshCurrentView).catch(() => {});
      if (currentView() === "status") renderMainStatus();
      break;
    case "ota":
      if (currentView() === "firmware") loadFirmware();
      break;
  }
}

function setLinkState(up) {
  const el = $("#link-state");
  el.textContent = up ? "uplink up" : "uplink down";
  el.className = "pill " + (up ? "up" : "down");
}

// ---- router ------------------------------------------------------------

const views = {
  map: { render: renderMap },
  overrides: { render: renderOverrides },
  status: { render: renderStatus },
  firmware: { render: renderFirmware },
  setup: { render: renderSetup },
};

function currentView() {
  return (location.hash.replace("#/", "") || "map").split("/")[0];
}
function refreshCurrentView() {
  const v = views[currentView()] || views.map;
  v.render();
}
function route() {
  const name = currentView();
  for (const a of $$("nav a")) a.classList.toggle("active", a.dataset.view === name);
  for (const sec of $$(".view")) sec.hidden = sec.id !== `view-${name}`;
  refreshCurrentView();
}
window.addEventListener("hashchange", route);

// ---- data loads ------------------------------------------------------

async function loadNodes() {
  state.nodes = (await api("/api/nodes")) || [];
}
async function loadFloors() {
  state.floors = (await api("/api/floors")) || [];
}
async function loadPlacements() {
  state.placements = (await api("/api/placements")) || [];
}

// ---- map view -------------------------------------------------------

function tempColor(c) {
  // piecewise blue(16) -> green(21) -> red(26)
  const stops = [
    [16, [47, 111, 219]],
    [21, [63, 157, 90]],
    [26, [209, 72, 58]],
  ];
  if (c <= stops[0][0]) return rgb(stops[0][1]);
  if (c >= stops[2][0]) return rgb(stops[2][1]);
  const seg = c < stops[1][0] ? [stops[0], stops[1]] : [stops[1], stops[2]];
  const t = (c - seg[0][0]) / (seg[1][0] - seg[0][0]);
  return rgb(seg[0][1].map((v, i) => Math.round(v + t * (seg[1][1][i] - v))));
}
const rgb = ([r, g, b]) => `rgb(${r} ${g} ${b})`;

async function renderMap() {
  await Promise.all([loadFloors(), loadPlacements(), loadNodes().catch(() => {})]);
  const sel = $("#floor-select");
  const prev = sel.value;
  sel.innerHTML = state.floors.map((f) => `<option value="${f.id}">${esc(f.name)}</option>`).join("");
  if (prev) sel.value = prev;

  $("#map-empty").hidden = state.floors.length > 0;
  $("#map-stage").hidden = state.floors.length === 0;
  if (!state.floors.length) return;

  drawFloor(sel.value, $("#floor-img"), $("#map-overlay"), false);
}
$("#floor-select").addEventListener("change", () =>
  drawFloor($("#floor-select").value, $("#floor-img"), $("#map-overlay"), false)
);

function drawFloor(floorId, img, svg, editable) {
  const floor = state.floors.find((f) => String(f.id) === String(floorId));
  if (!floor) return;
  img.src = `/api/floors/${floor.id}/image`;
  svg.setAttribute("viewBox", `0 0 ${floor.widthPx} ${floor.heightPx}`);
  paintOverlay(svg, floor, editable);
}

function paintOverlay(svg, floor, editable) {
  const r = Math.max(floor.widthPx, floor.heightPx) * 0.06;
  const selectedNode = editable ? parseInt($("#setup-node-select").value, 10) : null;
  let defs = `<defs>`;
  let blobs = "";
  let markers = "";

  for (const p of state.placements.filter((p) => p.floorId === floor.id)) {
    const v = state.values.get(key(p.nodeId, "RoomTemp"));
    const set = state.values.get(key(p.nodeId, "RoomSetpoint"));
    const temp = v && v.value.kind === "number" ? v.value.num : null;
    const col = temp == null ? "var(--muted)" : tempColor(temp);
    const gid = `g${p.nodeId}`;
    defs += `<radialGradient id="${gid}">
      <stop offset="0%" stop-color="${col}" stop-opacity="0.55"/>
      <stop offset="100%" stop-color="${col}" stop-opacity="0"/>
    </radialGradient>`;
    if (p.polyJson) {
      try {
        const pts = JSON.parse(p.polyJson).map((pt) => pt.join(",")).join(" ");
        blobs += `<polygon points="${pts}" fill="${col}" fill-opacity="0.28"/>`;
      } catch {}
    } else {
      blobs += `<circle cx="${p.xPx}" cy="${p.yPx}" r="${r}" fill="url(#${gid})"/>`;
    }
    const label = temp == null ? "—" : `${temp.toFixed(1)}°`;
    const sub = set && set.value.kind === "number" ? `set ${set.value.num.toFixed(1)}°` : `node ${p.nodeId}`;
    const selected = p.nodeId === selectedNode;
    markers += `<g data-node="${p.nodeId}">
      ${selected ? `<circle class="node-halo" cx="${p.xPx}" cy="${p.yPx}" r="6"/>` : ""}
      <circle class="node-dot${selected ? " selected" : ""}" cx="${p.xPx}" cy="${p.yPx}" r="6"/>
      <text class="node-label" x="${p.xPx + 12}" y="${p.yPx - 2}">${label}</text>
      <text class="node-sub" x="${p.xPx + 12}" y="${p.yPx + 14}">${esc(sub)}</text>
    </g>`;
  }
  defs += `</defs>`;

  let draftMarkup = "";
  if (editable && draft && draft.floorId === floor.id) {
    if (draft.points && draft.points.length) {
      const ptsAttr = draft.points.map(([x, y]) => `${x},${y}`).join(" ");
      draftMarkup +=
        draft.mode === "manual"
          ? `<polyline points="${ptsAttr}" class="draft-outline"/>`
          : `<polygon points="${ptsAttr}" class="draft-outline draft-outline-fill"/>`;
      for (const [x, y] of draft.points) {
        draftMarkup += `<circle class="draft-vertex" cx="${x}" cy="${y}" r="4"/>`;
      }
    } else if (draft.seed) {
      draftMarkup += `<circle class="draft-vertex" cx="${draft.seed.x}" cy="${draft.seed.y}" r="5"/>`;
    }
  }
  svg.innerHTML = defs + blobs + markers + draftMarkup;

  if (editable) {
    svg.onclick = (ev) => {
      const nodeSel = $("#setup-node-select");
      const nodeId = parseInt(nodeSel.value, 10);
      if (!nodeId) return;
      const pt = svgPoint(svg, ev);
      const mode = $("#setup-mode-select").value;
      if (mode === "flood") {
        startFloodDraft(nodeId, floor, svg, pt);
        return;
      }
      if (mode === "manual") {
        addManualPoint(nodeId, floor, svg, pt);
        return;
      }
      api(`/api/placements/${nodeId}`, {
        method: "PUT",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ floorId: floor.id, xPx: Math.round(pt.x), yPx: Math.round(pt.y) }),
      }).then(() => loadPlacements()).then(() => paintOverlay(svg, floor, true));
    };
  }
}

function svgPoint(svg, ev) {
  const rect = svg.getBoundingClientRect();
  const vb = svg.viewBox.baseVal;
  return {
    x: ((ev.clientX - rect.left) / rect.width) * vb.width,
    y: ((ev.clientY - rect.top) / rect.height) * vb.height,
  };
}

// ---- room outline editor (flood fill + manual polygon) ---------------

let draft = null; // { mode: "flood"|"manual", nodeId, floorId, seed?, points, error?, warning? }
let floodWorkspace = null; // cached downsampled floor-plan pixels for flood fill

function startFloodDraft(nodeId, floor, svg, pt) {
  draft = {
    mode: "flood",
    nodeId,
    floorId: floor.id,
    seed: { x: Math.round(pt.x), y: Math.round(pt.y) },
    points: null,
    error: null,
    warning: null,
  };
  recomputeFloodDraft(floor, svg);
}

function recomputeFloodDraft(floor, svg) {
  if (!draft || draft.mode !== "flood" || !draft.seed) return;
  const tolerance = parseInt($("#setup-tolerance").value, 10);
  let result;
  try {
    result = computeFloodPolygon(floor, draft.seed, tolerance);
  } catch {
    result = { error: "Could not read the floor plan image — wait for it to finish loading and try again." };
  }
  draft.points = result.points || null;
  draft.error = result.error || null;
  draft.warning = result.warning || null;
  updateOutlineBar();
  paintOverlay(svg, floor, true);
}

function addManualPoint(nodeId, floor, svg, pt) {
  if (!draft || draft.mode !== "manual" || draft.nodeId !== nodeId || draft.floorId !== floor.id) {
    draft = { mode: "manual", nodeId, floorId: floor.id, points: [] };
  }
  draft.points.push([Math.round(pt.x), Math.round(pt.y)]);
  updateOutlineBar();
  paintOverlay(svg, floor, true);
}

async function acceptDraft(svg) {
  if (!draft || !draft.points || draft.points.length < 3) return;
  const { nodeId, floorId, points, seed } = draft;
  const cx = seed ? seed.x : Math.round(points.reduce((s, p) => s + p[0], 0) / points.length);
  const cy = seed ? seed.y : Math.round(points.reduce((s, p) => s + p[1], 0) / points.length);
  await api(`/api/placements/${nodeId}`, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ floorId, xPx: cx, yPx: cy, polyJson: JSON.stringify(points) }),
  });
  draft = null;
  await loadPlacements();
  updateOutlineBar();
  const floor = state.floors.find((f) => f.id === floorId);
  if (floor) paintOverlay(svg, floor, true);
}

function cancelDraft(svg) {
  const floor = draft ? state.floors.find((f) => f.id === draft.floorId) : null;
  draft = null;
  updateOutlineBar();
  if (floor) paintOverlay(svg, floor, true);
}

function updateOutlineBar() {
  const mode = $("#setup-mode-select").value;
  $("#setup-outline-bar").hidden = mode === "point";
  $("#setup-tolerance-label").hidden = mode !== "flood";
  $("#setup-outline-undo").hidden = !(mode === "manual" && draft && draft.points.length > 0);
  $("#setup-outline-finish").hidden = !(mode === "manual" && draft);
  $("#setup-outline-finish").disabled = !(draft && draft.points && draft.points.length >= 3);
  $("#setup-outline-accept").hidden = !(mode === "flood" && draft && draft.points);
  $("#setup-outline-manual").hidden = !(mode === "flood" && draft);
  $("#setup-outline-cancel").hidden = !draft;

  const msg = $("#setup-outline-msg");
  if (draft && draft.error) {
    msg.className = "msg err";
    msg.textContent = draft.error;
  } else if (draft && draft.warning) {
    msg.className = "msg err";
    msg.textContent = draft.warning;
  } else if (mode === "flood" && !draft) {
    msg.className = "msg";
    msg.textContent = "click inside a room to flood-fill it";
  } else if (mode === "manual" && draft) {
    msg.className = "msg";
    msg.textContent = `${draft.points.length} point${draft.points.length === 1 ? "" : "s"} — click to add more, then Finish`;
  } else {
    msg.className = "msg";
    msg.textContent = "";
  }
}

// -- flood fill: seed pixel -> tolerant region -> traced, simplified polygon --

// Kept high on purpose: flood fill runs at (near) the floor plan's native
// resolution whenever possible, because downsampling a thin wall line can
// skip it entirely and let the fill leak into the next room. Only images
// bigger than this get scaled down, and then with smoothing on (not nearest-
// neighbor) so a thin line survives as a blended-but-distinct color instead
// of vanishing between samples.
const FLOOD_MAX_PIXELS = 6_000_000;

function getFloodWorkspace(floor) {
  if (floodWorkspace && floodWorkspace.floorId === floor.id) return floodWorkspace;
  const img = $("#setup-img");
  const nativeW = floor.widthPx, nativeH = floor.heightPx;
  const totalPx = nativeW * nativeH;
  const scale = totalPx > FLOOD_MAX_PIXELS ? Math.sqrt(FLOOD_MAX_PIXELS / totalPx) : 1;
  const w = Math.max(1, Math.round(nativeW * scale));
  const h = Math.max(1, Math.round(nativeH * scale));
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  if (scale < 1) ctx.imageSmoothingQuality = "high";
  else ctx.imageSmoothingEnabled = false;
  ctx.drawImage(img, 0, 0, w, h);
  const data = ctx.getImageData(0, 0, w, h).data;
  floodWorkspace = { floorId: floor.id, width: w, height: h, scale, data };
  return floodWorkspace;
}

function floodFillMask(data, width, height, sx, sy, tolerance) {
  const n = width * height;
  const mask = new Uint8Array(n);
  const stack = new Int32Array(n);
  let sp = 0;
  const seedIdx = sy * width + sx;
  const si = seedIdx * 4;
  const sr = data[si], sg = data[si + 1], sb = data[si + 2];
  mask[seedIdx] = 1;
  stack[sp++] = seedIdx;
  let count = 1;
  let touchesEdge = sx === 0 || sy === 0 || sx === width - 1 || sy === height - 1;

  // Per-channel cap (Chebyshev distance), not Euclidean: a wall that differs
  // from the floor in only one channel (e.g. a bluish line) must still be
  // caught, so no single channel may drift more than `tolerance`.
  const tryAdd = (nx, ny) => {
    const mi = ny * width + nx;
    if (mask[mi]) return;
    const i = mi * 4;
    const dr = Math.abs(data[i] - sr), dg = Math.abs(data[i + 1] - sg), db = Math.abs(data[i + 2] - sb);
    if (dr <= tolerance && dg <= tolerance && db <= tolerance) {
      mask[mi] = 1;
      count++;
      stack[sp++] = mi;
    }
  };

  while (sp > 0) {
    const p = stack[--sp];
    const x = p % width, y = (p / width) | 0;
    if (x === 0 || y === 0 || x === width - 1 || y === height - 1) touchesEdge = true;
    if (x + 1 < width) tryAdd(x + 1, y);
    if (x - 1 >= 0) tryAdd(x - 1, y);
    if (y + 1 < height) tryAdd(x, y + 1);
    if (y - 1 >= 0) tryAdd(x, y - 1);
  }
  return { mask, count, touchesEdge };
}

// Collects unit boundary edges of the mask (each filled cell contributes an
// edge wherever its neighbor is not filled) and chains them corner-to-corner
// to recover ordered polygon loop(s); the largest by area is the outline.
function traceMaskContour(mask, width, height) {
  const isFilled = (x, y) => x >= 0 && y >= 0 && x < width && y < height && mask[y * width + x] === 1;
  const key = (x, y) => x + "," + y;
  const next = new Map();
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      if (!isFilled(x, y)) continue;
      if (!isFilled(x, y - 1)) next.set(key(x, y), [x + 1, y]);
      if (!isFilled(x + 1, y)) next.set(key(x + 1, y), [x + 1, y + 1]);
      if (!isFilled(x, y + 1)) next.set(key(x + 1, y + 1), [x, y + 1]);
      if (!isFilled(x - 1, y)) next.set(key(x, y + 1), [x, y]);
    }
  }

  const visited = new Set();
  let best = null, bestArea = 0;
  for (const startKey of next.keys()) {
    if (visited.has(startKey)) continue;
    const loop = [];
    let curKey = startKey;
    let guard = 0;
    while (!visited.has(curKey) && guard++ < next.size + 1) {
      visited.add(curKey);
      const [cx, cy] = curKey.split(",").map(Number);
      loop.push([cx, cy]);
      const n = next.get(curKey);
      if (!n) break;
      curKey = key(n[0], n[1]);
    }
    if (loop.length < 3) continue;
    const area = Math.abs(shoelaceArea(loop));
    if (area > bestArea) {
      bestArea = area;
      best = loop;
    }
  }
  return best;
}

function shoelaceArea(pts) {
  let a = 0;
  for (let i = 0; i < pts.length; i++) {
    const [x1, y1] = pts[i], [x2, y2] = pts[(i + 1) % pts.length];
    a += x1 * y2 - x2 * y1;
  }
  return a / 2;
}

function perpendicularDistance(p, a, b) {
  const [px, py] = p, [ax, ay] = a, [bx, by] = b;
  const dx = bx - ax, dy = by - ay;
  const len = Math.hypot(dx, dy);
  if (len === 0) return Math.hypot(px - ax, py - ay);
  return Math.abs(dy * px - dx * py + bx * ay - by * ax) / len;
}

function rdp(points, epsilon) {
  if (points.length < 3) return points.slice();
  let maxDist = 0, index = 0;
  const a = points[0], b = points[points.length - 1];
  for (let i = 1; i < points.length - 1; i++) {
    const d = perpendicularDistance(points[i], a, b);
    if (d > maxDist) {
      maxDist = d;
      index = i;
    }
  }
  if (maxDist > epsilon) {
    const left = rdp(points.slice(0, index + 1), epsilon);
    const right = rdp(points.slice(index), epsilon);
    return left.slice(0, -1).concat(right);
  }
  return [a, b];
}

function simplifyPolygon(points, epsilon) {
  if (points.length <= 3) return points;
  const closed = points.concat([points[0]]);
  const simplified = rdp(closed, epsilon);
  simplified.pop();
  return simplified.length >= 3 ? simplified : points;
}

function computeFloodPolygon(floor, seedFloorPt, tolerance) {
  const ws = getFloodWorkspace(floor);
  const sx = Math.min(ws.width - 1, Math.max(0, Math.round(seedFloorPt.x * ws.scale)));
  const sy = Math.min(ws.height - 1, Math.max(0, Math.round(seedFloorPt.y * ws.scale)));
  const { mask, count, touchesEdge } = floodFillMask(ws.data, ws.width, ws.height, sx, sy, tolerance);
  if (count < 9) {
    return { error: "Selected area is too small — try a different point or a higher tolerance." };
  }
  if (count > ws.width * ws.height * 0.95) {
    return { error: "Fill spread across almost the whole image — try a lower tolerance or draw manually." };
  }
  const loop = traceMaskContour(mask, ws.width, ws.height);
  if (!loop) {
    return { error: "Could not trace an outline — try a different point or draw manually." };
  }
  // Kept tight so angled corners and small notches survive simplification —
  // this only needs to clean up the pixel-staircase noise from tracing, not
  // reshape the outline. The cap below is the real safety valve for noisy
  // (e.g. photographed) sources that would otherwise produce huge polygons.
  const epsilonWork = Math.max(0.75, 0.0012 * Math.max(ws.width, ws.height));
  let simplified = simplifyPolygon(loop, epsilonWork);
  for (let guard = 1; simplified.length > 200 && guard <= 8; guard++) {
    simplified = simplifyPolygon(loop, epsilonWork * Math.pow(1.3, guard));
  }
  const points = simplified.map(([x, y]) => [Math.round(x / ws.scale), Math.round(y / ws.scale)]);
  return { points, warning: touchesEdge ? "Outline touches the image edge — check it looks right." : null };
}

function onValue(msg) {
  if (currentView() === "map") {
    const floor = state.floors.find((f) => String(f.id) === String($("#floor-select").value));
    if (floor) paintOverlay($("#map-overlay"), floor, false);
  } else if (currentView() === "status") {
    renderNodeTable();
  }
}

// ---- overrides view -------------------------------------------------

async function renderOverrides() {
  await loadNodes().catch(() => {});
  fillNodeSelect($("#ov-node"));
  $("#ov-endpoint").innerHTML = WRITABLE.map((e) => `<option>${e}</option>`).join("");
  await renderOverrideTable();
}

async function renderOverrideTable() {
  const rows = (await api("/api/overrides")) || [];
  $("#override-table tbody").innerHTML = rows
    .map(
      (o) => `<tr>
        <td>${o.nodeId}</td><td>${o.endpoint}</td><td>${o.value}</td><td>${esc(o.user || "")}</td>
        <td><button data-del="${o.nodeId}/${o.endpoint}" class="danger">clear</button></td>
      </tr>`
    )
    .join("");
}

$("#override-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("#ov-msg");
  msg.className = "msg";
  msg.textContent = "sending…";
  try {
    await api("/api/commands", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        node: parseInt($("#ov-node").value, 10),
        endpoint: $("#ov-endpoint").value,
        value: parseFloat($("#ov-value").value),
      }),
    });
    msg.className = "msg ok";
    msg.textContent = "queued";
    renderOverrideTable();
  } catch (err) {
    msg.className = "msg err";
    msg.textContent = err.message;
  }
});

$("#override-table").addEventListener("click", async (e) => {
  const t = e.target.dataset.del;
  if (!t) return;
  await api(`/api/overrides/${t}`, { method: "DELETE" });
  renderOverrideTable();
});

// ---- status view --------------------------------------------------

async function renderStatus() {
  await loadNodes().catch(() => {});
  renderNodeTable();
  renderMainStatus();
}

function roomOrDuct(nodeId) {
  const parts = [];
  for (const ep of ["RoomTemp", "SupplyTemp", "ReturnTemp", "DamperActual"]) {
    const v = state.values.get(key(nodeId, ep));
    if (v && v.value.kind === "number") parts.push(`${ep.replace("Temp", "")} ${v.value.num.toFixed(1)}${v.value.unit || ""}`);
  }
  return parts.join(" · ") || "—";
}

const STATUS_CLASS = { online: "on", offline: "off", unexpected: "warn", "link-down": "warn" };

function renderNodeTable() {
  $("#node-table tbody").innerHTML = state.nodes
    .map(
      (n) => `<tr>
        <td>${n.id}</td><td>${esc(n.name || "")}</td><td>${esc(n.module)}</td>
        <td class="${STATUS_CLASS[n.status] || "off"}">${esc(n.status)}</td>
        <td>${esc(roomOrDuct(n.id))}</td>
        <td>${n.lastSeen ? new Date(n.lastSeen * 1000).toLocaleTimeString() : "—"}</td>
      </tr>`
    )
    .join("");
}

function renderMainStatus() {
  const s = state.main && state.main.status;
  const el = $("#main-status");
  if (!s) {
    el.innerHTML = `<div><b>${state.uplinkUp ? "connected" : "no MainController"}</b><span>uplink</span></div>`;
    return;
  }
  const kv = (label, val) => `<div><b>${val}</b><span>${label}</span></div>`;
  el.innerHTML =
    kv("uplink", state.uplinkUp ? "up" : "down") +
    kv("rx frames", s.rxFrames) +
    kv("crc errors", s.crcErrors) +
    kv("resyncs", s.resyncs) +
    kv("tx drops", s.txDrops) +
    kv("downlink drops", s.downlinkDrops) +
    kv("wifi rssi", s.wifiRssi + " dBm") +
    kv("free heap", s.freeHeap + " B");
}

// ---- firmware view ----------------------------------------------

const fwState = { images: [], targets: [] };

async function renderFirmware() {
  await loadFirmware();
}

async function loadFirmware() {
  const [view, jobs] = await Promise.all([api("/api/firmware"), api("/api/ota")]);
  fwState.images = view.images || [];
  fwState.targets = view.targets || [];
  renderFirmwareImages();
  renderFirmwareNodes();
  renderFirmwareJobs(jobs);
}

function fmtBytes(n) {
  if (n >= 1024) return (n / 1024).toFixed(1) + " kB";
  return n + " B";
}

function updatableCount(module) {
  return fwState.targets.filter((t) => t.module === module && t.canUpdate).length;
}

function renderFirmwareImages() {
  const body = $("#fw-image-table tbody");
  if (!fwState.images.length) {
    body.innerHTML = `<tr><td colspan="7" class="empty">No firmware images uploaded yet.</td></tr>`;
    return;
  }
  body.innerHTML = fwState.images
    .map((fi) => {
      const n = updatableCount(fi.module);
      return `<tr>
        <td>${esc(fi.module)}</td>
        <td>${esc(fi.versionStr)}</td>
        <td><code>${esc(fi.filename)}</code></td>
        <td>${fmtBytes(fi.size)}</td>
        <td>${fi.uploadedTs ? new Date(fi.uploadedTs).toLocaleString() : "—"}</td>
        <td><button data-fw-all="${esc(fi.module)}" ${n ? "" : "disabled"}>Update all${n ? ` (${n})` : ""}</button></td>
        <td><button data-fw-del="${esc(fi.module)}" class="danger">Remove</button></td>
      </tr>`;
    })
    .join("");
}

function renderFirmwareNodes() {
  const body = $("#fw-node-table tbody");
  if (!fwState.targets.length) {
    body.innerHTML = `<tr><td colspan="7" class="empty">No nodes known yet.</td></tr>`;
    return;
  }
  body.innerHTML = fwState.targets
    .map((t) => {
      const label = t.job ? t.job : t.canUpdate ? "Update" : t.reason || "—";
      const btn = t.canUpdate
        ? `<button data-fw-up="${t.nodeId}/${t.target}">Update</button>`
        : `<button disabled>${esc(label)}</button>`;
      const indent = t.target === "thermostat" ? ' style="padding-left:1.6rem"' : "";
      const idCell = t.target === "thermostat" ? "↳" : t.nodeId === 0 ? "MC" : t.nodeId;
      return `<tr>
        <td>${idCell}</td>
        <td${indent}>${esc(t.name || "")}</td>
        <td>${esc(t.module)}</td>
        <td class="${STATUS_CLASS[t.status] || "off"}">${esc(t.status)}</td>
        <td>${esc(t.installedStr)}</td>
        <td>${esc(t.latestStr || "—")}</td>
        <td>${btn}</td>
      </tr>`;
    })
    .join("");
}

function renderFirmwareJobs(jobs) {
  $("#fw-job-table tbody").innerHTML = (jobs || [])
    .map((j) => {
      const pct = j.size ? Math.round((j.lastOffset / j.size) * 100) : 0;
      return `<tr>
        <td>${j.id}</td><td>${j.nodeId}</td><td>${esc(j.target || "node")}</td><td>${esc(j.filename)}</td>
        <td>${esc(j.state)}${j.error ? " – " + esc(j.error) : ""}</td>
        <td>${pct}%</td>
      </tr>`;
    })
    .join("");
}

$("#fw-upload-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("#fw-msg");
  msg.className = "msg";
  msg.textContent = "uploading…";
  const f = $("#fw-file").files[0];
  if (!f) {
    msg.className = "msg err";
    msg.textContent = "choose a file";
    return;
  }
  const fd = new FormData();
  fd.append("image", f);
  try {
    const fi = await api("/api/firmware", { method: "POST", body: fd });
    msg.className = "msg ok";
    msg.textContent = `stored ${fi.module} ${fi.versionStr}`;
    $("#fw-file").value = "";
    loadFirmware();
  } catch (err) {
    msg.className = "msg err";
    msg.textContent = err.message;
  }
});

$("#fw-image-table").addEventListener("click", async (e) => {
  const del = e.target.dataset.fwDel;
  const all = e.target.dataset.fwAll;
  try {
    if (del) {
      if (!confirm(`Remove the stored ${del} image?`)) return;
      await api(`/api/firmware/${encodeURIComponent(del)}`, { method: "DELETE" });
    } else if (all) {
      if (all === "MainController" && !confirm("Update the MainController? The bus and this server link drop while it reboots into its bootloader.")) return;
      e.target.disabled = true;
      const r = await api("/api/firmware/update-all", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ module: all }),
      });
      flash(`queued ${r.queued} update${r.queued === 1 ? "" : "s"}`);
    } else return;
    loadFirmware();
  } catch (err) {
    flash(err.message, true);
    loadFirmware();
  }
});

$("#fw-node-table").addEventListener("click", async (e) => {
  const up = e.target.dataset.fwUp;
  if (!up) return;
  const [node, target] = up.split("/");
  if (node === "0" && !confirm("Update the MainController itself? The bus and this server link drop while it reboots into its bootloader.")) return;
  e.target.disabled = true;
  try {
    await api("/api/firmware/update", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ node: parseInt(node, 10), target }),
    });
    flash("update queued");
  } catch (err) {
    flash(err.message, true);
  }
  loadFirmware();
});

function flash(text, isErr) {
  const msg = $("#fw-msg");
  msg.className = "msg " + (isErr ? "err" : "ok");
  msg.textContent = text;
}

// ---- map setup view ---------------------------------------------

async function renderSetup() {
  draft = null;
  await Promise.all([loadFloors(), loadPlacements(), loadNodes().catch(() => {})]);
  const fs = $("#setup-floor-select");
  const prev = fs.value;
  fs.innerHTML = state.floors.map((f) => `<option value="${f.id}">${esc(f.name)}</option>`).join("");
  if (prev) fs.value = prev;

  fillNodeSelect($("#setup-node-select"), true);
  updateOutlineBar();
  if (state.floors.length) drawFloor(fs.value, $("#setup-img"), $("#setup-overlay"), true);
}

function setupHintForMode(mode) {
  return mode === "flood"
    ? "click inside the room to flood-fill it"
    : mode === "manual"
    ? "click each corner, then Finish shape"
    : "then click on the plan";
}

$("#setup-floor-select").addEventListener("change", () => {
  draft = null;
  updateOutlineBar();
  drawFloor($("#setup-floor-select").value, $("#setup-img"), $("#setup-overlay"), true);
});
$("#setup-node-select").addEventListener("change", () => {
  draft = null;
  updateOutlineBar();
  drawFloor($("#setup-floor-select").value, $("#setup-img"), $("#setup-overlay"), true);
});
$("#setup-mode-select").addEventListener("change", () => {
  draft = null;
  $("#setup-hint").textContent = setupHintForMode($("#setup-mode-select").value);
  updateOutlineBar();
  drawFloor($("#setup-floor-select").value, $("#setup-img"), $("#setup-overlay"), true);
});
$("#setup-tolerance").addEventListener("input", () => {
  if (!draft || draft.mode !== "flood" || !draft.seed) return;
  const floor = state.floors.find((f) => f.id === draft.floorId);
  if (floor) recomputeFloodDraft(floor, $("#setup-overlay"));
});
$("#setup-outline-undo").addEventListener("click", () => {
  if (!draft || draft.mode !== "manual") return;
  draft.points.pop();
  const floor = state.floors.find((f) => f.id === draft.floorId);
  updateOutlineBar();
  if (floor) paintOverlay($("#setup-overlay"), floor, true);
});
$("#setup-outline-finish").addEventListener("click", () => {
  if (!draft || !draft.points || draft.points.length < 3) return;
  acceptDraft($("#setup-overlay"));
});
$("#setup-outline-accept").addEventListener("click", () => {
  if (!draft || !draft.points) return;
  acceptDraft($("#setup-overlay"));
});
$("#setup-outline-manual").addEventListener("click", () => {
  if (!draft) return;
  const { nodeId, floorId } = draft;
  $("#setup-mode-select").value = "manual";
  $("#setup-hint").textContent = setupHintForMode("manual");
  draft = { mode: "manual", nodeId, floorId, points: [] };
  const floor = state.floors.find((f) => f.id === floorId);
  updateOutlineBar();
  if (floor) paintOverlay($("#setup-overlay"), floor, true);
});
$("#setup-outline-cancel").addEventListener("click", () => cancelDraft($("#setup-overlay")));

$("#floor-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("#floor-msg");
  msg.className = "msg";
  msg.textContent = "uploading…";
  const fd = new FormData();
  fd.append("name", $("#floor-name").value);
  fd.append("image", $("#floor-file").files[0]);
  try {
    await api("/api/floors", { method: "POST", body: fd });
    msg.className = "msg ok";
    msg.textContent = "added";
    renderSetup();
  } catch (err) {
    msg.className = "msg err";
    msg.textContent = err.message;
  }
});

$("#floor-delete").addEventListener("click", async () => {
  const id = $("#setup-floor-select").value;
  if (!id || !confirm("Delete this floor and its placements?")) return;
  await api(`/api/floors/${id}`, { method: "DELETE" });
  renderSetup();
});

// ---- helpers -----------------------------------------------------

function fillNodeSelect(sel, controllersOnly = false) {
  const prev = sel.value;
  let list = state.nodes;
  if (controllersOnly) list = list.filter((n) => n.module === "ControllerNode" || n.module === "Unknown");
  sel.innerHTML = list.map((n) => `<option value="${n.id}">${n.id} — ${esc(n.name || n.module)}</option>`).join("");
  if (prev) sel.value = prev;
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// ---- boot -------------------------------------------------------

loadNodes().catch(() => {});
connectWS();
route();
