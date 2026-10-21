/*
 * ═══════════════════════════════════════════════════════════════
 *  PrintSafe v1.0 — Estufa Inteligente para Filamentos
 *  Hardware: ESP32-S3 N16R8 (mesmo PCB do PrintSense v4.7)
 * ═══════════════════════════════════════════════════════════════
 *
 *  PINOUT (ESP32-S3 N16R8):
 *  ┌─────────────────────────────────────────────────────────┐
 *  │ GPIO 17  DHT22 DATA                                     │
 *  │ GPIO  4  HX711 DOUT                                     │
 *  │ GPIO  8  HX711 SCK                                      │
 *  │ GPIO 19  Relé Resistência (LOW liga)                    │
 *  │ GPIO 20  Relé Ventoinha   (LOW liga)                    │
 *  │ GPIO 38  LED WiFi/Status                                │
 *  │ GPIO 39  LED Resistência                                │
 *  │ GPIO 40  LED Ventoinha                                  │
 *  │ GPIO 10  SD SPI CS                                      │
 *  │ GPIO 11  SD SPI MOSI                                    │
 *  │ GPIO 12  SD SPI SCK                                     │
 *  │ GPIO 13  SD SPI MISO                                    │
 *  │ GPIO 15  I2C SDA LCD                                    │
 *  │ GPIO 16  I2C SCL LCD                                    │
 *  │ GPIO  7  Encoder CLK                                    │
 *  │ GPIO  6  Encoder DT                                     │
 *  │ GPIO  5  Encoder SW                                     │
 *  └─────────────────────────────────────────────────────────┘
 *  Pinos evitados no N16R8: GPIO 26-32 (flash/PSRAM), 33-37
 *  (PSRAM octal) e 0/3/45/46 (strapping) — a PSRAM pode ficar
 *  habilitada sem conflito. GPIO 19/20 são o USB nativo: com os
 *  relés neles, grave e depure pela porta UART da placa.
 *
 *  Bibliotecas (Library Manager):
 *    - DHT sensor library      (Adafruit)
 *    - HX711                   (bogde)
 *    - ArduinoJson             (Benoit Blanchon) v6+
 *    - PID_v1                  (Brett Beauregard)
 *    - LiquidCrystal_I2C       (Frank de Brabander)
 *
 *  LCD: 20x4 I2C — addr 0x27 (tente 0x3F se não acender)
 *
 *  Encoder — novo comportamento:
 *    GIRAR           → muda material diretamente (circular)
 *    PRESSIONAR 1x   → abre menu de ações:
 *                       ► Ligar / Desligar
 *                         Secar
 *                         Armazenar
 *                         Zerar Tara
 *    No menu: GIRAR  → navega entre opções
 *             PRESS  → confirma opção selecionada / cancela no último item
 *
 *  SD card: copiar index.html, style.css, script.js
 *           para /sdcard/web/
 * ═══════════════════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <HX711.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PID_v1.h>
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>

// ─────────────────────────────────────────────────────────────
// WiFi
// ─────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "SEU_WIFI";
const char* WIFI_PASSWORD = "SUA_SENHA";

// ─────────────────────────────────────────────────────────────
// Pinos
// ─────────────────────────────────────────────────────────────
#define DHT_PIN      17
#define DHT_TYPE     DHT22

#define HX711_DOUT    4
#define HX711_SCK     8

#define RELE_RESIST  19
#define RELE_VENT    20
#define RELE1_INV    true
#define RELE2_INV    true

#define LED_WIFI     38
#define LED_RESIST   39
#define LED_VENT     40

// SD via SPI (FSPI / SPI2)
#define SD_CS_PIN    GPIO_NUM_10
#define SD_MOSI_PIN  GPIO_NUM_11
#define SD_SCK_PIN   GPIO_NUM_12
#define SD_MISO_PIN  GPIO_NUM_13
#define SD_SPI_HOST  SPI2_HOST

#define I2C_SDA      15
#define I2C_SCL      16

#define ENC_CLK       7
#define ENC_DT        6
#define ENC_SW        5

// ─────────────────────────────────────────────────────────────
// LCD
// ─────────────────────────────────────────────────────────────
#define LCD_ADDR     0x27
#define LCD_COLS     20
#define LCD_ROWS      4

// ─────────────────────────────────────────────────────────────
// Encoder — debounce
// ─────────────────────────────────────────────────────────────
#define BTN_DEBOUNCE  50    // ms mínimo para considerar um clique válido

// ─────────────────────────────────────────────────────────────
// LCD — timing
// ─────────────────────────────────────────────────────────────
#define LCD_INTERVALO        1000
#define LCD_TEMPO_POR_PAGINA 4000
#define LCD_TOTAL_PAGINAS       3

// ─────────────────────────────────────────────────────────────
// Perfis de material
// ─────────────────────────────────────────────────────────────
struct Perfil {
  const char* nome;
  float tempSecagem;
  float tempArmazenamento;
  float umidadeAlvo;
  int   tempoSecagem;       // minutos
  int   validadeInicial;    // dias
};

const Perfil PERFIS[] = {
  { "PLA",   50, 28, 20, 300, 730 },
  { "PETG",  65, 30, 15, 420, 730 },
  { "ABS",   70, 40, 10, 480, 365 },
  { "Nylon", 80, 45, 10, 720, 180 },
  { "TPU",   50, 25, 20, 240, 365 }
};
const int NUM_PERFIS = 5;

// ─────────────────────────────────────────────────────────────
// Histórico para gráfico web
// ─────────────────────────────────────────────────────────────
#define MAX_HIST 60
float histTemp[MAX_HIST] = {};
float histUmid[MAX_HIST] = {};
int   histIdx = 0;

// ─────────────────────────────────────────────────────────────
// Objetos
// ─────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
sdmmc_card_t*     sdCard = nullptr;
bool              sdCardAvailable = false;
DHT               dht(DHT_PIN, DHT_TYPE);
HX711             balanca;
WebServer         server(80);
Preferences       prefs;

double pidInput, pidOutput, pidSetpoint;
double Kp = 2.0, Ki = 5.0, Kd = 1.0;
PID myPID(&pidInput, &pidOutput, &pidSetpoint, Kp, Ki, Kd, DIRECT);

// ─────────────────────────────────────────────────────────────
// Estado do sistema
// ─────────────────────────────────────────────────────────────
int   materialIdx    = 0;
bool  modoSecagem    = false;
bool  sistemaAtivo   = false;
bool  sensoresOK     = false;
int   diasRestantes  = 730;

unsigned long tsSecagem  = 0;
unsigned long tsValidade = 0;

float temperatura = 0, umidade = 0;
float pesoAtual   = 0;
float pesoMaximo  = 1000;
float tara        = 0;
float hxScale     = 420.0f;

unsigned long tUltSensor      = 0;
unsigned long tUltBalanca     = 0;
unsigned long tUltLCD         = 0;
unsigned long tUltTrocaPagina = 0;

// WiFi — controle de reconexão
bool          wifiConectando  = false;
unsigned long tUltWifiCheck   = 0;
unsigned long tUltWifiRetry   = 0;
#define WIFI_CHECK_INTERVAL  5000   // verifica status a cada 5s
#define WIFI_RETRY_INTERVAL  30000  // tenta reconectar a cada 30s

int  lcdPagina = 0;

// ─────────────────────────────────────────────────────────────
// Encoder — máquina de estados
// ─────────────────────────────────────────────────────────────
volatile int  encDelta     = 0;
int           encUltimoClk = HIGH;

// Estados do encoder
enum EncEstado {
  ENC_NORMAL,   // girar = muda material, press = abre menu
  ENC_MENU      // girar = navega menu, press = executa/cancela
};
EncEstado encEstado = ENC_NORMAL;

// Itens do menu de ações
const char* MENU_ITENS[] = {
  "Ligar / Desligar",
  "Secar",
  "Armazenar",
  "Zerar Tara",
  "Cancelar"
};
const int MENU_TOTAL = 5;
int  menuIdx = 0;

// Estado do botão
bool          btnPressionado = false;
unsigned long tBtnPress      = 0;

// ─────────────────────────────────────────────────────────────
// Caracteres customizados LCD
// ─────────────────────────────────────────────────────────────
byte icoTemp[8]     = {0b00100,0b01010,0b01010,0b01110,0b01110,0b11111,0b11111,0b01110};
byte icoUmi[8]      = {0b00100,0b00100,0b01110,0b01110,0b11111,0b11111,0b11111,0b01110};
byte icoCarretel[8] = {0b01110,0b10001,0b10101,0b10001,0b10101,0b10001,0b01110,0b00000};
byte icoBarra[8]    = {0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111};
byte icoChama[8]    = {0b00010,0b00110,0b01101,0b01111,0b11111,0b11111,0b01110,0b00000};
byte icoVento[8]    = {0b00100,0b10110,0b11100,0b00100,0b00111,0b01101,0b00100,0b00000};
byte icoWifi[8]     = {0b00000,0b01110,0b10001,0b00100,0b01010,0b00000,0b00100,0b00000};

#define ICO_TEMP     0
#define ICO_UMI      1
#define ICO_CARRETEL 2
#define ICO_BARRA    3
#define ICO_CHAMA    4
#define ICO_VENTO    5
#define ICO_WIFI     6

// ═════════════════════════════════════════════════════════════
// HELPERS GERAIS
// ═════════════════════════════════════════════════════════════

void releSet(int pino, bool inv, bool ligar) {
  digitalWrite(pino, ligar ? (inv ? LOW : HIGH) : (inv ? HIGH : LOW));
}

void desligarTudo() {
  releSet(RELE_RESIST, RELE1_INV, false);
  releSet(RELE_VENT,   RELE2_INV, false);
  digitalWrite(LED_RESIST, LOW);
  digitalWrite(LED_VENT,   LOW);
}

void atualizarLEDs() {
  digitalWrite(LED_RESIST,
    digitalRead(RELE_RESIST) == (RELE1_INV ? LOW : HIGH) ? HIGH : LOW);
  digitalWrite(LED_VENT,
    digitalRead(RELE_VENT) == (RELE2_INV ? LOW : HIGH) ? HIGH : LOW);
}

void cors() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

// ═════════════════════════════════════════════════════════════
// SD (SPI)
// ═════════════════════════════════════════════════════════════

String mimeType(const String& p) {
  if (p.endsWith(".html")) return "text/html; charset=utf-8";
  if (p.endsWith(".css"))  return "text/css; charset=utf-8";
  if (p.endsWith(".js"))   return "application/javascript; charset=utf-8";
  if (p.endsWith(".json")) return "application/json; charset=utf-8";
  if (p.endsWith(".ico"))  return "image/x-icon";
  if (p.endsWith(".png"))  return "image/png";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".svg"))  return "image/svg+xml";
  if (p.endsWith(".webp")) return "image/webp";
  if (p.endsWith(".gif"))  return "image/gif";
  if (p.endsWith(".woff2")) return "font/woff2";
  if (p.endsWith(".woff")) return "font/woff";
  if (p.endsWith(".txt"))  return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

void serveSD(const String& uri) {
  if (!sdCardAvailable) { server.send(503, "text/plain", "SD indisponivel"); return; }
  String path = "/sdcard/web";
  path += (uri == "/" || uri.isEmpty()) ? "/index.html" : uri;

  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { server.send(404, "text/plain", "Nao encontrado: " + path); return; }

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);

  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: " + mimeType(path));
  client.print("Content-Length: "); client.println(sz);
  client.println("Connection: close");
  client.println();

  uint8_t buf[1024]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (!client.connected()) break;
    client.write(buf, n);
  }
  fclose(f);
}

bool initSDCard() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num     = SD_MOSI_PIN;
  bus.miso_io_num     = SD_MISO_PIN;
  bus.sclk_io_num     = SD_SCK_PIN;
  bus.quadwp_io_num   = -1;
  bus.quadhd_io_num   = -1;
  bus.max_transfer_sz = 4096;
  esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("[SD-SPI] Falha no barramento: %s\n", esp_err_to_name(err));
    sdCardAvailable = false; return false;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_SPI_HOST;
  host.max_freq_khz = 20000;   // reduza para 10000 ou 4000 se houver instabilidade

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = SD_CS_PIN;
  slot.host_id = SD_SPI_HOST;

  esp_vfs_fat_sdmmc_mount_config_t mnt = {};
  mnt.format_if_mount_failed = false; mnt.max_files = 8; mnt.allocation_unit_size = 16*1024;

  err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mnt, &sdCard);
  if (err != ESP_OK) {
    Serial.printf("[SD-SPI] Falha: %s\n", esp_err_to_name(err));
    spi_bus_free(SD_SPI_HOST);
    sdCardAvailable = false; return false;
  }
  sdCardAvailable = true;
  mkdir("/sdcard/web", 0777); mkdir("/sdcard/logs", 0777);
  uint64_t sz = (uint64_t)sdCard->csd.capacity * sdCard->csd.sector_size;
  Serial.printf("[SD-SPI] OK — %.2f GB\n", (double)sz / 1073741824.0);
  return true;
}

// ═════════════════════════════════════════════════════════════
// LCD — auxiliares
// ═════════════════════════════════════════════════════════════

void lcdPrint(int col, int row, const String& txt, int larg = 0) {
  lcd.setCursor(col, row);
  lcd.print(txt);
  for (int i = 0; i < larg - (int)txt.length(); i++) lcd.print(' ');
}

void lcdBarra(int col, int row, int larg, float pct) {
  int cheias = constrain((int)(pct / 100.0f * larg), 0, larg);
  lcd.setCursor(col, row);
  for (int i = 0; i < larg; i++)
    i < cheias ? lcd.write(ICO_BARRA) : lcd.print('-');
}

String formataTempo(long min) {
  if (min <= 0) return "0m     ";
  int h = min / 60, m = min % 60;
  return h > 0 ? String(h) + "h " + (m < 10 ? "0" : "") + String(m) + "m"
               : String(m) + "min  ";
}

void lcdFeedback(const String& l1, const String& l2) {
  lcd.clear();
  lcdPrint(0, 1, "  " + l1, LCD_COLS);
  lcdPrint(0, 2, "  " + l2, LCD_COLS);
  delay(1200);
  lcd.clear();
}

// ═════════════════════════════════════════════════════════════
// LCD — páginas normais
// ═════════════════════════════════════════════════════════════

void lcdPaginaPrincipal() {
  float pct = pesoMaximo > 0 ? min(100.0f, pesoAtual / pesoMaximo * 100.0f) : 0;
  bool  res = digitalRead(RELE_RESIST) == (RELE1_INV ? LOW : HIGH);
  bool  ven = digitalRead(RELE_VENT)   == (RELE2_INV ? LOW : HIGH);

  String l0; l0.reserve(22);
  l0 += (char)ICO_TEMP; l0 += " "; l0 += String(temperatura, 1); l0 += "\xDF""C";
  while ((int)l0.length() < 10) l0 += ' ';
  l0 += (char)ICO_UMI;  l0 += " "; l0 += String(umidade, 1);    l0 += "%";
  lcdPrint(0, 0, l0, LCD_COLS);

  String l1 = "SP:"; l1 += String(pidSetpoint, 1); l1 += "\xDF""C";
  while ((int)l1.length() < 12) l1 += ' ';
  l1 += PERFIS[materialIdx].nome;
  lcdPrint(0, 1, l1, LCD_COLS);

  lcd.setCursor(0, 2); lcd.write(ICO_CARRETEL); lcd.print(' ');
  lcdBarra(2, 2, LCD_COLS - 10, pct);
  String ps = " " + String((int)pct) + "%  ";
  lcd.setCursor(LCD_COLS - 6, 2); lcd.print(ps.substring(0, 6));

  String l3 = modoSecagem ? "SECAG " : "ARMAZ ";
  if (modoSecagem && tsSecagem > 0) {
    long rest = (long)PERFIS[materialIdx].tempoSecagem
              - (long)((millis() - tsSecagem) / 60000UL);
    l3 += formataTempo(max(0L, rest));
  } else {
    l3 += sistemaAtivo ? "ATIVO " : "PARADO";
  }
  while ((int)l3.length() < LCD_COLS - 2) l3 += ' ';
  lcdPrint(0, 3, l3, LCD_COLS - 2);
  lcd.setCursor(LCD_COLS - 2, 3);
  lcd.write(res ? ICO_CHAMA : ' ');
  lcd.write(ven ? ICO_VENTO : ' ');
}

void lcdPaginaFilamento() {
  float pct = pesoMaximo > 0 ? min(100.0f, pesoAtual / pesoMaximo * 100.0f) : 0;
  lcdPrint(0, 0, String("FILAMENTO  ") + PERFIS[materialIdx].nome, LCD_COLS);
  lcd.setCursor(0, 1); lcd.write(ICO_CARRETEL); lcd.print("  ");
  lcdPrint(3, 1, String((int)pesoAtual) + "g / " + String((int)pesoMaximo) + "g", LCD_COLS - 3);
  int bw = LCD_COLS - 6;
  lcdBarra(0, 2, bw, pct);
  lcdPrint(bw, 2, " " + String((int)pct) + "%", 6);
  lcdPrint(0, 3, "Validade: " + String(diasRestantes) + " dias", LCD_COLS);
}

void lcdPaginaStatus() {
  bool res = digitalRead(RELE_RESIST) == (RELE1_INV ? LOW : HIGH);
  bool ven = digitalRead(RELE_VENT)   == (RELE2_INV ? LOW : HIGH);
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "Sem WiFi   ";
  lcdPrint(0, 0, "PrintSafe v1.0", LCD_COLS);
  lcd.setCursor(0, 1); lcd.write(ICO_WIFI); lcd.print(' ');
  lcdPrint(2, 1, ip, LCD_COLS - 2);
  String l2 = "Resist: "; l2 += res ? "ON " : "OFF"; l2 += "  Vent: "; l2 += ven ? "ON " : "OFF";
  lcdPrint(0, 2, l2, LCD_COLS);
  String l3 = "SP:"; l3 += String(pidSetpoint, 1); l3 += "\xDF""C  "; l3 += String(umidade, 1); l3 += "% umi";
  lcdPrint(0, 3, l3, LCD_COLS);
}

void atualizarLCD() {
  unsigned long agora = millis();
  if (agora - tUltTrocaPagina >= LCD_TEMPO_POR_PAGINA) {
    tUltTrocaPagina = agora;
    lcdPagina = (lcdPagina + 1) % LCD_TOTAL_PAGINAS;
    lcd.clear();
  }
  if (agora - tUltLCD < LCD_INTERVALO) return;
  tUltLCD = agora;
  switch (lcdPagina) {
    case 0: lcdPaginaPrincipal(); break;
    case 1: lcdPaginaFilamento(); break;
    case 2: lcdPaginaStatus();    break;
  }
}

// ═════════════════════════════════════════════════════════════
// LCD — menu de ações (chamado pelo encoder)
// ═════════════════════════════════════════════════════════════

/*  Layout 20x4:
 *  ┌────────────────────┐
 *  │ *** MENU ***       │
 *  │  Ligar / Desligar  │  ← item anterior
 *  │► Secar            │  ← selecionado (com ►)
 *  │  Armazenar         │  ← próximo
 *  └────────────────────┘
 *  (scroll automático — sempre mostra 3 itens centrados no selecionado)
 */
