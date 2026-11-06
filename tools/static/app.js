// TETHER dashboard front end. Polls the JSON API every few seconds; no build step, no framework.
"use strict";

const POLL_MS = 5000;
const STALE_S = 120;
const $ = (s) => document.querySelector(s);

const state = { peer: "", metric: "snr", map: null, layers: null, fitted: false, geoWatch: null, lastGeoPost: 0 };

// ------------------------------------------------------------------------------------------------------------------
// Formatting

const fmtDist = (m) => (m == null ? "–" : m < 1000 ? `${m.toFixed(0)} m` : `${(m / 1000).toFixed(m < 10000 ? 2 : 1)} km`);
const fmtNum = (v, d = 0) => (v == null ? "–" : Number(v).toFixed(d));
const fmtClock = (t) => new Date(t * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
function fmtAge(t) {
  if (t == null) return "no packets yet";
  const s = Math.max(0, Date.now() / 1000 - t);
  if (s < 60) return `last rx ${s.toFixed(0)} s ago`;
  if (s < 3600) return `last rx ${(s / 60).toFixed(0)} min ago`;
  return `last rx ${fmtClock(t)}`;
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

async function get(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (!r.ok) throw new Error(`${path}: ${r.status}`);
  return r.json();
}

// ------------------------------------------------------------------------------------------------------------------
// Tiny SVG charts

function niceTicks(min, max, n = 5) {
  if (!isFinite(min) || !isFinite(max)) return [];
  if (min === max) { min -= 1; max += 1; }
  const raw = (max - min) / n;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1, 2, 5, 10].map((k) => k * mag).find((s) => s >= raw) || raw;
  const out = [];
  for (let v = Math.ceil(min / step) * step; v <= max + 1e-9; v += step) out.push(+v.toFixed(10));
  return out;
}

function svgEl(tag, attrs, text) {
  const e = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (text != null) e.textContent = text;
  return e;
}

/**
 * series: [{cls: "s1"|"s2", points: [[x, y], ...]}]; scatter draws dots instead of lines.
 * Gaps longer than gapX break a line.
 */
function drawChart(el, { series, xFmt, yFmt, scatter = false, gapX = Infinity, emptyText = "No data yet" }) {
  const W = Math.max(el.clientWidth, 280), H = el.clientHeight || 220;
  const m = { l: 44, r: 10, t: 10, b: 24 };
  const svg = svgEl("svg", { viewBox: `0 0 ${W} ${H}`, preserveAspectRatio: "none" });
  const pts = series.flatMap((s) => s.points);
  el.replaceChildren(svg);
  if (!pts.length) {
    svg.append(svgEl("text", { x: W / 2, y: H / 2, "text-anchor": "middle", class: "empty" }, emptyText));
    return;
  }
  let [x0, x1] = [Math.min(...pts.map((p) => p[0])), Math.max(...pts.map((p) => p[0]))];
  let [y0, y1] = [Math.min(...pts.map((p) => p[1])), Math.max(...pts.map((p) => p[1]))];
  if (x0 === x1) { x0 -= 1; x1 += 1; }
  const pad = Math.max((y1 - y0) * 0.08, 1);
  y0 -= pad; y1 += pad;
  const X = (v) => m.l + ((v - x0) / (x1 - x0)) * (W - m.l - m.r);
  const Y = (v) => H - m.b - ((v - y0) / (y1 - y0)) * (H - m.t - m.b);

  for (const v of niceTicks(y0, y1, 4)) {
    svg.append(svgEl("line", { x1: m.l, x2: W - m.r, y1: Y(v), y2: Y(v), class: "gridline" }));
    svg.append(svgEl("text", { x: m.l - 6, y: Y(v) + 4, "text-anchor": "end" }, yFmt(v)));
  }
  const xt = niceTicks(x0, x1, Math.max(3, Math.floor(W / 110)));
  for (const v of xt) svg.append(svgEl("text", { x: X(v), y: H - 6, "text-anchor": "middle" }, xFmt(v)));
  svg.append(svgEl("line", { x1: m.l, x2: m.l, y1: m.t, y2: H - m.b, class: "axis" }));
  svg.append(svgEl("line", { x1: m.l, x2: W - m.r, y1: H - m.b, y2: H - m.b, class: "axis" }));

  for (const s of series) {
    if (scatter) {
      for (const [x, y] of s.points) svg.append(svgEl("rect", { x: X(x) - 2, y: Y(y) - 2, width: 4, height: 4, class: s.cls.replace("s", "d") }));
      continue;
    }
    let d = "", prev = null;
    for (const [x, y] of s.points) {
      d += `${prev == null || x - prev > gapX ? "M" : "L"}${X(x).toFixed(1)},${Y(y).toFixed(1)}`;
      prev = x;
    }
    svg.append(svgEl("path", { d, class: s.cls }));
  }
}

// ------------------------------------------------------------------------------------------------------------------
// Map

function initMap() {
  if (state.map || !window.L) return;
  state.map = L.map("map", { zoomControl: true, attributionControl: true }).setView([0, 0], 2);
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 19,
    attribution: "© OpenStreetMap contributors",
  }).addTo(state.map);
  state.layers = L.layerGroup().addTo(state.map);
}

