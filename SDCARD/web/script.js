/* ═══════════════════════════════════════════════════════════════
   ESTUFA INTELIGENTE — script.js
   Funções originais (material, secagem, armazenamento, validade)
   + Card de balança HX711
   ═══════════════════════════════════════════════════════════════ */

function getApiBase() {
  const h    = window.location.hostname;
  const prot = window.location.protocol;
  // Aberto direto no browser (duplo clique) ou via emulador → usa localhost:8080
  if (prot === "file:" || h === "localhost" || h === "127.0.0.1") {
    return "http://localhost:8080";
  }
  // Rodando no ESP32 → usa o próprio host
  return "";
}
const API_BASE = getApiBase();

// ── Gráfico ────────────────────────────────────────────────────
let chart;

function initChart() {
  const ctx = document.getElementById("chart").getContext("2d");
  chart = new Chart(ctx, {
    type: "line",
    data: {
      labels: Array(60).fill(""),
      datasets: [
        { label: "Temperatura (°C)", data: Array(60).fill(0),
          borderColor: "#00d9ff", backgroundColor: "rgba(0,217,255,.1)",
          tension: 0.4, fill: true, pointRadius: 0, borderWidth: 2 },
        { label: "Umidade (%)", data: Array(60).fill(0),
          borderColor: "#00ff88", backgroundColor: "rgba(0,255,136,.08)",
          tension: 0.4, fill: true, pointRadius: 0, borderWidth: 2 }
      ]
    },
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      plugins: { legend: { labels: { color: "#e0f7ff", font: { family: "Outfit" } } } },
      scales: {
        y: { ticks: { color: "#8899aa", font: { family: "JetBrains Mono", size: 10 } },
             grid: { color: "rgba(0,217,255,.08)" } },
        x: { display: false }
      }
    }
  });
}

// ── Polling ────────────────────────────────────────────────────
async function fetchData() {
  try {
    const resp = await fetch(API_BASE + "/api/data");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    const data = await resp.json();
    atualizarDashboard(data);
    setConexao(true);
  } catch (e) {
    console.warn("[Estufa] Falha:", e.message);
    setConexao(false);
  }
}

function setConexao(online) {
  const el = document.getElementById("connectionStatus");
  if (!el) return;
  el.textContent = online ? "● ONLINE" : "○ OFFLINE";
  el.className   = "status-badge" + (online ? "" : " offline");
}

// ── Atualizar UI ───────────────────────────────────────────────
function atualizarDashboard(data) {
  setText("temp",     parseFloat(data.temp     ?? data.temperatura ?? 0).toFixed(1));
  setText("umid",     parseFloat(data.umid     ?? data.umidade     ?? 0).toFixed(1));
  setText("setpoint", parseFloat(data.setpoint ?? 0).toFixed(1));
  setText("validade", data.validade ?? "--");

  const matSel = document.getElementById("material");
  if (matSel && data.materialIdx !== undefined) matSel.value = data.materialIdx;

  setText("modoTexto", data.modo ?? "Armazenamento");
  setText("btnTexto",  data.ativo ? "⏹️ DESLIGAR SISTEMA" : "🔌 LIGAR SISTEMA");

  if (data.tempoRestante > 0) {
    const h = Math.floor(data.tempoRestante / 60), m = data.tempoRestante % 60;
    setText("tempoInfo", `Resta: ${h}h ${m}min`);
  } else {
    setText("tempoInfo", "Modo atual");
  }

  setIndicador("indResistencia", data.resistencia);
  setIndicador("indVentoinha",   data.ventoinha);

  if (data.historico) {
    chart.data.datasets[0].data = data.historico.temp;
    chart.data.datasets[1].data = data.historico.umid;
    chart.update("none");
  }

  atualizarBalanca(data);
}

// ── Controles originais ────────────────────────────────────────
async function toggleSistema()    { await postCmd("/api/comando", { cmd: "TOGGLE" }); }
async function mudarModo(modo)    { await postCmd("/api/comando", { cmd: modo }); }
async function selecionarMaterial() {
  const idx = document.getElementById("material").value;
  await postCmd("/api/comando", { cmd: "MATERIAL", valor: parseInt(idx) });
}
async function resetarValidade() {
  if (!confirm("Deseja resetar o contador de validade?")) return;
  await postCmd("/api/comando", { cmd: "RESET" });
}

// ── Balança ────────────────────────────────────────────────────
function atualizarBalanca(data) {
  if (data.pesoAtual === undefined) return;

  const g   = Math.max(0, parseFloat(data.pesoAtual  ?? 0));
  const mx  = parseFloat(data.pesoMaximo ?? 0);
  const pct = mx > 0 ? Math.min(100, (g / mx) * 100) : 0;
  const low = pct <= 15;

  const gEl = document.getElementById("bal-gramas");
  if (gEl) {
    gEl.textContent = g.toFixed(0);
    gEl.classList.toggle("low",   low && pct > 0);
    gEl.classList.toggle("empty", pct === 0 && mx > 0);
  }

  const pEl = document.getElementById("bal-pct");
  if (pEl) { pEl.textContent = pct.toFixed(1) + "%"; pEl.classList.toggle("low", low); }

  const bEl = document.getElementById("bal-barra");
  if (bEl) { bEl.style.width = pct.toFixed(1) + "%"; bEl.classList.toggle("low", low); }

  setText("bal-max-label", mx > 0 ? mx.toFixed(0) + " g" : "--- g");
  setText("bal-tara", parseFloat(data.tara ?? 0).toFixed(1) + " g");

  const badge = document.getElementById("balanca-badge");
  if (badge) {
    if      (pct > 25)  { badge.textContent = "OK";      badge.className = "badge-status ok"; }
    else if (pct > 10)  { badge.textContent = "ATENÇÃO"; badge.className = "badge-status warning"; }
    else if (pct > 0)   { badge.textContent = "CRÍTICO"; badge.className = "badge-status error"; }
    else if (mx > 0)    { badge.textContent = "VAZIO";   badge.className = "badge-status error"; }
  }
}

async function balDefinirMax() {
  const inp = document.getElementById("bal-inp-max");
  const v   = parseFloat(inp ? inp.value : 0);
  if (!v || v <= 0 || v > 5000) { alert("Informe um valor entre 1 e 5000 gramas."); return; }
  const ok = await postCmd("/api/pesomax", { pesoMaximo: v });
  if (ok && inp) inp.value = "";
  await fetchData();
}

async function balFazerTara() {
  if (!confirm("Coloque o CARRETEL VAZIO na balança e confirme.")) return;
  await postCmd("/api/tara", {});
  await fetchData();
}

// ── Helpers ────────────────────────────────────────────────────
function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}

function setIndicador(id, ligado) {
  const el = document.getElementById(id);
  if (el) el.className = "indicator " + (ligado ? "on" : "off");
}

async function postCmd(rota, body) {
  try {
    const resp = await fetch(API_BASE + rota, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    });
    const data = await resp.json();
    return !!data.success;
  } catch (e) {
    console.warn("[Estufa] POST falhou:", e.message);
    return false;
  }
}

// ── Init ───────────────────────────────────────────────────────
window.addEventListener("DOMContentLoaded", () => {
  initChart();
  fetchData();
  setInterval(fetchData, 2000);
});