void lcdDesenharMenu() {
  lcd.clear();
  lcdPrint(0, 0, "    *** MENU ***", LCD_COLS);

  // Mostra item anterior, atual e próximo
  int ant = (menuIdx - 1 + MENU_TOTAL) % MENU_TOTAL;
  int prx = (menuIdx + 1) % MENU_TOTAL;

  lcdPrint(2, 1, String(MENU_ITENS[ant]), LCD_COLS - 2);

  lcd.setCursor(0, 2); lcd.print('\x7e');  // ► (char 0x7E do LCD = →)
  lcdPrint(1, 2, String(MENU_ITENS[menuIdx]), LCD_COLS - 1);

  lcdPrint(2, 3, String(MENU_ITENS[prx]), LCD_COLS - 2);
}

// ═════════════════════════════════════════════════════════════
// ENCODER — ISR + máquina de estados
// ═════════════════════════════════════════════════════════════

void IRAM_ATTR isrEncoder() {
  int clk = digitalRead(ENC_CLK);
  int dt  = digitalRead(ENC_DT);
  if (clk != encUltimoClk) {
    encDelta += (dt != clk) ? +1 : -1;
    encUltimoClk = clk;
  }
}

// Salva material selecionado em Preferences
void salvarMaterial() {
  prefs.begin("printsafe", false);
  prefs.putInt("material",   materialIdx);
  prefs.putInt("dias",       diasRestantes);
  prefs.end();
}

