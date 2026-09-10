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
    markers += `<g data-node="${p.nodeId}">
      <circle class="node-dot" cx="${p.xPx}" cy="${p.yPx}" r="6"/>
      <text class="node-label" x="${p.xPx + 12}" y="${p.yPx - 2}">${label}</text>
      <text class="node-sub" x="${p.xPx + 12}" y="${p.yPx + 14}">${esc(sub)}</text>
    </g>`;
  }
  defs += `</defs>`;
  svg.innerHTML = defs + blobs + markers;

  if (editable) {
    svg.onclick = (ev) => {
      const nodeSel = $("#setup-node-select");
      const nodeId = parseInt(nodeSel.value, 10);
      if (!nodeId) return;
      const pt = svgPoint(svg, ev);
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
  await Promise.all([loadFloors(), loadPlacements(), loadNodes().catch(() => {})]);
  const fs = $("#setup-floor-select");
  const prev = fs.value;
  fs.innerHTML = state.floors.map((f) => `<option value="${f.id}">${esc(f.name)}</option>`).join("");
  if (prev) fs.value = prev;

  fillNodeSelect($("#setup-node-select"), true);
  if (state.floors.length) drawFloor(fs.value, $("#setup-img"), $("#setup-overlay"), true);
}

$("#setup-floor-select").addEventListener("change", () =>
  drawFloor($("#setup-floor-select").value, $("#setup-img"), $("#setup-overlay"), true)
);

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
  sel.innerHTML = list.map((n) => `<option value="${n.id}">${n.id} — ${esc(n.module)}</option>`).join("");
  if (prev) sel.value = prev;
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// ---- boot -------------------------------------------------------

loadNodes().catch(() => {});
connectWS();
route();
