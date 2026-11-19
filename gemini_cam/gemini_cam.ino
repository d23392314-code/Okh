/*
 * ESP32-S3 CAM  +  Gemini AI  +  TFT ST7789 240x240  +  Keyboard USB-OTG
 * ----------------------------------------------------------------------
 *  - Setting WiFi / API key / model lewat halaman web (port 80)
 *      * AP selalu aktif : SSID "Gemini-ESP32"  pass "12345678"  -> http://192.168.4.1
 *      * Setelah konek WiFi rumah : buka IP yang tampil di layar (atau http://gemini.local)
 *  - Keyboard USB lewat port USB-OTG (native USB, GPIO19/20)
 *  - Kode di-upload lewat port USB-UART seperti biasa
 *  - Tombol keyboard:
 *      ketik + Enter  : kirim ke Gemini
 *      ~ (atau `)     : ambil foto, tampil 6 detik, ikut terkirim di pertanyaan berikutnya
 *      Up/Down        : scroll 1 baris        PgUp/PgDn : scroll 1 halaman
 *      Home/End       : ke paling atas/bawah  Esc       : hapus ketikan / batalkan foto
 *      Ctrl+L         : hapus semua chat
 *      Ctrl+huruf     : simbol -> P=π R=√ S=∑ I=∫ D=Δ T=θ A=α B=β M=μ W=ω O=° E=∞
 *                                 X=× N=≠ U=≤ Y=≥ Q=≈ F=∂ G=→
 *
 *  Library: Adafruit GFX, Adafruit ST7735 and ST7789, U8g2_for_Adafruit_GFX,
 *           JPEGDEC, ArduinoJson (v7), EspUsbHost (tanakamasayuki)
 *  Board  : ESP32S3 Dev Module, PSRAM OPI, USB Mode = USB-OTG (TinyUSB),
 *           USB CDC On Boot = Disabled
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <JPEGDEC.h>
#include <ArduinoJson.h>
#include <vector>
#include "esp_camera.h"
#include "mbedtls/base64.h"
#include "EspUsbHost.h"

// ============================ PIN TFT ============================
// Modul kamu: GND VCC SCK SDA RES DC BLK (tanpa CS)
#define TFT_SCK   39
#define TFT_MOSI  40   // pin "SDA" di modul
#define TFT_DC    41
#define TFT_RST   42
#define TFT_BL    1    // isi -1 kalau BLK disambung langsung ke 3V3

// ======================= PIN KAMERA (OV2640) =====================
// Standar board "ESP32-S3-WROOM CAM" (Freenove & klonnya).
// Kalau board kamu beda, ubah di sini.
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_Y2    11
#define CAM_Y3    9
#define CAM_Y4    8
#define CAM_Y5    10
#define CAM_Y6    12
#define CAM_Y7    18
#define CAM_Y8    17
#define CAM_Y9    16
#define CAM_VSYNC 6
#define CAM_HREF  7
#define CAM_PCLK  13
#define CAM_VFLIP   0   // ubah ke 1 kalau gambar terbalik
#define CAM_HMIRROR 0   // ubah ke 1 kalau gambar mirror

// ============================ LAIN-LAIN ==========================
#define AP_SSID        "Gemini-ESP32"
#define AP_PASS        "12345678"
#define PREVIEW_MS     6000
#define MAX_HISTORY    5          // jumlah tanya-jawab yang diingat
#define SCR_W          240
#define CHAT_ROWS      12
#define LINE_H         16

static const char* MODEL_PRESETS[] = {
  "gemini-2.5-flash", "gemini-2.5-pro", "gemini-2.5-flash-lite",
  "gemini-2.0-flash", "gemini-3-pro-preview"
};
static const int N_PRESETS = sizeof(MODEL_PRESETS) / sizeof(MODEL_PRESETS[0]);

static const char SYS_PROMPT[] =
  "Kamu asisten AI pada perangkat kecil dengan layar teks sempit. "
  "Jawab dengan bahasa yang dipakai pengguna. Untuk soal matematika/sains: "
  "tunjukkan langkah yang ringkas tapi lengkap, lalu beri jawaban akhir yang jelas. "
  "JANGAN pakai LaTeX, markdown, tabel, atau tanda $. "
  "Tulis simbol dengan Unicode: √ ² ³ ⁿ π ∫ ∑ ∞ ≤ ≥ ≠ ≈ ± × ÷ → ⇒ α β θ Δ ∂ °. "
  "Pecahan tulis a/b dengan tanda kurung bila perlu. "
  "Jika ada foto, analisis isinya; jika berisi soal, selesaikan soalnya.";

// ============================ GLOBAL =============================
Preferences prefs;
WebServer server(80);
SPIClass tftSPI(HSPI);
Adafruit_ST7789 tft(&tftSPI, -1, TFT_DC, TFT_RST);
GFXcanvas16 *canvas = nullptr;
U8G2_FOR_ADAFRUIT_GFX u8g2;
JPEGDEC jpeg;

struct Line { uint8_t t; String s; };   // t: 0=user 1=AI 2=sistem
std::vector<Line> lines;
int scrollPos = 0;
String input;
bool dirty = true;
bool camOK = false;
bool wasConnected = false;
String netsHtml;

uint8_t *photoBuf = nullptr;
size_t photoLen = 0;
uint32_t previewUntil = 0;

std::vector<String> histQ, histA;

struct KeyEv { uint8_t ascii, code, mod; };
QueueHandle_t keyQ;

class KbHost : public EspUsbHost {
 public:
  void onKeyboardKey(uint8_t ascii, uint8_t keycode, uint8_t modifier) {
    KeyEv e{ascii, keycode, modifier};
    xQueueSend(keyQ, &e, 0);
  }
};
KbHost usbHost;

// ======================= TEKS: LaTeX -> Unicode ==================
static int matchBrace(const String &s, int open) {
  int d = 0;
  for (int i = open; i < (int)s.length(); i++) {
    if (s[i] == '{') d++;
    else if (s[i] == '}') { d--; if (d == 0) return i; }
  }
  return -1;
}

static const char *supOf(char c) {
  switch (c) {
    case '0': return "⁰"; case '1': return "¹"; case '2': return "²"; case '3': return "³";
    case '4': return "⁴"; case '5': return "⁵"; case '6': return "⁶"; case '7': return "⁷";
    case '8': return "⁸"; case '9': return "⁹"; case '+': return "⁺"; case '-': return "⁻";
    case '=': return "⁼"; case '(': return "⁽"; case ')': return "⁾"; case 'n': return "ⁿ";
    case 'i': return "ⁱ"; default: return nullptr;
  }
}
static const char *subOf(char c) {
  switch (c) {
    case '0': return "₀"; case '1': return "₁"; case '2': return "₂"; case '3': return "₃";
    case '4': return "₄"; case '5': return "₅"; case '6': return "₆"; case '7': return "₇";
    case '8': return "₈"; case '9': return "₉"; case '+': return "₊"; case '-': return "₋";
    case '=': return "₌"; case '(': return "₍"; case ')': return "₎"; case 'n': return "ₙ";
    case 'x': return "ₓ"; case 'a': return "ₐ"; case 'e': return "ₑ"; case 'o': return "ₒ";
    default: return nullptr;
  }
}

static String convertScripts(const String &s) {
  String out;
  size_t i = 0, n = s.length();
  while (i < n) {
    char c = s[i];
    bool isSup = (c == '^');
    bool isSub = (c == '_' && i + 1 < n && (isdigit((unsigned char)s[i + 1]) || s[i + 1] == '{'));
    if (isSup || isSub) {
      String body;
      size_t j = i + 1;
      if (j < n && s[j] == '{') {
        int e = matchBrace(s, j);
        if (e < 0) { out += c; i++; continue; }
        body = s.substring(j + 1, e);
        j = e + 1;
      } else if (j < n && isdigit((unsigned char)s[j])) {
        while (j < n && isdigit((unsigned char)s[j])) body += s[j++];
      } else if (isSup && j + 1 < n && s[j] == '-' && isdigit((unsigned char)s[j + 1])) {
        body += s[j++];
        while (j < n && isdigit((unsigned char)s[j])) body += s[j++];
      } else if (isSup && j < n && isalpha((unsigned char)s[j])) {
        body = String(s[j]); j++;
      } else { out += c; i++; continue; }
      String conv; bool ok = true;
      for (size_t k = 0; k < body.length(); k++) {
        const char *m = isSup ? supOf(body[k]) : subOf(body[k]);
        if (!m) { ok = false; break; }
        conv += m;
      }
      if (ok) out += conv;
      else { out += c; if (body.length() > 1) { out += "("; out += body; out += ")"; } else out += body; }
      i = j;
      continue;
    }
    out += c; i++;
  }
  return out;
}

static String wrapIf(const String &x) {
  if (x.length() > 1 && (x.indexOf(' ') >= 0 || x.indexOf('+') >= 0 || x.indexOf('-') >= 0 ||
                         x.indexOf('*') >= 0 || x.indexOf('/') >= 0)) return "(" + x + ")";
  return x;
}

static const char *const TEX[][2] = {
  {"\\Leftrightarrow", "⇔"}, {"\\leftarrow", "←"}, {"\\rightarrow", "→"}, {"\\Rightarrow", "⇒"},
  {"\\left", ""}, {"\\right", ""}, {"\\displaystyle", ""}, {"\\quad", " "}, {"\\,", " "}, {"\\;", " "}, {"\\!", ""},
  {"\\times", "×"}, {"\\cdot", "·"}, {"\\div", "÷"}, {"\\pm", "±"}, {"\\mp", "∓"},
  {"\\leq", "≤"}, {"\\geq", "≥"}, {"\\neq", "≠"}, {"\\approx", "≈"}, {"\\equiv", "≡"},
  {"\\infty", "∞"}, {"\\int", "∫"}, {"\\sum", "∑"}, {"\\prod", "∏"}, {"\\partial", "∂"}, {"\\nabla", "∇"},
  {"\\to", "→"}, {"\\alpha", "α"}, {"\\beta", "β"}, {"\\gamma", "γ"}, {"\\delta", "δ"}, {"\\Delta", "Δ"},
  {"\\epsilon", "ε"}, {"\\theta", "θ"}, {"\\lambda", "λ"}, {"\\mu", "μ"}, {"\\pi", "π"}, {"\\rho", "ρ"},
  {"\\sigma", "σ"}, {"\\Sigma", "Σ"}, {"\\tau", "τ"}, {"\\phi", "φ"}, {"\\omega", "ω"}, {"\\Omega", "Ω"},
  {"\\degree", "°"}, {"\\circ", "°"}, {"\\ldots", "…"}, {"\\dots", "…"}, {"\\forall", "∀"}, {"\\exists", "∃"},
  {"\\in", "∈"}, {"\\le", "≤"}, {"\\ge", "≥"}
};

static String cleanText(String s) {
  s.replace("\r", "");
  s.replace("\t", "  ");
  if (s.indexOf('\\') >= 0) {                       // ada LaTeX tersisa
    s.replace("\\dfrac", "\\frac");
    s.replace("\\tfrac", "\\frac");
    s.replace("\\(", ""); s.replace("\\)", ""); s.replace("\\[", ""); s.replace("\\]", "");
    s.replace("$$", ""); s.replace("$", "");
    int idx;
    while ((idx = s.lastIndexOf("\\frac{")) >= 0) {
      int ne = matchBrace(s, idx + 5);
      if (ne < 0 || ne + 1 >= (int)s.length() || s[ne + 1] != '{') { s.remove(idx, 5); continue; }
      int de = matchBrace(s, ne + 1);
      if (de < 0) { s.remove(idx, 5); continue; }
      String num = s.substring(idx + 6, ne), den = s.substring(ne + 2, de);
      s = s.substring(0, idx) + wrapIf(num) + "/" + wrapIf(den) + s.substring(de + 1);
    }
    while ((idx = s.lastIndexOf("\\sqrt{")) >= 0) {
      int e = matchBrace(s, idx + 5);
      if (e < 0) { s.remove(idx, 5); continue; }
      String c = s.substring(idx + 6, e);
      s = s.substring(0, idx) + "√" + (c.length() == 1 ? c : "(" + c + ")") + s.substring(e + 1);
    }
    static const char *wr[] = {"\\text{", "\\mathrm{", "\\mathbf{", "\\textbf{", "\\operatorname{", "\\mathit{", "\\boxed{"};
    for (auto w : wr) {
      int wl = strlen(w);
      while ((idx = s.indexOf(w)) >= 0) {
        int e = matchBrace(s, idx + wl - 1);
        if (e < 0) { s.remove(idx, wl); break; }
        s = s.substring(0, idx) + s.substring(idx + wl, e) + s.substring(e + 1);
      }
    }
    s = convertScripts(s);
    for (auto &p : TEX) s.replace(p[0], p[1]);
    s.replace("{", ""); s.replace("}", "");
  }
  // markdown ringan
  String out; int st = 0;
  while (st <= (int)s.length()) {
    int nl = s.indexOf('\n', st);
    String ln = (nl < 0) ? s.substring(st) : s.substring(st, nl);
    int h = 0; while (h < (int)ln.length() && ln[h] == '#') h++;
    if (h > 0 && h <= 6 && ln[h] == ' ') ln = ln.substring(h + 1);
    if (ln.startsWith("* ")) ln = "• " + ln.substring(2);
    else if (ln.startsWith("  * ")) ln = "  • " + ln.substring(4);
    out += ln;
    if (nl < 0) break;
    out += '\n'; st = nl + 1;
  }
  out.replace("**", ""); out.replace("`", "");
  return out;
}

static String jsonEsc(const String &s) {
  String o; o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') {}
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) {}
    else o += (char)c;
  }
  return o;
}

static String htmlEsc(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

// ========================= LAYAR & CHAT ==========================
static void useMainFont() {
  u8g2.setFont(u8g2_font_unifont_t_symbols);
  u8g2.setFontMode(1);
}

static void wrapText(const String &t, std::vector<String> &out, int maxW) {
  useMainFont();
  String line; int w = 0; int lastSp = -1;
  size_t i = 0;
  while (i < t.length()) {
    uint8_t c = t[i];
    int n = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    String ch = t.substring(i, i + n); i += n;
    if (ch == "\n") { out.push_back(line); line = ""; w = 0; lastSp = -1; continue; }
    int cw = u8g2.getUTF8Width(ch.c_str());
    if (w + cw > maxW) {
      if (lastSp > 0 && ch != " ") {
        String head = line.substring(0, lastSp), rest = line.substring(lastSp + 1);
        out.push_back(head); line = rest; w = u8g2.getUTF8Width(line.c_str());
      } else { out.push_back(line); line = ""; w = 0; }
      lastSp = -1;
      if (ch == " ") continue;
    }
    if (ch == " ") lastSp = line.length();
    line += ch; w += cw;
  }
  out.push_back(line);
}

static void addMessage(uint8_t type, const String &text, bool gap = true) {
  std::vector<String> w;
  wrapText(text, w, SCR_W - 6);
  for (auto &x : w) lines.push_back({type, x});
  if (gap) lines.push_back({type, ""});
  if (lines.size() > 1500) lines.erase(lines.begin(), lines.begin() + 300);
  int total = lines.size();
  scrollPos = max(0, total - CHAT_ROWS);
  dirty = true;
}

static String currentModel() { return prefs.getString("model", "gemini-2.5-flash"); }

static void drawScreen() {
  if ((int32_t)(millis() - previewUntil) < 0) return;   // sedang tampil foto
  if (!canvas) return;
  canvas->fillScreen(0);
  useMainFont();
  int total = lines.size();
  int maxStart = max(0, total - CHAT_ROWS);
  if (scrollPos > maxStart) scrollPos = maxStart;
  if (scrollPos < 0) scrollPos = 0;
  for (int r = 0; r < CHAT_ROWS; r++) {
    int idx = scrollPos + r;
    if (idx >= total) break;
    uint16_t col = lines[idx].t == 0 ? 0x07FF : (lines[idx].t == 1 ? 0xFFFF : 0xFFE0);
    u8g2.setForegroundColor(col);
    u8g2.drawUTF8(2, r * LINE_H, lines[idx].s.c_str());
  }
  if (total > CHAT_ROWS) {                               // scrollbar
    int H = CHAT_ROWS * LINE_H;
    int th = max(6, H * CHAT_ROWS / total);
    int ty = (H - th) * scrollPos / max(1, maxStart);
    canvas->fillRect(SCR_W - 3, ty, 3, th, 0x7BEF);
  }
  canvas->drawFastHLine(0, 193, SCR_W, 0x4208);
  // area input (2 baris terakhir)
  std::vector<String> il;
  wrapText("> " + input + "_", il, SCR_W - 6);
  int start = max(0, (int)il.size() - 2);
  u8g2.setForegroundColor(0x07E0);
  for (int r = 0; r < 2 && start + r < (int)il.size(); r++)
    u8g2.drawUTF8(2, 196 + r * LINE_H, il[start + r].c_str());
  // status bar
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setFontMode(1);
  u8g2.setFontPosTop();
  u8g2.setForegroundColor(0x8410);
  String st = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("AP:192.168.4.1");
  String m = currentModel(); m.replace("gemini-", "");
  st += " " + m + (photoBuf ? " [FOTO]" : "");
  if (st.length() > 39) st = st.substring(0, 39);
  u8g2.drawUTF8(2, 229, st.c_str());
  useMainFont();
  tft.startWrite();
  tft.setAddrWindow(0, 0, 240, 240);
  tft.writePixels(canvas->getBuffer(), 240 * 240);
  tft.endWrite();
  dirty = false;
}

// ============================ KAMERA =============================
static bool initCamera() {
  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
  c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK; c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN; c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_SVGA;        // 800x600 -> bagus untuk baca soal
  c.jpeg_quality = 10;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, CAM_VFLIP); s->set_hmirror(s, CAM_HMIRROR); }
  return true;
}

static int jpegDraw(JPEGDRAW *p) {
  tft.setAddrWindow(p->x, p->y, p->iWidth, p->iHeight);
  tft.writePixels(p->pPixels, (uint32_t)p->iWidth * p->iHeight, true, true);
  return 1;
}

static void takePhoto() {
  if (!camOK) { addMessage(2, "Kamera tidak aktif."); return; }
  for (int i = 0; i < 2; i++) {                    // buang frame lama
    camera_fb_t *w = esp_camera_fb_get();
    if (w) esp_camera_fb_return(w);
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { addMessage(2, "Gagal ambil foto."); return; }
  if (photoBuf) { free(photoBuf); photoBuf = nullptr; photoLen = 0; }
  photoBuf = (uint8_t *)ps_malloc(fb->len);
  if (!photoBuf) { esp_camera_fb_return(fb); addMessage(2, "Memori foto habis."); return; }
  memcpy(photoBuf, fb->buf, fb->len);
  photoLen = fb->len;
  esp_camera_fb_return(fb);

  tft.fillScreen(0);
  if (jpeg.openRAM(photoBuf, photoLen, jpegDraw)) {
    int w = jpeg.getWidth() / 4, h = jpeg.getHeight() / 4;
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    tft.startWrite();
    jpeg.decode((240 - w) / 2, (240 - h) / 2, JPEG_SCALE_QUARTER);
    tft.endWrite();
    jpeg.close();
  }
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(20, 14);   tft.print("Foto terlampir");
  tft.setCursor(20, 218);  tft.print("Ketik soal lalu Enter");
  previewUntil = millis() + PREVIEW_MS;
  dirty = true;
}

// ============================ GEMINI =============================
static bool callGemini(const String &q, String &out, String &err) {
  String key = prefs.getString("key", "");
  if (key.length() < 10) { err = "API key belum diisi. Buka halaman web."; return false; }
  if (WiFi.status() != WL_CONNECTED) { err = "WiFi belum terhubung."; return false; }
  String model = currentModel();

  size_t b64cap = photoBuf ? (4 * ((photoLen + 2) / 3) + 4) : 0;
  String body;
  body.reserve(b64cap + 6000);
  body += F("{\"system_instruction\":{\"parts\":[{\"text\":\"");
  body += jsonEsc(SYS_PROMPT);
  body += F("\"}]},\"contents\":[");
  for (size_t i = 0; i < histQ.size(); i++) {
    body += F("{\"role\":\"user\",\"parts\":[{\"text\":\""); body += jsonEsc(histQ[i]); body += F("\"}]},");
    body += F("{\"role\":\"model\",\"parts\":[{\"text\":\""); body += jsonEsc(histA[i]); body += F("\"}]},");
  }
  body += F("{\"role\":\"user\",\"parts\":[{\"text\":\""); body += jsonEsc(q); body += F("\"}");
  if (photoBuf) {
    unsigned char *b64 = (unsigned char *)ps_malloc(b64cap);
    if (b64) {
      size_t olen = 0;
      mbedtls_base64_encode(b64, b64cap, &olen, photoBuf, photoLen);
      body += F(",{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"");
      body.concat((const char *)b64, (unsigned int)olen);
      body += F("\"}}");
      free(b64);
    }
  }
  body += F("]}],\"generationConfig\":{\"maxOutputTokens\":8192}}");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(120000);
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
  if (!http.begin(client, url)) { err = "Gagal mulai HTTPS."; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", key);
  int code = http.POST((uint8_t *)body.c_str(), body.length());
  body = "";
  if (code <= 0) { err = HTTPClient::errorToString(code); http.end(); return false; }
  String resp = http.getString();
  http.end();

  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  filter["error"]["message"] = true;
  filter["promptFeedback"]["blockReason"] = true;
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  resp = "";
  if (code != 200) {
    const char *m = doc["error"]["message"];
    err = "HTTP " + String(code) + (m ? String(": ") + m : String(""));
    return false;
  }
  if (de) { err = String("JSON error: ") + de.c_str(); return false; }
  JsonArray parts = doc["candidates"][0]["content"]["parts"];
  for (JsonVariant p : parts) {
    const char *t = p["text"];
    if (t) out += t;
  }
  if (out.length() == 0) {
    const char *br = doc["promptFeedback"]["blockReason"];
    err = br ? String("Diblokir: ") + br : String("Jawaban kosong.");
    return false;
  }
  return true;
}

static void sendToGemini() {
  String q = input; q.trim();
  if (q.length() == 0 && !photoBuf) return;
  if (q.length() == 0) q = "Jelaskan foto ini. Jika ada soal, selesaikan.";
  addMessage(0, "Kamu: " + q + (photoBuf ? " [+foto]" : ""), false);
  input = "";
  lines.push_back({2, "Berpikir..."});
  scrollPos = max(0, (int)lines.size() - CHAT_ROWS);
  dirty = true; drawScreen();

  String ans, err;
  bool ok = callGemini(q, ans, err);
  lines.pop_back();
  if (ok) {
    ans = cleanText(ans);
    addMessage(1, ans);
    histQ.push_back(q); histA.push_back(ans);
    if ((int)histQ.size() > MAX_HISTORY) { histQ.erase(histQ.begin()); histA.erase(histA.begin()); }
  } else {
    addMessage(2, "Error: " + err);
  }
  if (photoBuf) { free(photoBuf); photoBuf = nullptr; photoLen = 0; }
  dirty = true;
}

// ========================== KEYBOARD =============================
static const char *ctrlSym(char l) {
  switch (l) {
    case 'p': return "π"; case 'r': return "√"; case 's': return "∑"; case 'i': return "∫";
    case 'd': return "Δ"; case 't': return "θ"; case 'a': return "α"; case 'b': return "β";
    case 'm': return "μ"; case 'w': return "ω"; case 'o': return "°"; case 'e': return "∞";
    case 'x': return "×"; case 'n': return "≠"; case 'u': return "≤"; case 'y': return "≥";
    case 'q': return "≈"; case 'f': return "∂"; case 'g': return "→";
    default: return nullptr;
  }
}

static void popUtf8(String &s) {
  if (s.length() == 0) return;
  int i = s.length() - 1;
  while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}

static void handleKey(const KeyEv &k) {
  if (k.code == 0x35 || k.ascii == '~' || k.ascii == '`') { takePhoto(); return; }
  if ((k.mod & 0x11) && k.code >= 0x04 && k.code <= 0x1D) {
    char l = 'a' + (k.code - 4);
    if (l == 'l') { lines.clear(); histQ.clear(); histA.clear(); scrollPos = 0; addMessage(2, "Chat dihapus."); return; }
    const char *s = ctrlSym(l);
    if (s && input.length() < 1000) input += s;
    dirty = true; return;
  }
  int total = lines.size();
  switch (k.code) {
    case 0x28: case 0x58: sendToGemini(); return;                       // Enter
    case 0x2A: popUtf8(input); dirty = true; return;                    // Backspace
    case 0x29:                                                          // Esc
      if (input.length()) input = "";
      else if (photoBuf) { free(photoBuf); photoBuf = nullptr; photoLen = 0; }
      dirty = true; return;
    case 0x52: scrollPos = max(0, scrollPos - 1); dirty = true; return;               // Up
    case 0x51: scrollPos = min(max(0, total - CHAT_ROWS), scrollPos + 1); dirty = true; return;  // Down
    case 0x4B: scrollPos = max(0, scrollPos - CHAT_ROWS + 1); dirty = true; return;   // PgUp
    case 0x4E: scrollPos = min(max(0, total - CHAT_ROWS), scrollPos + CHAT_ROWS - 1); dirty = true; return; // PgDn
    case 0x4A: scrollPos = 0; dirty = true; return;                                   // Home
    case 0x4D: scrollPos = max(0, total - CHAT_ROWS); dirty = true; return;           // End
  }
  if (k.ascii >= 32 && k.ascii < 127 && input.length() < 1000) { input += (char)k.ascii; dirty = true; }
}

// ============================= WEB ===============================
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Gemini ESP32</title><style>
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}
.c{max-width:460px;margin:auto;background:#1e293b;padding:18px;border-radius:14px}
h1{font-size:20px;margin:0 0 4px}small{color:#94a3b8}
label{display:block;margin:14px 0 4px;font-size:14px}
input,select,button{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #475569;background:#0f172a;color:#e2e8f0;font-size:15px}
button{background:#2563eb;border:0;margin-top:18px;font-weight:600}
.s{margin-top:12px;font-size:13px;color:#94a3b8}a{color:#60a5fa;font-size:13px}
</style></head><body><div class="c"><h1>Gemini ESP32-S3</h1><small>%STATUS%</small>
<form method="POST" action="/save">
<label>WiFi (SSID)</label><input name="ssid" value="%SSID%" list="nets" autocomplete="off">
<datalist id="nets">%NETS%</datalist><a href="/scan">Scan ulang WiFi</a>
<label>Password WiFi</label><input name="pass" type="password" placeholder="%PASSHINT%">
<label>API Key Gemini</label><input name="key" type="password" placeholder="%KEYHINT%">
<label>Model</label><select name="model">%MODELS%</select>
<label>Atau nama model lain (opsional, menimpa pilihan di atas)</label>
<input name="custom" value="%CUSTOM%" placeholder="mis. gemini-2.5-flash">
<button type="submit">Simpan</button></form>
<div class="s">Buat API key di aistudio.google.com/apikey. Kosongkan kolom password/API key bila tidak ingin mengubahnya.</div>
</div></body></html>)HTML";

static void scanNetworks() {
  int n = WiFi.scanNetworks();
  netsHtml = "";
  for (int i = 0; i < n; i++) netsHtml += "<option value=\"" + htmlEsc(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
}

static void handleRoot() {
  String p = PAGE;
  String status = (WiFi.status() == WL_CONNECTED) ? "WiFi terhubung: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")"
                                                  : "WiFi belum terhubung";
  String model = currentModel();
  bool preset = false;
  String opts;
  for (int i = 0; i < N_PRESETS; i++) {
    bool sel = (model == MODEL_PRESETS[i]);
    if (sel) preset = true;
    opts += String("<option") + (sel ? " selected" : "") + ">" + MODEL_PRESETS[i] + "</option>";
  }
  String key = prefs.getString("key", "");
  p.replace("%STATUS%", status);
  p.replace("%SSID%", htmlEsc(prefs.getString("ssid", "")));
  p.replace("%NETS%", netsHtml);
  p.replace("%PASSHINT%", prefs.getString("pass", "").length() ? "(tersimpan)" : "password WiFi");
  p.replace("%KEYHINT%", key.length() > 4 ? "tersimpan (..." + key.substring(key.length() - 4) + ")" : "tempel API key di sini");
  p.replace("%MODELS%", opts);
  p.replace("%CUSTOM%", preset ? "" : htmlEsc(model));
  server.send(200, "text/html; charset=utf-8", p);
}

static void handleSave() {
  String ssid = server.arg("ssid"), pass = server.arg("pass"), key = server.arg("key");
  String model = server.arg("custom"); model.trim();
  if (model.length() == 0) model = server.arg("model");
  if (model.startsWith("models/")) model = model.substring(7);
  if (model.length() == 0) model = "gemini-2.5-flash";
  String oldSsid = prefs.getString("ssid", "");
  bool wifiChanged = false;
  if (ssid.length()) {
    if (ssid != oldSsid || pass.length()) wifiChanged = true;
    if (pass.length() == 0 && ssid != oldSsid) prefs.putString("pass", "");
    if (pass.length()) prefs.putString("pass", pass);
    prefs.putString("ssid", ssid);
  }
  key.trim();
  if (key.length()) prefs.putString("key", key);
  prefs.putString("model", model);
  server.send(200, "text/html; charset=utf-8",
              "<meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;"
              "background:#0f172a;color:#e2e8f0;padding:24px'><h2>Tersimpan ✔</h2><p>Model: " + htmlEsc(model) +
              "</p><p><a style='color:#60a5fa' href='/'>Kembali</a></p></body>");
  if (wifiChanged) { WiFi.disconnect(false); WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str()); }
  addMessage(2, "Pengaturan disimpan. Model: " + model);
}

// ============================ SETUP ==============================
void setup() {
  Serial.begin(115200);
  Serial.println("\nGemini ESP32-S3 start");
  prefs.begin("gem", false);
  keyQ = xQueueCreate(32, sizeof(KeyEv));

  if (TFT_BL >= 0) { pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH); }
  tftSPI.begin(TFT_SCK, -1, TFT_MOSI, -1);
  tft.init(240, 240, SPI_MODE3);
  tft.setSPISpeed(40000000);
  tft.setRotation(0);
  tft.fillScreen(0);
  canvas = new GFXcanvas16(240, 240);
  if (!canvas || !canvas->getBuffer()) { Serial.println("Canvas gagal (PSRAM?)"); tft.print("PSRAM/Canvas gagal"); while (1) delay(1000); }
  u8g2.begin(*canvas);
  useMainFont();

  addMessage(2, "Gemini AI siap.", false);
  camOK = initCamera();
  addMessage(2, camOK ? "Kamera OK." : "Kamera GAGAL (cek pin/PSRAM).", false);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setAutoReconnect(true);
  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
  scanNetworks();
  MDNS.begin("gemini");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/scan", HTTP_GET, []() { scanNetworks(); server.sendHeader("Location", "/"); server.send(302); });
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();

  addMessage(2, String("Setting: sambung WiFi \"") + AP_SSID + "\" (pass " + AP_PASS + ") lalu buka 192.168.4.1");
  addMessage(2, "Ketik+Enter=kirim  ~=foto  Up/Down/PgUp/PgDn=scroll  Ctrl+L=hapus  Ctrl+P=π Ctrl+R=√");

  usbHost.begin();                 // keyboard USB-OTG
  drawScreen();
}

// ============================= LOOP ==============================
void loop() {
  server.handleClient();

  KeyEv k;
  while (xQueueReceive(keyQ, &k, 0) == pdTRUE) handleKey(k);

  bool conn = (WiFi.status() == WL_CONNECTED);
  if (conn != wasConnected) {
    wasConnected = conn;
    if (conn) addMessage(2, "WiFi OK. Buka http://" + WiFi.localIP().toString() + " untuk API key & model.");
    else dirty = true;
  }

  static bool previewWas = false;
  bool previewNow = (int32_t)(millis() - previewUntil) < 0;
  if (previewWas && !previewNow) dirty = true;      // preview selesai -> kembali ke chat
  previewWas = previewNow;

  if (dirty) drawScreen();
  delay(5);
}