// Executa a opção escolhida no menu
void executarMenu() {
  switch (menuIdx) {
    case 0:  // Ligar / Desligar
      sistemaAtivo = !sistemaAtivo;
      if (!sistemaAtivo) desligarTudo();
      Serial.printf("[MENU] Sistema → %s\n", sistemaAtivo ? "ON" : "OFF");
      lcdFeedback("Sistema:", sistemaAtivo ? "LIGADO" : "DESLIGADO");
      break;

    case 1:  // Secar
      modoSecagem = true;
      tsSecagem   = millis();
      Serial.println("[MENU] Modo → Secagem");
      lcdFeedback("Modo:", "SECAGEM");
      break;

    case 2:  // Armazenar
      modoSecagem = false;
      tsSecagem   = 0;
      Serial.println("[MENU] Modo → Armazenamento");
      lcdFeedback("Modo:", "ARMAZENAMENTO");
      break;

    case 3:  // Zerar Tara
      if (balanca.is_ready()) {
        balanca.tare(10);
        long offset = balanca.get_offset();
        tara = 0; pesoAtual = 0;
        prefs.begin("printsafe", false);
        prefs.putLong("hxOffset", offset);
        prefs.putFloat("tara", 0.0f);
        prefs.end();
        Serial.printf("[MENU] Tara zerada — offset=%ld\n", offset);
        lcdFeedback("Tara:", "Zerada!");
      } else {
        lcdFeedback("Balanca:", "Indisponivel");
      }
      break;

    case 4:  // Cancelar
      Serial.println("[MENU] Cancelado");
      lcd.clear();
      break;
  }
}

