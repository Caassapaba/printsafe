#!/usr/bin/env python3
"""
Estufa Inteligente — Emulador Server
Simula o ESP32 para desenvolvimento frontend sem hardware.

Uso:
    python emulator_server.py
    http://localhost:8080
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
import json, random, math, time, os, sys
from datetime import datetime
from urllib.parse import urlparse

WEB_DIR = os.path.dirname(os.path.abspath(__file__))

PERFIS = [
    {"nome": "PLA",   "tempSecagem": 50, "tempArm": 28, "umidAlvo": 20, "tempoSec": 300, "validade": 730},
    {"nome": "PETG",  "tempSecagem": 65, "tempArm": 30, "umidAlvo": 15, "tempoSec": 420, "validade": 730},
    {"nome": "ABS",   "tempSecagem": 70, "tempArm": 40, "umidAlvo": 10, "tempoSec": 480, "validade": 365},
    {"nome": "Nylon", "tempSecagem": 80, "tempArm": 45, "umidAlvo": 10, "tempoSec": 720, "validade": 180},
    {"nome": "TPU",   "tempSecagem": 50, "tempArm": 25, "umidAlvo": 20, "tempoSec": 240, "validade": 365},
]


class EstufaHandler(BaseHTTPRequestHandler):

    state = {
        "temperatura": 25.0, "umidade": 55.0,
        "materialIdx": 0, "modoSecagem": False,
        "sistemaAtivo": False, "diasRestantes": 730,
        "tsSecagem": None, "setpoint": 28.0,
        "resistencia": False, "ventoinha": False,
        "pesoAtual": 750.0, "pesoMaximo": 1000.0, "tara": 0.0,
        "histTemp": [0.0]*60, "histUmid": [0.0]*60, "histIdx": 0,
        "_tempAlvo": 28.0, "_tick": 0,
    }

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/data":
            self._evoluir(); self._send_json(self._build_data())
        elif path in ("/", "/index.html"): self._serve_static("index.html")
        elif path == "/style.css":         self._serve_static("style.css")
        elif path == "/script.js":         self._serve_static("script.js")
        else: self._send_json({"error": "not found"}, 404)

    def do_POST(self):
        path = urlparse(self.path).path
        body = self._read_body()
        if   path == "/api/comando": self._handle_comando(body)
        elif path == "/api/pesomax": self._handle_pesomax(body)
        elif path == "/api/tara":    self._handle_tara()
        else: self._send_json({"error": "not found"}, 404)

    def do_OPTIONS(self):
        self.send_response(200); self._cors(); self.end_headers()

    def _handle_comando(self, body):
        s = self.state
        cmd = body.get("cmd", "")
        if cmd == "TOGGLE":
            s["sistemaAtivo"] = not s["sistemaAtivo"]
            if not s["sistemaAtivo"]: s["resistencia"] = s["ventoinha"] = False
            self._log(f"TOGGLE → {'ON' if s['sistemaAtivo'] else 'OFF'}")
        elif cmd == "MATERIAL":
            idx = int(body.get("valor", 0))
            if 0 <= idx < len(PERFIS):
                s["materialIdx"] = idx
                s["diasRestantes"] = PERFIS[idx]["validade"]
                self._log(f"MATERIAL → {PERFIS[idx]['nome']}")
        elif cmd == "SECAGEM":
            s["modoSecagem"] = True; s["tsSecagem"] = time.time()
            self._log("MODO → Secagem")
        elif cmd == "ARMAZENAMENTO":
            s["modoSecagem"] = False; s["tsSecagem"] = None
            self._log("MODO → Armazenamento")
        elif cmd == "RESET":
            s["diasRestantes"] = PERFIS[s["materialIdx"]]["validade"]
            self._log("Validade resetada")
        self._send_json({"success": True})

    def _handle_pesomax(self, body):
        v = float(body.get("pesoMaximo", 0))
        if 1 <= v <= 5000:
            self.state["pesoMaximo"] = v
            self._log(f"Peso máximo → {v:.0f} g")
            self._send_json({"success": True, "pesoMaximo": v})
        else:
            self._send_json({"success": False, "error": "Fora do range 1-5000"}, 400)

    def _handle_tara(self):
        raw = self.state["pesoAtual"] + self.state["tara"]
        self.state["tara"] = round(raw, 1)
        self.state["pesoAtual"] = 0.0
        self._log(f"Tara → {raw:.1f} g")
        self._send_json({"success": True, "tara": round(raw, 1)})

    def _evoluir(self):
        s = self.state; s["_tick"] += 1
        t = time.time(); p = PERFIS[s["materialIdx"]]
        sp = p["tempSecagem"] if s["modoSecagem"] else p["tempArm"]
        s["setpoint"] = sp
        if s["sistemaAtivo"]:
            if s["temperatura"] < sp: s["_tempAlvo"] = min(sp, s["_tempAlvo"] + 0.4); s["resistencia"] = True
            else: s["resistencia"] = False
            s["ventoinha"] = (s["umidade"] > p["umidAlvo"]+5 or s["temperatura"] > sp+3 or s["modoSecagem"])
            if s["ventoinha"]: s["_tempAlvo"] = max(20, s["_tempAlvo"] - 0.1)
        else:
            s["resistencia"] = s["ventoinha"] = False
            s["_tempAlvo"] = max(20, s["_tempAlvo"] - 0.05)
        s["temperatura"] = round(s["_tempAlvo"] + math.sin(t/15)*0.3 + random.uniform(-0.15, 0.15), 1)
        um = max(10, min(95, 80 - (s["temperatura"]-20)*0.8)) + random.uniform(-0.4, 0.4)
        s["umidade"] = round(um, 1)
        s["pesoAtual"] = round(max(0, s["pesoAtual"] + random.uniform(-0.4, 0.4)), 1)
        if s["modoSecagem"] and s["tsSecagem"]:
            if (time.time() - s["tsSecagem"])/60 >= p["tempoSec"]:
                s["modoSecagem"] = False; s["tsSecagem"] = None
                s["diasRestantes"] = p["validade"]
                self._log("Secagem finalizada")
        s["histTemp"][s["histIdx"]] = s["temperatura"]
        s["histUmid"][s["histIdx"]] = s["umidade"]
        s["histIdx"] = (s["histIdx"] + 1) % 60

    def _build_data(self):
        s = self.state; p = PERFIS[s["materialIdx"]]
        g = max(0, s["pesoAtual"]); mx = s["pesoMaximo"]
        pct = round(min(100, g/mx*100) if mx > 0 else 0, 1)
        tempo_rest = 0
        if s["modoSecagem"] and s["tsSecagem"]:
            tempo_rest = max(0, int(p["tempoSec"] - (time.time()-s["tsSecagem"])/60))
        idx = s["histIdx"]
        hist_temp = s["histTemp"][idx:] + s["histTemp"][:idx]
        hist_umid = s["histUmid"][idx:] + s["histUmid"][:idx]
        return {
            "temp": round(s["temperatura"],1), "umid": round(s["umidade"],1),
            "setpoint": round(s["setpoint"],1),
            "material": p["nome"], "materialIdx": s["materialIdx"],
            "modo": "Secagem" if s["modoSecagem"] else "Armazenamento",
            "ativo": s["sistemaAtivo"], "validade": s["diasRestantes"],
            "resistencia": s["resistencia"], "ventoinha": s["ventoinha"],
            "tempoRestante": tempo_rest,
            "historico": {"temp": hist_temp, "umid": hist_umid},
            "pesoAtual": round(g,1), "pesoMaximo": round(mx,1),
            "pesoPercentual": pct, "tara": round(s["tara"],1),
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        }

    def _serve_static(self, filename):
        filepath = os.path.join(WEB_DIR, filename)
        if not os.path.isfile(filepath):
            self._send_json({"error": f"Não encontrado: {filename}"}, 404); return
        mime = {".html":"text/html; charset=utf-8",".css":"text/css; charset=utf-8",
                ".js":"application/javascript; charset=utf-8"}.get(
                    os.path.splitext(filename)[1], "application/octet-stream")
        with open(filepath, "rb") as f: content = f.read()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(content)))
        self._cors(); self.end_headers(); self.wfile.write(content)

    def _send_json(self, data, code=200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self._cors(); self.end_headers(); self.wfile.write(body)

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0))
        if n == 0: return {}
        try: return json.loads(self.rfile.read(n).decode("utf-8"))
        except: return {}

    def _log(self, msg):
        print(f"  [{datetime.now().strftime('%H:%M:%S')}] {msg}")

    def log_message(self, *_): pass


def run(port=8080):
    for f in ("index.html", "style.css", "script.js"):
        if not os.path.isfile(os.path.join(WEB_DIR, f)):
            print(f"\n⚠️  '{f}' não encontrado na pasta do emulador.\n"); sys.exit(1)

    server = HTTPServer(("", port), EstufaHandler)
    print()
    print("=" * 58)
    print("  🌡️  Estufa Inteligente — Emulador Server")
    print("=" * 58)
    print(f"\n  Dashboard:  http://localhost:{port}")
    print(f"  API:        http://localhost:{port}/api/data\n")
    print("  POST /api/comando  → TOGGLE / MATERIAL / SECAGEM /")
    print("                       ARMAZENAMENTO / RESET")
    print("  POST /api/pesomax  → {\"pesoMaximo\": 1000}")
    print("  POST /api/tara     → tara da balança\n")
    print("  ⌨️  Ctrl+C para parar")
    print("=" * 58 + "\n")
    try: server.serve_forever()
    except KeyboardInterrupt: print("\n  🛑 Encerrado.\n"); server.shutdown()


if __name__ == "__main__":
    port = 8080
    if len(sys.argv) > 1:
        try: port = int(sys.argv[1])
        except: sys.exit("Uso: python emulator_server.py [porta]")
    run(port)
