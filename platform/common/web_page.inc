// SPDX-License-Identifier: GPL-3.0-or-later
// platform/common/web_page.inc — the web interface's page (web_ui.cpp).
// One file: HTML, CSS and script; it talks to the /api routes.
R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>G3MacPi</title>
<style>
  :root { --ink:#000; --paper:#fff; --platinum:#dddddd; --shade:#888; --hi:#2b3fbf; }
  * { box-sizing: border-box; }
  body { margin:0; font: 13px/1.45 "Charcoal","Chicago","Geneva",system-ui,sans-serif; color:var(--ink);
         background: repeating-conic-gradient(#8e8ea0 0% 25%, #7c7c8e 0% 50%) 0 0/4px 4px; }
  header { background:var(--paper); border-bottom:1px solid #000; padding:3px 14px; display:flex; gap:18px;
           align-items:center; font-weight:bold; position:sticky; top:0; z-index:2; }
  header .mark { width:16px; height:14px; background:#000; border-radius:2px; position:relative; }
  header .mark::after { content:""; position:absolute; left:3px; top:2px; width:10px; height:7px; background:#4a7bd9; }
  header .right { margin-left:auto; font-weight:normal; }
  header a { color:inherit; text-decoration:none; padding:0 6px; }
  header a:hover { background:#000; color:#fff; }
  main { max-width:980px; margin:22px auto; padding:0 12px; display:grid; gap:22px;
         grid-template-columns: repeat(auto-fit, minmax(440px, 1fr)); }
  .win { background:var(--paper); border:1px solid #000; box-shadow:2px 2px 0 #000; }
  .title { height:20px; border-bottom:1px solid #000; position:relative; text-align:center;
           background: repeating-linear-gradient(#fff 0 1px, #000 1px 2px) 0 4px/100% 12px no-repeat; }
  .title span { background:#fff; padding:0 8px; font-weight:bold; line-height:20px; }
  .title i { position:absolute; left:8px; top:4px; width:11px; height:11px; border:1px solid #000; background:#fff; }
  .body { padding:12px 14px; }
  table { width:100%; border-collapse:collapse; }
  td, th { padding:3px 6px; text-align:left; vertical-align:top; }
  th { font-weight:bold; border-bottom:1px solid #aaa; }
  tr.sel td { background:#000; color:#fff; }
  .k { color:#444; width:42%; }
  button, .file { font:inherit; background:var(--platinum); border:1px solid #000; border-radius:5px;
                  padding:3px 12px; box-shadow:inset -1px -1px 0 #999, inset 1px 1px 0 #fff; cursor:pointer; }
  button:active { background:#000; color:#fff; }
  button.default { outline:2px solid #000; outline-offset:2px; }
  textarea { width:100%; height:260px; font:12px/1.4 Monaco,"Courier New",monospace; border:1px solid #000; padding:6px; }
  .tabs { display:flex; gap:4px; margin-bottom:8px; }
  .tabs button.on { background:#000; color:#fff; }
  .bar { height:12px; border:1px solid #000; background:#fff; margin:8px 0 2px; }
  .bar div { height:100%; width:0; background: repeating-linear-gradient(45deg,#4a7bd9 0 6px,#2b4fa8 6px 12px); }
  .msg { min-height:1.4em; margin-top:6px; }
  .bad { color:#a00000; } .good { color:#006000; }
  .small { color:#555; font-size:12px; }
  .row { display:flex; gap:8px; align-items:center; flex-wrap:wrap; margin-top:8px; }
  .wide { grid-column: 1 / -1; }
</style>
</head>
<body>
<header><div class="mark"></div><div>G3MacPi</div><a href="#status-win">Status</a><a href="#images-win">Images</a>
  <a href="#settings-win">Settings</a><a href="#update-win">Update</a>
  <div class="right" id="clock"></div></header>
<main>
  <section class="win" id="status-win">
    <div class="title"><i></i><span>Status</span></div>
    <div class="body"><table id="status"><tr><td>Connecting…</td></tr></table></div>
  </section>

  <section class="win" id="update-win">
    <div class="title"><i></i><span>Update the emulator</span></div>
    <div class="body">
      <p>Upload a new <b>kernel8-rpi4.img</b> (from a G3MacPi build). It is checked, the current image is kept
         as kernel8-rpi4.bak, and the new one runs after a restart.</p>
      <div class="row"><input type="file" id="kfile" accept=".img"><button onclick="updateKernel()">Upload image</button></div>
      <div class="bar"><div id="kbar"></div></div>
      <div class="msg" id="kmsg"></div>
      <div class="row"><button onclick="restartPi()">Restart the Pi</button>
        <span class="small">The Mac's state is saved first, as with Ctrl+Alt+Delete.</span></div>
    </div>
  </section>

  <section class="win wide" id="images-win">
    <div class="title"><i></i><span>Images on the SD card</span></div>
    <div class="body">
      <div class="tabs" id="tabs">
        <button data-dir="cd" class="on">CD images</button><button data-dir="hdd">Hard disks</button>
        <button data-dir="rom">ROM</button><button data-dir="disks">Other disks</button>
      </div>
      <table id="files"></table>
      <div class="row"><input type="file" id="ufile"><button onclick="upload()">Upload to this folder</button></div>
      <div class="bar"><div id="ubar"></div></div>
      <div class="msg" id="umsg"></div>
      <p class="small">"Insert in CD drive" puts a disc into the Mac's CD-ROM drive at once. An image the
         running Mac is using can't be replaced or deleted: eject it first (Status, or F12 on the Pi).</p>
    </div>
  </section>

  <section class="win wide" id="settings-win">
    <div class="title"><i></i><span>Settings — g3macpi.ini</span></div>
    <div class="body">
      <textarea id="cfg" spellcheck="false"></textarea>
      <div class="row"><button class="default" onclick="saveCfg()">Save</button><button onclick="loadCfg()">Revert</button>
        <span class="small">Checked before saving; changes apply when the Mac next starts.</span></div>
      <div class="msg" id="cmsg"></div>
    </div>
  </section>
</main>
<script>
"use strict";
let dir = "cd";
let cdDrive = -1;   // the first CD-ROM drive (from the status)
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function size(n) {
  const u = ["bytes","KB","MB","GB"]; let i = 0;
  while (n >= 1024 && i < 3) { n /= 1024; i++; }
  return (i ? n.toFixed(1) : n) + " " + u[i];
}
function msg(el, text, ok) { el.textContent = text; el.className = "msg " + (ok ? "good" : "bad"); }

async function status() {
  try {
    const s = await (await fetch("/api/status")).json();
    const h = Math.floor(s.uptime_s / 3600), m = Math.floor(s.uptime_s / 60) % 60;
    const rows = [["Emulator", s.name + " " + s.version], ["The Mac", s.mac_state],
      ["Speed", s.mips + " MIPS"], ["CPU emulation", s.jit || "interpreter"], ["Pi running for", h + " h " + m + " min"],
      ["Pi address", s.ip || "(none yet)"], ["Pi Ethernet", s.pi_ethernet], ["Mac Ethernet", s.mac_ethernet],
      ["Sound", s.sound ? "on (HDMI)" : "off"], ["Mac network", s.network ? "on" : "off"], ["Pi connection", s.port || "(none)"],
      ["Password", s.password ? "set" : "none (set web_password in the settings)"]];
    cdDrive = s.drives.findIndex(d => d.type === "cdrom");
    const drives = s.drives.map((d, i) => d.type === "none" ? "" :
      `<tr><td class="k">${esc(d.label)}</td><td>${d.type === "cdrom" ? "CD: " : "Disk: "}` +
      `${esc(d.image ? d.image.split("/").pop() : "(empty)")}` +
      `${d.type === "cdrom" && d.image ? ` <button onclick="eject(${i})">Eject</button>` : ""}</td></tr>`).join("");
    $("status").innerHTML = rows.map(r => `<tr><td class="k">${esc(r[0])}</td><td>${esc(r[1])}</td></tr>`).join("") + drives;
  } catch (e) { $("status").innerHTML = "<tr><td class='bad'>The Pi does not answer.</td></tr>"; }
}

async function files() {
  const r = await fetch("/api/files?dir=" + dir);
  if (!r.ok) { $("files").innerHTML = "<tr><td class='bad'>Can't list this folder.</td></tr>"; return; }
  const j = await r.json();
  if (!j.files.length) { $("files").innerHTML = `<tr><td class="small">${esc(j.dir)} is empty.</td></tr>`; return; }
  $("files").innerHTML = "<tr><th>Name</th><th>Size</th><th></th></tr>" + j.files.map(f =>
    `<tr><td>${esc(f.name)}${f.in_use ? " <b>(in use)</b>" : ""}</td><td>${size(f.size)}</td>` +
    `<td>${cdDrive >= 0 && !f.in_use ? `<button onclick="insert('${encodeURIComponent(f.name)}')">Insert in CD drive</button> ` : ""}` +
    `${f.in_use ? "" : `<button onclick="del('${encodeURIComponent(f.name)}')">Delete</button>`}</td></tr>`).join("");
}

async function insert(name) {
  const r = await fetch(`/api/insert?drive=${cdDrive}&dir=${dir}&name=${name}`, {method: "POST"});
  msg($("umsg"), await r.text(), r.ok);
  status(); files();
}
async function eject(drive) {
  const r = await fetch(`/api/insert?drive=${drive}`, {method: "POST"});
  msg($("umsg"), await r.text(), r.ok);
  status(); files();
}

async function del(name) {
  if (!confirm("Delete " + decodeURIComponent(name) + "?")) return;
  const r = await fetch(`/api/files?dir=${dir}&name=${name}`, {method: "DELETE"});
  msg($("umsg"), await r.text(), r.ok);
  files();
}

// PUT with progress (fetch has no upload progress).
function put(url, file, bar, out, done) {
  const x = new XMLHttpRequest();
  x.open("PUT", url);
  x.upload.onprogress = e => { if (e.lengthComputable) bar.style.width = (100 * e.loaded / e.total) + "%"; };
  x.onload = () => { msg(out, x.responseText, x.status === 200); if (done) done(x.status === 200); };
  x.onerror = () => msg(out, "The upload was interrupted.", false);
  msg(out, "Uploading " + file.name + " (" + size(file.size) + ")…", true);
  x.send(file);
}

function upload() {
  const f = $("ufile").files[0];
  if (!f) { msg($("umsg"), "Choose a file first.", false); return; }
  put(`/api/upload?dir=${dir}&name=${encodeURIComponent(f.name)}`, f, $("ubar"), $("umsg"), () => files());
}

function updateKernel() {
  const f = $("kfile").files[0];
  if (!f) { msg($("kmsg"), "Choose kernel8-rpi4.img first.", false); return; }
  put("/api/update", f, $("kbar"), $("kmsg"));
}

async function restartPi() {
  if (!confirm("Restart the Pi now? The Mac stops (its state is saved).")) return;
  try { await fetch("/api/reboot", {method: "POST"}); } catch (e) {}
  msg($("kmsg"), "Restarting… this page reconnects when the Pi is back.", true);
}

async function loadCfg() {
  const r = await fetch("/api/config");
  $("cfg").value = r.ok ? await r.text() : "";
  $("cmsg").textContent = "";
}
async function saveCfg() {
  const r = await fetch("/api/config", {method: "PUT", body: $("cfg").value});
  msg($("cmsg"), await r.text(), r.ok);
}

document.querySelectorAll("#tabs button").forEach(b => b.onclick = () => {
  document.querySelectorAll("#tabs button").forEach(x => x.classList.toggle("on", x === b));
  dir = b.dataset.dir; $("umsg").textContent = ""; $("ubar").style.width = "0"; files();
});
setInterval(() => { $("clock").textContent = new Date().toLocaleTimeString(); }, 1000);
setInterval(status, 3000);
status().then(files); loadCfg();
</script>
</body>
</html>
)HTML"