void processarEncoder() {
  // ── Leitura do delta (ISR) ───────────────────────────────
  int delta = 0;
  noInterrupts(); delta = encDelta; encDelta = 0; interrupts();

  if (delta != 0) {
    int dir = delta > 0 ? 1 : -1;

    if (encEstado == ENC_NORMAL) {
      // GIRAR no modo normal → muda material diretamente
      materialIdx = (materialIdx + dir + NUM_PERFIS) % NUM_PERFIS;
      diasRestantes = PERFIS[materialIdx].validadeInicial;
      salvarMaterial();
      Serial.printf("[ENC] Material → %s\n", PERFIS[materialIdx].nome);
      // Feedback rápido no LCD sem apagar a tela
      lcd.setCursor(0, 1);
      lcd.print("Mat: ");
      lcd.print(PERFIS[materialIdx].nome);
      lcd.print("          ");

    } else {
      // GIRAR no menu → navega itens
      menuIdx = (menuIdx + dir + MENU_TOTAL) % MENU_TOTAL;
      lcdDesenharMenu();
    }
  }

  // ── Botão ────────────────────────────────────────────────
  bool btnAtual = (digitalRead(ENC_SW) == LOW);

  if (btnAtual && !btnPressionado) {
    btnPressionado = true;
    tBtnPress = millis();
  }

  if (!btnAtual && btnPressionado) {
    btnPressionado = false;
    unsigned long dur = millis() - tBtnPress;
    if (dur < BTN_DEBOUNCE) return;  // ignora ruído

    if (encEstado == ENC_NORMAL) {
      // PRESSIONAR no modo normal → abre menu
      encEstado = ENC_MENU;
      menuIdx   = 0;
      lcdDesenharMenu();
      Serial.println("[ENC] Menu aberto");

    } else {
      // PRESSIONAR no menu → executa item e fecha menu
      encEstado = ENC_NORMAL;
      executarMenu();
      // Se feedback usou delay, força atualização do LCD
      tUltLCD = 0;
      tUltTrocaPagina = millis();
      lcd.clear();
    }
  }
}

