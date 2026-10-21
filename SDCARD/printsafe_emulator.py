#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
═══════════════════════════════════════════════════════════════
 PrintSafe v1.0 — Emulador dos endpoints HTTP (ESP32)
═══════════════════════════════════════════════════════════════
 Reproduz o servidor web do PrintSafe.ino usando apenas a
 biblioteca padrão do Python 3.8+.

 Rotas emuladas (iguais às do firmware):
   GET  /  /index.html  /style.css  /script.js   (pasta ./web = /sdcard/web)
   GET  /api/data
   POST /api/comando    {"cmd":"TOGGLE|MATERIAL|SECAGEM|ARMAZENAMENTO|RESET","valor":n}
   POST /api/pesomax    {"pesoMaximo":1..5000}
   POST /api/tara
   POST /api/calibrar   {"hxScale":x} e/ou {"pesoReferencia":g}
   OPTIONS nas 4 rotas POST (CORS)
   qualquer outra rota /api/*  → 404 JSON ; demais → tenta a pasta web

 Também simula: PID_v1, relés, DHT22, HX711, histórico de 60 pontos,
 contagem de secagem/validade, encoder (menu) e a "Preferences".

 Uso:  python printsafe_emulator.py [--porta 8080] [--velocidade 60]
       (digite "ajuda" no console para ver os comandos de simulação)
═══════════════════════════════════════════════════════════════
"""

import argparse
import json
import math
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

# ─────────────────────────────────────────────────────────────
# Constantes do firmware
# ─────────────────────────────────────────────────────────────
# (nome, tempSecagem, tempArmazenamento, umidadeAlvo, tempoSecagem[min], validadeInicial[dias])
PERFIS = [
    ("PLA",   50, 28, 20, 300, 730),
    ("PETG",  65, 30, 15, 420, 730),
    ("ABS",   70, 40, 10, 480, 365),
    ("Nylon", 80, 45, 10, 720, 180),
    ("TPU",   50, 25, 20, 240, 365),
]
MAX_HIST = 60
MENU_ITENS = ["Ligar / Desligar", "Secar", "Armazenar", "Zerar Tara", "Cancelar"]

# Modelo do HX711: contagens brutas = RAW_ZERO + ESCALA_REAL * massa(g)
RAW_ZERO = 123456
HX_ESCALA_REAL = 420.0

MIME = {
    ".html": "text/html; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".ico":  "image/x-icon",
    ".png":  "image/png",
    ".jpg":  "image/jpeg",
    ".jpeg": "image/jpeg",
    ".svg":  "image/svg+xml",
}


def mime_type(caminho):
    for ext, tipo in MIME.items():
        if caminho.endswith(ext):
            return tipo
    return "application/octet-stream"


def c_round1(x):
    """round(x*10)/10.0 como em C (arredonda .5 para longe do zero)."""
    v = x * 10.0
    return (math.floor(v + 0.5) if v >= 0 else math.ceil(v - 0.5)) / 10.0


def numero(v):
    """Equivalente a `doc[k] | 0.0f` do ArduinoJson: só aceita número."""
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return float(v)
    return 0.0


def hora():
    return time.strftime("%H:%M:%S")


# ─────────────────────────────────────────────────────────────
# PID — porta fiel da PID_v1 (Brett Beauregard), P_ON_E, DIRECT
# ─────────────────────────────────────────────────────────────
class PID:
    def __init__(self, kp, ki, kd, amostra_ms=100, saida_min=0.0, saida_max=1.0):
        seg = amostra_ms / 1000.0
        self.kp, self.ki, self.kd = kp, ki * seg, kd / seg
        self.amostra_ms = amostra_ms
        self.saida_min, self.saida_max = saida_min, saida_max
        self.soma = 0.0
        self.ultima_entrada = 0.0
        self.ultimo_tempo = None

    def _lim(self, v):
        return max(self.saida_min, min(self.saida_max, v))

    def compute(self, setpoint, entrada, agora_ms):
        if self.ultimo_tempo is not None and agora_ms - self.ultimo_tempo < self.amostra_ms:
            return None
        erro = setpoint - entrada
        d_entrada = entrada - self.ultima_entrada
        self.soma = self._lim(self.soma + self.ki * erro)
        saida = self._lim(self.kp * erro + self.soma - self.kd * d_entrada)
        self.ultima_entrada = entrada
        self.ultimo_tempo = agora_ms
        return saida


# ─────────────────────────────────────────────────────────────
# Estufa — estado do firmware + física simplificada
# ─────────────────────────────────────────────────────────────
class Estufa:
    def __init__(self, velocidade=1.0, arq_prefs=None, novo=False, ligado=False):
        self.lock = threading.RLock()
        self.velocidade = float(velocidade)
        self.arq_prefs = Path(arq_prefs) if arq_prefs else None

        prefs = {}
        if self.arq_prefs and not novo and self.arq_prefs.exists():
            try:
                prefs = json.loads(self.arq_prefs.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                prefs = {}
        self.prefs = prefs

        # millis() simulado (começa como se o boot tivesse levado 6 s)
        self.ms = 6000.0
        self.t_ult_sensor = self.ms - 2000
        self.t_ult_balanca = self.ms - 500

        # Preferences
        n = len(PERFIS)
        self.material_idx = min(max(int(prefs.get("material", 0)), 0), n - 1)
        self.dias = int(prefs.get("dias", PERFIS[self.material_idx][5]))
        self.peso_max = float(prefs.get("pesoMax", 1000.0))
        self.tara = float(prefs.get("tara", 0.0))
        self.hx_scale = float(prefs.get("hxScale", 420.0))
        self.hx_offset = int(prefs.get("hxOffset", RAW_ZERO))

        # Estado do sistema
        self.modo_secagem = False
        self.sistema_ativo = bool(ligado)
        self.sensores_ok = False
        self.ts_secagem = 0
        self.ts_validade = 0
        self.temperatura = 0.0
        self.umidade = 0.0
        self.peso_atual = 0.0
        self.pid_setpoint = 0.0
        self.pid_output = 0.0
        self.pid = PID(2.0, 5.0, 1.0)
        self.rele_resist = False
        self.rele_vent = False
        self.hist_temp = [0.0] * MAX_HIST
        self.hist_umid = [0.0] * MAX_HIST
        self.hist_idx = 0

        # Encoder
        self.enc_menu = False
        self.menu_idx = 0

        # Ambiente simulado
        self.amb_t = 25.0
        self.amb_h = 55.0
        self.t_cam = 25.0
        self.umid_extra = 8.0           # umidade liberada pelo filamento
        self.h_cam = min(100.0, self.amb_h + self.umid_extra)
        self.massa_g = 850.0            # carretel sobre a balança

        # Falhas simuladas
        self.dht_ok = True
        self.balanca_ok = True
        self.sd_ok = True

        self._parar = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._passo()

    # ── infraestrutura ───────────────────────────────────────
    def iniciar(self):
        self._thread.start()

    def parar(self):
        self._parar.set()

    def log(self, msg):
        print(f"[{hora()}] {msg}", flush=True)

    def _loop(self):
        ultimo = time.monotonic()
        while not self._parar.is_set():
            time.sleep(0.05)
            agora = time.monotonic()
            dt = agora - ultimo
            ultimo = agora
            with self.lock:
                self.ms += dt * 1000.0 * self.velocidade
                self._passo()

    def _passo(self):
        while self.ms - self.t_ult_sensor >= 2000:
            self.t_ult_sensor += 2000
            self._tick_sensor()
        while self.ms - self.t_ult_balanca >= 500:
            self.t_ult_balanca += 500
            self._tick_balanca()

    def _salvar(self, **kw):
        self.prefs.update(kw)
        if self.arq_prefs:
            try:
                self.arq_prefs.write_text(json.dumps(self.prefs, indent=2), encoding="utf-8")
            except OSError:
                pass

    def _salvar_material(self):
        self._salvar(material=self.material_idx, dias=self.dias)

    # ── física simplificada ──────────────────────────────────
    @staticmethod
    def _es(t):
        return 6.112 * math.exp(17.62 * t / (243.12 + t))

    def _fisica(self, dt):
        aquec = 0.075 if self.rele_resist else 0.0
        perda = (0.0008 + (0.0004 if self.rele_vent else 0.0)) * (self.t_cam - self.amb_t)
        self.t_cam += (aquec - perda) * dt
        if self.rele_vent:
            self.umid_extra *= math.exp(-dt / 1500.0)
        eq = self.amb_h * self._es(self.amb_t) / self._es(self.t_cam)
        alvo = min(100.0, max(0.0, eq + self.umid_extra))
        self.h_cam += (alvo - self.h_cam) * (1.0 - math.exp(-dt / 120.0))

    # ── loop() do firmware: sensores + controle (2 s) ────────
    def _tick_sensor(self):
        self._fisica(2.0)
        if self.dht_ok:
            t = round(self.t_cam + random.gauss(0, 0.08), 1)
            h = round(min(100.0, max(0.0, self.h_cam + random.gauss(0, 0.4))), 1)
        else:
            t = h = float("nan")
        if not (math.isnan(t) or math.isnan(h)) and -40.0 < t < 125.0 and 0.0 <= h <= 100.0:
            self.sensores_ok = True
            self.temperatura, self.umidade = t, h
            self.hist_temp[self.hist_idx] = t
            self.hist_umid[self.hist_idx] = h
            self.hist_idx = (self.hist_idx + 1) % MAX_HIST
        else:
            if self.sensores_ok:
                self.log("[DHT22] Falha — estado seguro")
            self.sensores_ok = False
            self.desligar_tudo()
        self.controlar_sistema()
        self.verificar_validade()

    # ── balança (500 ms) ─────────────────────────────────────
    def _raw_medio(self, n=1):
        soma = sum(RAW_ZERO + HX_ESCALA_REAL * self.massa_g + random.gauss(0, 120) for _ in range(n))
        return int(round(soma / n))

    def _tick_balanca(self):
        if self.balanca_ok:
            bruto = (self._raw_medio(3) - self.hx_offset) / self.hx_scale
            self.peso_atual = 0.0 if bruto < 0.0 else bruto

    # ── controle ─────────────────────────────────────────────
    def desligar_tudo(self):
        self.rele_resist = False
        self.rele_vent = False

    def controlar_sistema(self):
        if not self.sistema_ativo or not self.sensores_ok:
            self.desligar_tudo()
            return
        p = PERFIS[self.material_idx]
        self.pid_setpoint = float(p[1] if self.modo_secagem else p[2])
        saida = self.pid.compute(self.pid_setpoint, self.temperatura, int(self.ms))
        if saida is not None:
            self.pid_output = saida
        self.rele_resist = self.pid_output > 0.5
        self.rele_vent = (self.umidade > p[3] + 5
                          or self.temperatura > self.pid_setpoint + 3
                          or self.modo_secagem)
        if self.modo_secagem and self.ts_secagem > 0:
            decorrido = int((self.ms - self.ts_secagem) // 60000)
            if decorrido >= p[4]:
                self.finalizar_secagem()

    def finalizar_secagem(self):
        self.modo_secagem = False
        self.ts_secagem = 0
        self.dias = PERFIS[self.material_idx][5]
        self.ts_validade = int(self.ms)
        self._salvar(dias=self.dias)
        self.log("[PrintSafe] Secagem finalizada.")

    def verificar_validade(self):
        if not self.sistema_ativo or self.modo_secagem:
            return
        if self.ts_validade == 0:
            self.ts_validade = int(self.ms)
            return
        if self.ms - self.ts_validade >= 86400000:
            self.ts_validade = int(self.ms)
            if self.dias > 0:
                self.dias -= 1
                self._salvar(dias=self.dias)

    # ── ações compartilhadas (web / encoder) ─────────────────
    def alternar_sistema(self):
        self.sistema_ativo = not self.sistema_ativo
        if not self.sistema_ativo:
            self.desligar_tudo()

    def iniciar_secagem(self):
        self.modo_secagem = True
        self.ts_secagem = int(self.ms)

    def iniciar_armazenamento(self):
        self.modo_secagem = False
        self.ts_secagem = 0

    def tarar(self):
        if not self.balanca_ok:
            return False
        self.hx_offset = self._raw_medio(10)
        self.tara = 0.0
        self.peso_atual = 0.0
        self._salvar(hxOffset=self.hx_offset, tara=0.0)
        self.log(f"[HX711] Tara — offset={self.hx_offset}")
        return True

    # ── rotas web ────────────────────────────────────────────
    def json_data(self):
        with self.lock:
            p = PERFIS[self.material_idx]
            d = {
                "sensoresOK": self.sensores_ok,
                "temp": c_round1(self.temperatura),
                "umid": c_round1(self.umidade),
                "setpoint": self.pid_setpoint,
                "material": p[0],
                "materialIdx": self.material_idx,
                "modo": "Secagem" if self.modo_secagem else "Armazenamento",
                "ativo": self.sistema_ativo,
                "validade": self.dias,
                "resistencia": self.rele_resist,
                "ventoinha": self.rele_vent,
            }
            if self.modo_secagem and self.ts_secagem > 0:
                rest = p[4] - int((self.ms - self.ts_secagem) // 60000)
                d["tempoRestante"] = max(0, rest)
            ordem = [(self.hist_idx + i) % MAX_HIST for i in range(MAX_HIST)]
            d["historico"] = {
                "temp": [self.hist_temp[i] for i in ordem],
                "umid": [self.hist_umid[i] for i in ordem],
            }
            pct = min(100.0, self.peso_atual / self.peso_max * 100.0) if self.peso_max > 0 else 0.0
            d["pesoAtual"] = c_round1(self.peso_atual)
            d["pesoMaximo"] = self.peso_max
            d["pesoPercentual"] = c_round1(pct)
            d["tara"] = self.tara
            d["hxScale"] = self.hx_scale
            return json.dumps(d, separators=(",", ":"), ensure_ascii=False)

    def comando(self, dados):
        cmd = dados.get("cmd")
        cmd = cmd if isinstance(cmd, str) else ""
        with self.lock:
            if cmd == "TOGGLE":
                self.alternar_sistema()
            elif cmd == "MATERIAL":
                v = dados.get("valor", 0)
                idx = v if isinstance(v, int) and not isinstance(v, bool) else 0
                if 0 <= idx < len(PERFIS):
                    self.material_idx = idx
                    self.dias = PERFIS[idx][5]
                    self._salvar_material()
            elif cmd == "SECAGEM":
                self.iniciar_secagem()
            elif cmd == "ARMAZENAMENTO":
                self.iniciar_armazenamento()
            elif cmd == "RESET":
                self.dias = PERFIS[self.material_idx][5]
                self.ts_validade = int(self.ms)
                self._salvar(dias=self.dias)
            self.log(f"[WEB] comando {cmd or '(vazio)'} → ativo={self.sistema_ativo} "
                     f"modo={'Secagem' if self.modo_secagem else 'Armazenamento'} "
                     f"material={PERFIS[self.material_idx][0]}")

    def definir_peso_max(self, dados):
        """Retorna (codigo, corpo_json)."""
        v = numero(dados.get("pesoMaximo"))
        if 1 <= v <= 5000:
            with self.lock:
                self.peso_max = v
                self._salvar(pesoMax=v)
                return 200, '{"success":true,"pesoMaximo":%.1f}' % self.peso_max
        return 400, '{"success":false,"error":"Fora do range 1-5000"}'

    def rota_tara(self):
        with self.lock:
            if not self.balanca_ok:
                return 503, '{"success":false,"error":"Balanca indisponivel"}'
            self.tarar()
            return 200, '{"success":true,"tara":0}'

    def calibrar(self, dados):
        with self.lock:
            if "hxScale" in dados:
                v = numero(dados["hxScale"])
                if v != 0:
                    self.hx_scale = v
                    self._salvar(hxScale=v)
            if "pesoReferencia" in dados and self.balanca_ok:
                ref = numero(dados["pesoReferencia"])
                if ref > 0:
                    raw = self._raw_medio(10) - self.hx_offset      # get_value(10)
                    if raw != 0:
                        self.hx_scale = raw / ref
                        self._salvar(hxScale=self.hx_scale)
                        self.log(f"[HX711] Calib: raw={raw} ref={ref:.1f} scale={self.hx_scale:.3f}")
            return '{"success":true,"hxScale":%.3f}' % self.hx_scale

    # ── encoder (simulado pelo console) ──────────────────────
    def _mostrar_menu(self):
        linhas = [("► " if i == self.menu_idx else "  ") + t for i, t in enumerate(MENU_ITENS)]
        self.log("[ENC] Menu:  " + " | ".join(linhas))

    def girar(self, direcao):
        with self.lock:
            if not self.enc_menu:
                self.material_idx = (self.material_idx + direcao) % len(PERFIS)
                self.dias = PERFIS[self.material_idx][5]
                self._salvar_material()
                self.log(f"[ENC] Material → {PERFIS[self.material_idx][0]}")
            else:
                self.menu_idx = (self.menu_idx + direcao) % len(MENU_ITENS)
                self._mostrar_menu()

    def botao(self):
        with self.lock:
            if not self.enc_menu:
                self.enc_menu = True
                self.menu_idx = 0
                self.log("[ENC] Menu aberto")
                self._mostrar_menu()
                return
            self.enc_menu = False
            i = self.menu_idx
            if i == 0:
                self.alternar_sistema()
                self.log(f"[MENU] Sistema → {'ON' if self.sistema_ativo else 'OFF'}")
            elif i == 1:
                self.iniciar_secagem()
                self.log("[MENU] Modo → Secagem")
            elif i == 2:
                self.iniciar_armazenamento()
                self.log("[MENU] Modo → Armazenamento")
            elif i == 3:
                if not self.tarar():
                    self.log("[MENU] Balança indisponível")
            else:
                self.log("[MENU] Cancelado")

    # ── resumo para o console ────────────────────────────────
    def resumo(self):
        with self.lock:
            p = PERFIS[self.material_idx]
            seg = self.ms / 1000.0
            linhas = [
                f"Tempo simulado : {int(seg // 3600):02d}h{int(seg % 3600 // 60):02d}m{int(seg % 60):02d}s  (x{self.velocidade:g})",
                f"Câmara         : {self.temperatura:.1f} °C  UR {self.umidade:.1f} %   "
                f"(real {self.t_cam:.1f} °C / {self.h_cam:.1f} %)",
                f"Ambiente       : {self.amb_t:.1f} °C  UR {self.amb_h:.1f} %",
                f"Controle       : {'LIGADO' if self.sistema_ativo else 'DESLIGADO'} | "
                f"{'Secagem' if self.modo_secagem else 'Armazenamento'} | {p[0]} | "
                f"SP {self.pid_setpoint:.1f} °C | PID {self.pid_output:.2f}",
                f"Saídas         : resistência {'ON' if self.rele_resist else 'OFF'} | "
                f"ventoinha {'ON' if self.rele_vent else 'OFF'}",
                f"Balança        : {self.peso_atual:.1f} g / {self.peso_max:.0f} g "
                f"(massa real {self.massa_g:.0f} g) | hxScale {self.hx_scale:.3f}",
                f"Validade       : {self.dias} dias",
                f"Falhas         : DHT22 {'ok' if self.dht_ok else 'FALHA'} | "
                f"balança {'ok' if self.balanca_ok else 'FALHA'} | SD {'ok' if self.sd_ok else 'FALHA'}",
            ]
            if self.modo_secagem and self.ts_secagem > 0:
                rest = max(0, p[4] - int((self.ms - self.ts_secagem) // 60000))
                linhas.append(f"Secagem        : faltam {rest} min")
            return "\n".join(linhas)


# ─────────────────────────────────────────────────────────────
# Servidor HTTP (mesmas rotas e respostas do WebServer do ESP32)
# ─────────────────────────────────────────────────────────────
class Config:
    estufa = None
    web_dir = Path("web")
    latencia = 0
    verbose = False


ROTAS = {
    ("GET", "/"):             "rota_index",
    ("GET", "/index.html"):   "rota_index",
    ("GET", "/style.css"):    "rota_css",
    ("GET", "/script.js"):    "rota_js",
    ("GET", "/api/data"):     "rota_data",
    ("POST", "/api/comando"): "rota_comando",
    ("POST", "/api/pesomax"): "rota_pesomax",
    ("POST", "/api/tara"):    "rota_tara",
    ("POST", "/api/calibrar"): "rota_calibrar",
    ("OPTIONS", "/api/comando"):  "rota_options",
    ("OPTIONS", "/api/pesomax"):  "rota_options",
    ("OPTIONS", "/api/tara"):     "rota_options",
    ("OPTIONS", "/api/calibrar"): "rota_options",
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def send_response(self, code, message=None):      # sem cabeçalhos Server/Date, como no ESP32
        self.send_response_only(code, message)

    def log_message(self, *args):
        pass

    # ── envio ────────────────────────────────────────────────
    def _responder(self, codigo, tipo, corpo="", cors=False):
        dados = corpo.encode("utf-8") if isinstance(corpo, str) else corpo
        self._codigo = codigo
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(dados)))
        if cors:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(dados)

    def _json_resp(self, codigo, texto, tipo="application/json"):
        self._responder(codigo, tipo, texto, cors=True)

    def _ler_json(self, corpo):
        if not corpo:
            return "sem_corpo", None
        try:
            dados = json.loads(corpo.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return "invalido", None
        return "ok", dados if isinstance(dados, dict) else {}

    # ── arquivos (SD emulado) ────────────────────────────────
    def _servir_sd(self, uri):
        if not Config.estufa.sd_ok:
            return self._responder(503, "text/plain", "SD indisponivel")
        sub = "/index.html" if uri in ("/", "") else uri
        caminho_fw = "/sdcard/web" + sub
        base = Config.web_dir.resolve()
        alvo = (base / sub.lstrip("/")).resolve()
        try:
            alvo.relative_to(base)
        except ValueError:
            return self._responder(404, "text/plain", "Nao encontrado: " + caminho_fw)
        if not alvo.is_file():
            return self._responder(404, "text/plain", "Nao encontrado: " + caminho_fw)
        self._responder(200, mime_type(caminho_fw), alvo.read_bytes())

    def rota_index(self, corpo): self._servir_sd("/index.html")
    def rota_css(self, corpo):   self._servir_sd("/style.css")
    def rota_js(self, corpo):    self._servir_sd("/script.js")

    # ── API ──────────────────────────────────────────────────
    def rota_options(self, corpo):
        self._responder(200, "text/plain", "", cors=True)

    def rota_data(self, corpo):
        self._json_resp(200, Config.estufa.json_data(), "application/json; charset=utf-8")

    def rota_comando(self, corpo):
        estado, dados = self._ler_json(corpo)
        if estado == "sem_corpo":
            return self._json_resp(400, '{"success":false}')
        if estado == "invalido":
            return self._json_resp(400, '{"success":false,"error":"JSON invalido"}')
        Config.estufa.comando(dados)
        self._json_resp(200, '{"success":true}')

    def rota_pesomax(self, corpo):
        estado, dados = self._ler_json(corpo)
        if estado != "ok":
            return self._json_resp(400, '{"success":false}')
        codigo, texto = Config.estufa.definir_peso_max(dados)
        self._json_resp(codigo, texto)

    def rota_tara(self, corpo):
        codigo, texto = Config.estufa.rota_tara()
        self._json_resp(codigo, texto)

    def rota_calibrar(self, corpo):
        estado, dados = self._ler_json(corpo)
        if estado != "ok":
            return self._json_resp(400, '{"success":false}')
        self._json_resp(200, Config.estufa.calibrar(dados))

    def _nao_encontrado(self, caminho):
        if caminho.startswith("/api/"):
            return self._json_resp(404, '{"error":"Rota nao encontrada"}')
        self._servir_sd(caminho)

    # ── despacho ─────────────────────────────────────────────
    def _tratar(self):
        t0 = time.perf_counter()
        self._codigo = 0
        caminho = unquote(urlsplit(self.path).path)
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        corpo = self.rfile.read(n) if n > 0 else b""
        if Config.latencia:
            time.sleep(Config.latencia / 1000.0)
        nome = ROTAS.get((self.command, caminho))
        if nome:
            getattr(self, nome)(corpo)
        else:
            self._nao_encontrado(caminho)
        if Config.verbose or caminho != "/api/data":
            ms = (time.perf_counter() - t0) * 1000.0
            print(f"[{hora()}] [HTTP] {self.command:<7} {caminho}  → {self._codigo}  ({ms:.0f} ms)", flush=True)
            if Config.verbose and corpo:
                print(f"           corpo: {corpo.decode('utf-8', 'replace')}", flush=True)

    do_GET = do_POST = do_OPTIONS = do_HEAD = do_PUT = do_DELETE = do_PATCH = _tratar


class Servidor(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError, ConnectionAbortedError)):
            return
        super().handle_error(request, client_address)


# ─────────────────────────────────────────────────────────────
# Console de simulação
# ─────────────────────────────────────────────────────────────
AJUDA = """
Comandos de simulação:
  status                 resumo do estado atual
  temp <°C>              força a temperatura real da câmara
  umid <%>               força a umidade real da câmara
  amb <°C>               temperatura ambiente
  ambumid <%>            umidade ambiente
  massa <g>              massa do carretel sobre a balança
  dht on|off             simula falha do DHT22 (estado seguro)
  balanca on|off         simula balança indisponível (HX711)
  sd on|off              simula cartão SD ausente (arquivos → 503)
  velocidade <x>         acelera o tempo simulado (ex.: 60 = 1 min/s)
  girar + | -            gira o encoder (material ou navegação no menu)
  botao                  pressiona o encoder (abre menu / confirma)
  ajuda                  esta lista
  sair                   encerra o emulador
"""


def _onoff(arg):
    if arg in ("on", "1", "sim", "ok"):
        return True
    if arg in ("off", "0", "nao", "não", "falha"):
        return False
    raise ValueError("use on ou off")


def executar_comando(est, linha):
    partes = linha.strip().split()
    if not partes:
        return True
    cmd, args = partes[0].lower(), partes[1:]
    try:
        with est.lock:
            if cmd in ("sair", "exit", "quit"):
                return False
            elif cmd in ("ajuda", "help", "?"):
                print(AJUDA)
            elif cmd == "status":
                print(est.resumo())
            elif cmd == "temp":
                est.t_cam = float(args[0].replace(",", "."))
                print(f"Temperatura real da câmara = {est.t_cam:.1f} °C")
            elif cmd == "umid":
                est.h_cam = min(100.0, max(0.0, float(args[0].replace(",", "."))))
                est.umid_extra = 0.0
                print(f"Umidade real da câmara = {est.h_cam:.1f} %")
            elif cmd == "amb":
                est.amb_t = float(args[0].replace(",", "."))
                print(f"Temperatura ambiente = {est.amb_t:.1f} °C")
            elif cmd == "ambumid":
                est.amb_h = min(100.0, max(0.0, float(args[0].replace(",", "."))))
                print(f"Umidade ambiente = {est.amb_h:.1f} %")
            elif cmd == "massa":
                est.massa_g = max(0.0, float(args[0].replace(",", ".")))
                print(f"Massa sobre a balança = {est.massa_g:.0f} g")
            elif cmd == "dht":
                est.dht_ok = _onoff(args[0])
                print(f"DHT22 {'OK' if est.dht_ok else 'em FALHA'}")
            elif cmd in ("balanca", "balança"):
                est.balanca_ok = _onoff(args[0])
                print(f"Balança {'OK' if est.balanca_ok else 'em FALHA'}")
            elif cmd == "sd":
                est.sd_ok = _onoff(args[0])
                print(f"SD {'OK' if est.sd_ok else 'em FALHA'}")
            elif cmd in ("velocidade", "vel"):
                v = float(args[0].replace(",", "."))
                if not 0.1 <= v <= 10000:
                    raise ValueError("use um valor entre 0.1 e 10000")
                est.velocidade = v
                print(f"Velocidade do tempo simulado = x{v:g}")
            elif cmd == "girar":
                est.girar(+1 if args and args[0] in ("+", "1", "dir", "horario") else -1)
            elif cmd in ("botao", "botão", "btn"):
                est.botao()
            else:
                print(f"Comando desconhecido: {cmd}  (digite 'ajuda')")
    except (IndexError, ValueError) as e:
        print(f"Argumento inválido ({e}). Digite 'ajuda'.")
    return True


# ─────────────────────────────────────────────────────────────
# main
# ─────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Emulador dos endpoints do PrintSafe (ESP32).")
    ap.add_argument("--host", default="127.0.0.1", help="endereço de escuta (use 0.0.0.0 para acessar da rede)")
    ap.add_argument("--porta", type=int, default=8080, help="porta HTTP (padrão 8080; o ESP32 usa 80)")
    ap.add_argument("--velocidade", type=float, default=1.0, help="aceleração do tempo simulado (padrão 1)")
    ap.add_argument("--web", default="web", help="pasta que emula /sdcard/web (padrão ./web)")
    ap.add_argument("--prefs", default="printsafe_prefs.json", help="arquivo que emula a Preferences")
    ap.add_argument("--novo", action="store_true", help="ignora a Preferences salva e começa do zero")
    ap.add_argument("--ligado", action="store_true", help="inicia com o sistema já LIGADO")
    ap.add_argument("--sem-sd", action="store_true", help="inicia simulando SD ausente")
    ap.add_argument("--latencia", type=int, default=0, help="atraso artificial por requisição, em ms")
    ap.add_argument("-v", "--verbose", action="store_true", help="loga também /api/data e o corpo dos POSTs")
    a = ap.parse_args()

    for fluxo in (sys.stdout, sys.stderr):
        try:
            fluxo.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    base = Path(__file__).resolve().parent
    web_dir = Path(a.web) if Path(a.web).is_absolute() else base / a.web
    web_dir.mkdir(parents=True, exist_ok=True)
    prefs = Path(a.prefs) if Path(a.prefs).is_absolute() else base / a.prefs

    est = Estufa(a.velocidade, prefs, a.novo, a.ligado)
    est.sd_ok = not a.sem_sd
    Config.estufa, Config.web_dir = est, web_dir
    Config.latencia, Config.verbose = a.latencia, a.verbose

    print("\n╔══════════════════════════════════════╗")
    print("║  PrintSafe v1.0 — Emulador (Python)  ║")
    print("╚══════════════════════════════════════╝")
    print(f"[PREFS] mat={PERFIS[est.material_idx][0]} dias={est.dias} pesoMax={est.peso_max:.0f} "
          f"scale={est.hx_scale:.3f}")
    print(f"[SD]    {'OK' if est.sd_ok else 'Falha (simulada)'} — arquivos em {web_dir}")

    try:
        srv = Servidor((a.host, a.porta), Handler)
    except OSError as e:
        print(f"\n[ERRO] Não foi possível abrir {a.host}:{a.porta} — {e}")
        print("       Tente outra porta, por exemplo:  --porta 8081")
        sys.exit(1)

    est.iniciar()
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    exibir = "localhost" if a.host in ("127.0.0.1", "0.0.0.0") else a.host
    print(f"[HTTP]  Pronto → http://{exibir}:{a.porta}/   (dados: /api/data)")
    print("        Digite 'ajuda' para ver os comandos de simulação.\n")

    try:
        while True:
            try:
                linha = input("emu> ")
            except EOFError:
                threading.Event().wait()        # sem console interativo: só serve HTTP
            if not executar_comando(est, linha):
                break
    except KeyboardInterrupt:
        pass
    print("\nEncerrando...")
    est.parar()
    srv.shutdown()


if __name__ == "__main__":
    main()