function drawMap(trk) {
  initMap();
  if (!state.map) return;
  const s1 = cssVar("--s1"), s2 = cssVar("--s2");
  state.layers.clearLayers();
  const bounds = [];
  const peerLine = trk.peer.map((p) => [p.lat, p.lon]);
  const localPts = trk.local.concat(trk.phone).map((p) => [p.lat, p.lon]);
  if (peerLine.length) {
    L.polyline(peerLine, { color: s2, weight: 3 }).addTo(state.layers);
    L.circleMarker(peerLine[peerLine.length - 1], { radius: 8, color: "#111", weight: 2, fillColor: s2, fillOpacity: 1 })
      .bindTooltip("peer").addTo(state.layers);
    bounds.push(...peerLine);
  }
  const here = localPts.length ? localPts[localPts.length - 1] : trk.base;
  if (here) {
    L.circleMarker(here, { radius: 8, color: "#111", weight: 2, fillColor: s1, fillOpacity: 1 })
      .bindTooltip(localPts.length ? "this node" : "base (fixed)").addTo(state.layers);
    bounds.push(here);
  }
  $("#map-hint").textContent = peerLine.length ? "" : "No peer positions yet. The peer needs GPS (or a fixed position) and position sharing on.";
  if (bounds.length && !state.fitted) {
    state.map.fitBounds(bounds, { padding: [24, 24], maxZoom: 16 });
    state.fitted = true;
  }
}

// ------------------------------------------------------------------------------------------------------------------
// Render

function renderSummary(sum) {
  $("#synthetic").hidden = !sum.synthetic;
  $("#local").textContent = sum.local_node || "no node";

  const sel = $("#peer");
  const ids = sum.nodes.map((n) => n.id);
  const want = ["", ...ids].join(",");
  if (sel.dataset.ids !== want) {
    sel.replaceChildren(new Option(`auto${sum.peer && !state.peer ? ` (${sum.peer})` : ""}`, ""));
    for (const n of sum.nodes) sel.append(new Option(`${n.id} · ${n.packets}`, n.id));
    sel.dataset.ids = want;
    sel.value = state.peer;
  }

  const last = sum.last_rx;
  const age = $("#age");
  age.textContent = fmtAge(last && last.t);
  age.classList.toggle("stale", !!last && Date.now() / 1000 - last.t > STALE_S);

  $("#t-rssi").textContent = last ? `${last.rssi} dBm` : "–";
  $("#t-snr").textContent = last ? `SNR ${fmtNum(last.snr, 2)} dB` : "SNR –";
  $("#t-dist").textContent = fmtDist(sum.last_distance && sum.last_distance.d_m);
  $("#t-max").textContent = fmtDist(sum.max_distance && sum.max_distance.d_m);
  $("#t-max-at").textContent = sum.max_distance
    ? `${new Date(sum.max_distance.t * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })} · SNR ${fmtNum(sum.max_distance.snr, 1)}`
    : "heard at";
  const st = sum.tether_stats;
  if (st && st.pdr32 != null) {
    $("#t-pdr").textContent = `${Math.round(st.pdr32 * 100)}%`;
    $("#t-pdr-n").textContent = `${st.window ?? "?"} pings · ${st.role || ""}`;
  } else if (st && st.loss != null) {
    $("#t-pdr").textContent = `${Math.round((1 - st.loss) * 100)}%`;
    $("#t-pdr-n").textContent = "pings heard (responder)";
  } else {
    $("#t-pdr").textContent = "–";
  }
  $("#t-pkts").textContent = sum.counts.packets;
  $("#t-pos").textContent = `positions ${sum.counts.positions}`;
}

function renderSignal(sig) {
  const key = state.metric;
  const rpt = sig.rpt.filter((r) => r[key] != null);
  const last = rpt[rpt.length - 1];
  $("#t-rpt").textContent = last ? `${last.rssi} dBm` : "–";
  $("#t-rpt-snr").textContent = last ? `SNR ${fmtNum(last.snr, 2)} dB` : "SNR –";
  drawChart($("#chart-time"), {
    series: [
      { cls: "s1", points: sig.rx.filter((r) => r[key] != null).map((r) => [r.t, r[key]]) },
      { cls: "s2", points: rpt.map((r) => [r.t, r[key]]) },
    ],
    xFmt: (t) => new Date(t * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }),
    yFmt: (v) => `${v}`,
    gapX: 180,
    emptyText: "No packets from the peer yet",
  });
}