// ═════════════════════════════════════════════════════════════
// SPLASH e WiFi LCD
// ═════════════════════════════════════════════════════════════

void lcdSplash() {
  lcd.clear();
  lcdPrint(0, 0, "   PrintSafe v1.0 ", LCD_COLS);
  lcdPrint(0, 1, "  Estufa Intelig. ", LCD_COLS);
  lcdPrint(0, 2, "  Iniciando...    ", LCD_COLS);
  delay(2000);
}

void lcdWifiConectando(int t) {
  String dots; for (int i = 0; i < t % 4; i++) dots += '.';
  lcdPrint(0, 2, "WiFi: conectando  ", LCD_COLS);
  lcdPrint(0, 3, dots, LCD_COLS);
}

void lcdWifiOK() {
  lcd.clear();
  lcdPrint(0, 0, "  PrintSafe v1.0", LCD_COLS);
  lcdPrint(0, 1, "  WiFi OK!", LCD_COLS);
  lcdPrint(0, 2, "  " + WiFi.localIP().toString(), LCD_COLS);
  lcdPrint(0, 3, "  Pronto!", LCD_COLS);
  delay(2000); lcd.clear();
}

// ═════════════════════════════════════════════════════════════
// CONTROLE
// ═════════════════════════════════════════════════════════════

void finalizarSecagem() {
  modoSecagem = false; tsSecagem = 0;
  diasRestantes = PERFIS[materialIdx].validadeInicial;
  tsValidade = millis();
  prefs.begin("printsafe", false);
  prefs.putInt("dias", diasRestantes);
  prefs.end();
  Serial.println("[PrintSafe] Secagem finalizada.");
  lcdFeedback("Secagem", "Finalizada!");
}

void controlarSistema() {
  if (!sistemaAtivo || !sensoresOK) { desligarTudo(); return; }
  const Perfil& p = PERFIS[materialIdx];
  pidSetpoint = modoSecagem ? p.tempSecagem : p.tempArmazenamento;
  pidInput    = temperatura;
  myPID.Compute();
  releSet(RELE_RESIST, RELE1_INV, pidOutput > 0.5);
  bool vent = umidade > p.umidadeAlvo + 5 || temperatura > pidSetpoint + 3 || modoSecagem;
  releSet(RELE_VENT, RELE2_INV, vent);
  atualizarLEDs();
  if (modoSecagem && tsSecagem > 0) {
    unsigned long dec = (millis() - tsSecagem) / 60000UL;
    if (dec >= (unsigned long)p.tempoSecagem) finalizarSecagem();
  }
}

void verificarValidade() {
  if (!sistemaAtivo || modoSecagem) return;
  if (tsValidade == 0) { tsValidade = millis(); return; }
  if (millis() - tsValidade >= 86400000UL) {
    tsValidade = millis();
    if (diasRestantes > 0) {
      diasRestantes--;
      prefs.begin("printsafe", false);
      prefs.putInt("dias", diasRestantes);
      prefs.end();
    }
  }
}

// ═════════════════════════════════════════════════════════════
// WiFi — reconexão segura (sem chamar reconnect() enquanto conectando)
// ═════════════════════════════════════════════════════════════

void gerenciarWifi() {
  unsigned long agora = millis();
  if (agora - tUltWifiCheck < WIFI_CHECK_INTERVAL) return;
  tUltWifiCheck = agora;

  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    wifiConectando = false;
    digitalWrite(LED_WIFI, HIGH);
    return;
  }

  digitalWrite(LED_WIFI, LOW);

  // Só tenta reconectar se NÃO está no meio de uma tentativa
  // e se já passou o intervalo de retry
  if (!wifiConectando && (agora - tUltWifiRetry >= WIFI_RETRY_INTERVAL)) {
    tUltWifiRetry  = agora;
    wifiConectando = true;
    Serial.println("[WiFi] Reconectando...");
    WiFi.disconnect(false);   // não apaga credenciais
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // Detecta quando a tentativa terminou (com ou sem sucesso)
  if (wifiConectando && status != WL_DISCONNECTED && status != WL_IDLE_STATUS) {
    wifiConectando = false;  // conectou ou falhou — libera para nova tentativa
  }
}

// ═════════════════════════════════════════════════════════════
// JSON
// ═════════════════════════════════════════════════════════════

String buildJSON() {
  StaticJsonDocument<768> doc;
  doc["sensoresOK"]  = sensoresOK;
  doc["temp"]        = round(temperatura * 10) / 10.0;
  doc["umid"]        = round(umidade * 10) / 10.0;
  doc["setpoint"]    = pidSetpoint;
  doc["material"]    = PERFIS[materialIdx].nome;
  doc["materialIdx"] = materialIdx;
  doc["modo"]        = modoSecagem ? "Secagem" : "Armazenamento";
  doc["ativo"]       = sistemaAtivo;
  doc["validade"]    = diasRestantes;
  doc["resistencia"] = (digitalRead(RELE_RESIST) == (RELE1_INV ? LOW : HIGH));
  doc["ventoinha"]   = (digitalRead(RELE_VENT)   == (RELE2_INV ? LOW : HIGH));
  if (modoSecagem && tsSecagem > 0) {
    long rest = (long)PERFIS[materialIdx].tempoSecagem
              - (long)((millis() - tsSecagem) / 60000L);
    doc["tempoRestante"] = max(0L, rest);
  }
  JsonObject hist  = doc.createNestedObject("historico");
  JsonArray  aTemp = hist.createNestedArray("temp");
  JsonArray  aUmid = hist.createNestedArray("umid");
  for (int i = 0; i < MAX_HIST; i++) {
    int idx = (histIdx + i) % MAX_HIST;
    aTemp.add(histTemp[idx]);
    aUmid.add(histUmid[idx]);
  }
  float pct = pesoMaximo > 0 ? min(100.0f, pesoAtual / pesoMaximo * 100.0f) : 0;
  doc["pesoAtual"]      = round(pesoAtual * 10) / 10.0;
  doc["pesoMaximo"]     = pesoMaximo;
  doc["pesoPercentual"] = round(pct * 10) / 10.0;
  doc["tara"]           = tara;
  doc["hxScale"]        = hxScale;
  String out; serializeJson(doc, out); return out;
}