function renderDistance(dist) {
  const km = dist.some((d) => d.d_m >= 1000);
  drawChart($("#chart-dist"), {
    series: [{ cls: "s1", points: dist.filter((d) => d.snr != null).map((d) => [km ? d.d_m / 1000 : d.d_m, d.snr]) }],
    xFmt: (v) => (km ? `${v} km` : `${v} m`),
    yFmt: (v) => `${v} dB`,
    scatter: true,
    emptyText: "Needs positions for both nodes",
  });
}

function renderMessages(msgs) {
  const ul = $("#msgs");
  if (!msgs.length) {
    ul.replaceChildren(Object.assign(document.createElement("li"), { className: "empty", textContent: "No messages yet" }));
    return;
  }
  ul.replaceChildren(...msgs.map((m) => {
    const li = document.createElement("li");
    li.className = m.dir;
    const who = document.createElement("div");
    who.className = "who";
    who.textContent = m.dir === "tx" ? `me → ${m.to_id || "all"} · ${fmtClock(m.t)}` : `${m.from_id} · ${fmtClock(m.t)}`;
    const body = document.createElement("div");
    body.textContent = m.text; // textContent: messages are untrusted
    li.append(who, body);
    if (m.dir === "rx" && m.rssi != null) {
      const sig = document.createElement("div");
      sig.className = "sig";
      const hops = m.hop_start != null && m.hop_limit != null ? m.hop_start - m.hop_limit : null;
      sig.textContent = `RSSI ${m.rssi} dBm · SNR ${fmtNum(m.snr, 2)} dB${hops != null ? ` · ${hops} hop${hops === 1 ? "" : "s"}` : ""}`;
      li.append(sig);
    }
    return li;
  }));
}

async function refresh() {
  const q = state.peer ? `?peer=${encodeURIComponent(state.peer)}` : "";
  try {
    const [sum, sig, dist, trk, msgs] = await Promise.all([
      get(`/api/summary${q}`), get(`/api/signal${q}`), get(`/api/distance${q}`), get(`/api/track${q}`), get("/api/messages?limit=100"),
    ]);
    $("#offline").hidden = true;
    renderSummary(sum);
    renderSignal(sig);
    renderDistance(dist);
    drawMap(trk);
    renderMessages(msgs);
  } catch (e) {
    $("#offline").hidden = false;
    console.warn(e);
  }
}

// ------------------------------------------------------------------------------------------------------------------
// Phone geolocation (Phase 4c)

function toggleGeo() {
  const btn = $("#geo"), status = $("#geo-status");
  if (state.geoWatch != null) {
    navigator.geolocation.clearWatch(state.geoWatch);
    state.geoWatch = null;
    btn.classList.remove("on");
    btn.textContent = "Use this phone's GPS";
    status.textContent = "Stopped.";
    return;
  }
  if (!("geolocation" in navigator)) { status.textContent = "This browser has no geolocation."; return; }
  if (!window.isSecureContext) { status.textContent = "Phone GPS needs HTTPS (e.g. tailscale serve). See tools/README.md."; return; }
  state.geoWatch = navigator.geolocation.watchPosition(async (pos) => {
    const now = Date.now();
    if (now - state.lastGeoPost < 5000) return;
    state.lastGeoPost = now;
    const body = { lat: pos.coords.latitude, lon: pos.coords.longitude, accuracy: pos.coords.accuracy, t: pos.timestamp / 1000 };
    try {
      const r = await fetch("/api/geo", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
      status.textContent = r.ok ? `Sent ${body.lat.toFixed(5)}, ${body.lon.toFixed(5)} (±${Math.round(body.accuracy)} m) at ${fmtClock(body.t)}` : `Server refused: ${r.status}`;
    } catch (e) {
      status.textContent = "Could not reach the server.";
    }
  }, (err) => { status.textContent = `Location error: ${err.message}`; }, { enableHighAccuracy: true, maximumAge: 2000, timeout: 20000 });
  btn.classList.add("on");
  btn.textContent = "Stop sharing GPS";
  status.textContent = "Waiting for a fix…";
}

// ------------------------------------------------------------------------------------------------------------------

$("#peer").addEventListener("change", (e) => { state.peer = e.target.value; state.fitted = false; refresh(); });
for (const b of document.querySelectorAll(".seg button")) {
  b.addEventListener("click", () => {
    state.metric = b.dataset.metric;
    document.querySelectorAll(".seg button").forEach((x) => x.classList.toggle("on", x === b));
    refresh();
  });
}
$("#geo").addEventListener("click", toggleGeo);
window.addEventListener("resize", () => refresh());

if ("serviceWorker" in navigator && window.isSecureContext) {
  navigator.serviceWorker.register("sw.js").catch((e) => console.warn("service worker:", e));
}

refresh();
setInterval(refresh, POLL_MS);