// ═════════════════════════════════════════════════════════════
// ROTAS HTTP
// ═════════════════════════════════════════════════════════════

void rotaOptions() { cors(); server.send(200, "text/plain", ""); }
void rotaData()    { cors(); server.send(200, "application/json; charset=utf-8", buildJSON()); }

void rotaComando() {
  cors();
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"success\":false}"); return; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400,"application/json","{\"success\":false,\"error\":\"JSON invalido\"}"); return;
  }
  String cmd = doc["cmd"].as<String>();
  if (cmd == "TOGGLE") {
    sistemaAtivo = !sistemaAtivo;
    if (!sistemaAtivo) desligarTudo(); else atualizarLEDs();
    lcd.clear();
  } else if (cmd == "MATERIAL") {
    int idx = doc["valor"] | 0;
    if (idx >= 0 && idx < NUM_PERFIS) {
      materialIdx = idx; diasRestantes = PERFIS[idx].validadeInicial;
      salvarMaterial(); lcd.clear();
    }
  } else if (cmd == "SECAGEM") {
    modoSecagem = true; tsSecagem = millis(); lcd.clear();
  } else if (cmd == "ARMAZENAMENTO") {
    modoSecagem = false; tsSecagem = 0; lcd.clear();
  } else if (cmd == "RESET") {
    diasRestantes = PERFIS[materialIdx].validadeInicial; tsValidade = millis();
    prefs.begin("printsafe",false); prefs.putInt("dias",diasRestantes); prefs.end();
  }
  server.send(200,"application/json","{\"success\":true}");
}

void rotaPesoMax() {
  cors();
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"success\":false}"); return; }
  StaticJsonDocument<64> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400,"application/json","{\"success\":false}"); return;
  }
  float v = doc["pesoMaximo"] | 0.0f;
  if (v >= 1 && v <= 5000) {
    pesoMaximo = v;
    prefs.begin("printsafe",false); prefs.putFloat("pesoMax",pesoMaximo); prefs.end();
    server.send(200,"application/json","{\"success\":true,\"pesoMaximo\":"+String(pesoMaximo,1)+"}");
  } else {
    server.send(400,"application/json","{\"success\":false,\"error\":\"Fora do range 1-5000\"}");
  }
}

void rotaTara() {
  cors();
  if (!balanca.is_ready()) {
    server.send(503,"application/json","{\"success\":false,\"error\":\"Balanca indisponivel\"}"); return;
  }
  balanca.tare(10);
  long offset = balanca.get_offset();
  tara = 0; pesoAtual = 0;
  prefs.begin("printsafe",false);
  prefs.putLong("hxOffset", offset);
  prefs.putFloat("tara", 0.0f);
  prefs.end();
  Serial.printf("[HX711] Tara via web — offset=%ld\n", offset);
  server.send(200,"application/json","{\"success\":true,\"tara\":0}");
}

void rotaCalibracao() {
  cors();
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"success\":false}"); return; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400,"application/json","{\"success\":false}"); return;
  }
  if (doc.containsKey("hxScale")) {
    float v = doc["hxScale"] | 0.0f;
    if (v != 0) {
      hxScale = v; balanca.set_scale(hxScale);
      prefs.begin("printsafe",false); prefs.putFloat("hxScale",hxScale); prefs.end();
    }
  }
  if (doc.containsKey("pesoReferencia") && balanca.is_ready()) {
    float ref = doc["pesoReferencia"];
    if (ref > 0) {
      long raw = balanca.get_value(10);
      if (raw != 0) {
        hxScale = (float)raw / ref; balanca.set_scale(hxScale);
        prefs.begin("printsafe",false); prefs.putFloat("hxScale",hxScale); prefs.end();
        Serial.printf("[HX711] Calib: raw=%ld ref=%.1f scale=%.3f\n", raw, ref, hxScale);
      }
    }
  }
  server.send(200,"application/json","{\"success\":true,\"hxScale\":"+String(hxScale,3)+"}");
}

// ═════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println(  "║     PrintSafe v1.0 — Estufa          ║");
  Serial.println(  "╚══════════════════════════════════════╝");

  // Pinos de saída
  pinMode(RELE_RESIST, OUTPUT); pinMode(RELE_VENT, OUTPUT);
  pinMode(LED_WIFI, OUTPUT); pinMode(LED_RESIST, OUTPUT); pinMode(LED_VENT, OUTPUT);
  desligarTudo();

  // Teste visual dos LEDs
  for (int p : {LED_WIFI, LED_RESIST, LED_VENT}) { digitalWrite(p, HIGH); delay(150); }
  for (int p : {LED_WIFI, LED_RESIST, LED_VENT}) digitalWrite(p, LOW);

  // LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init(); lcd.backlight();
  lcd.createChar(ICO_TEMP,     icoTemp);
  lcd.createChar(ICO_UMI,      icoUmi);
  lcd.createChar(ICO_CARRETEL, icoCarretel);
  lcd.createChar(ICO_BARRA,    icoBarra);
  lcd.createChar(ICO_CHAMA,    icoChama);
  lcd.createChar(ICO_VENTO,    icoVento);
  lcd.createChar(ICO_WIFI,     icoWifi);
  Serial.println("[LCD] OK");

  // Encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  encUltimoClk = digitalRead(ENC_CLK);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), isrEncoder, CHANGE);
  Serial.println("[ENC] OK");

  lcdSplash();

  // Preferences
  prefs.begin("printsafe", true);
  materialIdx   = constrain(prefs.getInt("material", 0), 0, NUM_PERFIS - 1);
  diasRestantes = prefs.getInt("dias",      PERFIS[materialIdx].validadeInicial);
  pesoMaximo    = prefs.getFloat("pesoMax", 1000.0f);
  tara          = prefs.getFloat("tara",    0.0f);
  hxScale       = prefs.getFloat("hxScale", 420.0f);
  long hxOffset = prefs.getLong("hxOffset", 0);
  prefs.end();
  Serial.printf("[PREFS] mat=%s dias=%d pesoMax=%.0f scale=%.3f\n",
                PERFIS[materialIdx].nome, diasRestantes, pesoMaximo, hxScale);

  // DHT22
  dht.begin();
  Serial.println("[DHT22] OK — GPIO 17");

  // HX711
  balanca.begin(HX711_DOUT, HX711_SCK);
  balanca.set_scale(hxScale);
  balanca.set_offset(hxOffset);
  Serial.printf("[HX711] %s — scale=%.3f offset=%ld\n",
                balanca.is_ready() ? "OK" : "Aguardando", hxScale, hxOffset);

  // PID
  myPID.SetMode(AUTOMATIC);
  myPID.SetOutputLimits(0, 1);

  // SD (SPI)
  lcdPrint(0, 2, "Montando SD...    ", LCD_COLS);
  initSDCard();
  lcdPrint(0, 3, sdCardAvailable ? "SD: OK            " : "SD: Falha (ok)    ", LCD_COLS);
  delay(800);

  // WiFi — conexão inicial (bloqueante só no boot)
  lcd.clear();
  lcdPrint(0, 0, "  PrintSafe v1.0", LCD_COLS);
  lcdPrint(0, 1, "  Conectando WiFi", LCD_COLS);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 20) {
    delay(500); Serial.print('.'); lcdWifiConectando(++t);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] http://%s\n", WiFi.localIP().toString().c_str());
    digitalWrite(LED_WIFI, HIGH);
    lcdWifiOK();
  } else {
    Serial.println("\n[WiFi] Sem rede — operando local");
    lcdFeedback("Sem WiFi", "Modo local");
  }
  // Inicializa controle de reconexão
  tUltWifiCheck = millis();
  tUltWifiRetry = millis();

  // Rotas HTTP
  server.on("/",             HTTP_GET,     []{ serveSD("/index.html"); });
  server.on("/index.html",   HTTP_GET,     []{ serveSD("/index.html"); });
  server.on("/style.css",    HTTP_GET,     []{ serveSD("/style.css");  });
  server.on("/script.js",    HTTP_GET,     []{ serveSD("/script.js");  });
  server.on("/api/data",     HTTP_GET,     rotaData);
  server.on("/api/comando",  HTTP_POST,    rotaComando);
  server.on("/api/pesomax",  HTTP_POST,    rotaPesoMax);
  server.on("/api/tara",     HTTP_POST,    rotaTara);
  server.on("/api/calibrar", HTTP_POST,    rotaCalibracao);
  server.on("/api/comando",  HTTP_OPTIONS, rotaOptions);
  server.on("/api/pesomax",  HTTP_OPTIONS, rotaOptions);
  server.on("/api/tara",     HTTP_OPTIONS, rotaOptions);
  server.on("/api/calibrar", HTTP_OPTIONS, rotaOptions);
  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/api/")) {
      cors(); server.send(404,"application/json","{\"error\":\"Rota nao encontrada\"}"); return;
    }
    serveSD(uri);
  });
  server.begin();
  Serial.println("[HTTP] Pronto.");
}

// ═════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();

  // WiFi — reconexão segura (não chama reconnect() enquanto conectando)
  gerenciarWifi();

  unsigned long agora = millis();

  // Sensores + controle (2s)
  if (agora - tUltSensor >= 2000) {
    tUltSensor = agora;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > -40.0f && t < 125.0f && h >= 0.0f && h <= 100.0f) {
      sensoresOK = true;
      temperatura = t; umidade = h;
      histTemp[histIdx] = t;
      histUmid[histIdx] = h;
      histIdx = (histIdx + 1) % MAX_HIST;
    } else {
      sensoresOK = false;
      desligarTudo();
      Serial.println("[DHT22] Falha — estado seguro");
    }
    controlarSistema();
    verificarValidade();
  }

  // Balança (500ms)
  if (agora - tUltBalanca >= 500) {
    tUltBalanca = agora;
    if (balanca.is_ready()) {
      float bruto = balanca.get_units(3);
      pesoAtual = bruto < 0.0f ? 0.0f : bruto;
    }
  }

  // Encoder e LCD
  processarEncoder();
  if (encEstado == ENC_NORMAL) atualizarLCD();
  // No modo menu o LCD é gerenciado pelo próprio encoder
}
