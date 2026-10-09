/********************************************************************************************************
 * Marco Mamone (e Claude.ai)																			*
 *																										*
 * MamoGB Emulator - Game Boy (DMG) emulator + browser streaming										*
 * Target: M5Stack Cardputer v1.1 (ESP32-S3, no PSRAM)													*
 *																										*
 * - Menu per scegliere una ROM (.gb / .gbc) dalla microSD ( mettere le ROM in una directory "/roms")	*
 * - Emulazione con Peanut-GB (MIT, https://github.com/deltabeard/Peanut-GB)							*
 * - La ROM non viene caricata in RAM: 4 KiB vengono cachati dalla microSD su richiesta del fw			*
 * - Salvataggio (.sav) in /saves																		*
 * - Wi-Fi AP + WebSocket streaming del framebuffer 160x144 2-bit nel browser							*
 *   Dal browser si possono anche dare input (si può giocare da pc emulando su CardPuter)				*
 *																										*
 * Librerie: M5Cardputer (+M5Unified, M5GFX), WebSockets di Markus Sattler.								*
 * La libreria peanut_gb.h deve trovarsi nella stessa directory di questo sketch						*
 *																										*
 * Keys (Cardputer):   ; . , /  or  W S A D = D-pad     J = A     K = B									*
 *                     Enter = START   Del = SELECT														*
 *                     ` (Esc) = Torna al menu   P = Cambia palette colori								*
 *                     Space = stream on/off															*
 * Browser: bottoni sullo schermo oppure tastiera (frecce/WASD, J, K, Enter, Backspace, Esc, P)			*
 *																										*
 *																										*
 * - Debug: Attraverso una console seriale (collegare il CardPuter al PC con baudrate = 115200) vengono *
 *			trasmessi ogni secondo le seguenti informazioni:											*
 *				FPS, memoria libera, frame più lungo, letture dalla SD e invio più lento.				*															*
 *			All’accensione il Cardputer mostra il motivo dell’ultimo riavvio, per esempio crash,		*
 *			watchdog o brownout, cioè calo di alimentazione.											*
 *																										*
 * - Se un invio dura più di 40 ms, lo streaming dimezza la frequenza, e la rialza						*
 *	 quando torna regolare.																				*
 *	 Dopo 15 secondi senza risposta (heartbeat) i client morti vengono scollegati.						*
 ********************************************************************************************************/
 
#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <setjmp.h>
#include <vector>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_system.h>					   

#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0   // 1 = more accurate, slower
#include "peanut_gb.h"

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------
static const char* AP_SSID = "MamoGB-Stream";
static const char* AP_PASS = "MamoGB-Stream";

// Cardputer microSD pins
#define SD_SCK  40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS   12

constexpr int GBW = 160;
constexpr int GBH = 144;
constexpr int FB_STRIDE = GBW / 4;          // bytes per row, 2 bits/pixel
constexpr int FB_BYTES = FB_STRIDE * GBH;   // 5760
constexpr int STREAM_DIV = 3;               // stream every Nth frame (~20 FPS)
constexpr uint32_t FRAME_US = 16742;        // 59.7275 Hz
constexpr int LOC_W = 150;                  // local display: 160x144 -> 150x135 (keeps aspect)
constexpr int LOC_H = 135;
constexpr int LOC_X = 45;
constexpr int MAX_ROMS = 300;

constexpr int BLK_SHIFT = 12;               // ROM cache block = 4 KiB
constexpr int BLK_SIZE = 1 << BLK_SHIFT;
constexpr size_t HEAP_RESERVE = 72 * 1024;  // left free for Wi-Fi, cart RAM, strings
constexpr int CACHE_MAX_BLOCKS = 96;
constexpr int CACHE_MIN_BLOCKS = 8;

// Defined here (before any function) because the Arduino IDE auto-generates
// function prototypes at the top of the file.
struct Keys {
  uint8_t pad = 0;     // JOYPAD_* bits (pressed = 1)
  bool esc = false;
  bool space = false;
  bool pal = false;
  bool any = false;    // any physical key
};

// ----------------------------------------------------------------------------
// Web page
// ----------------------------------------------------------------------------
static WebServer http(80);
static WebSocketsServer ws(81);

static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>MamoGB - Emulator</title>
<style>
body{margin:0;background:#101114;color:#eee;font-family:system-ui;text-align:center}
main{max-width:720px;margin:auto;padding:16px}h1{font-size:20px;margin:0 0 4px}
#st{color:#9aa0aa;font-size:13px;margin-bottom:12px}
.wrap{background:#050505;border:1px solid #30343c;border-radius:12px;padding:10px}
canvas{width:min(640px,100%);height:auto;image-rendering:pixelated;display:block;margin:auto;background:#000}
button{min-width:56px;height:48px;margin:3px;background:#242830;color:#eee;border:1px solid #444;border-radius:9px;font-size:16px;touch-action:none;user-select:none;-webkit-user-select:none}
.controls{margin-top:12px}.hint{color:#9aa0aa;font-size:12px;margin-top:8px}
</style></head><body><main>
<h1>MAMO GAMEBOY EMULATOR</h1><div id="st">connecting...</div>
<div class="wrap"><canvas id="c" width="160" height="144"></canvas></div>
<div class="controls">
<div><button data-k="up">&#9650;</button></div>
<div><button data-k="left">&#9664;</button><button data-k="down">&#9660;</button><button data-k="right">&#9654;</button></div>
<div><button data-k="b">B</button><button data-k="a">A</button></div>
<div><button data-k="select">SELECT</button><button data-k="start">START</button><button data-k="menu">MENU</button><button data-k="pal">PAL</button></div>
</div>
<div class="hint">WASD / arrows, J=A, K=B, Enter=START, Backspace=SELECT, Esc=MENU, P=palette</div>
</main><script>
const P=[[[224,248,208],[136,192,112],[52,104,86],[8,24,32]],
[[255,255,255],[176,176,176],[96,96,96],[0,0,0]],
[[196,207,161],[139,149,109],[77,83,60],[31,31,31]]];
let pal=P[0],lastB=null,s,frames=0,last=performance.now(),info='';
const c=document.getElementById('c'),x=c.getContext('2d',{alpha:false}),im=x.createImageData(160,144),st=document.getElementById('st');
function draw(b){let p=0;for(const q of b){for(let sh=6;sh>=0;sh-=2){const v=pal[(q>>sh)&3];im.data[p++]=v[0];im.data[p++]=v[1];im.data[p++]=v[2];im.data[p++]=255}}x.putImageData(im,0,0)}
function connect(){
 s=new WebSocket('ws://'+location.hostname+':81/');s.binaryType='arraybuffer';
 s.onopen=()=>{st.textContent='CONNECTED'};
 s.onclose=()=>{st.textContent='DISCONNECTED';setTimeout(connect,1000)};
 s.onmessage=e=>{
  if(typeof e.data==='string'){
   if(e.data.startsWith('PAL:')){pal=P[+e.data.slice(4)]||pal;if(lastB)draw(lastB)}
   else info=e.data;
   return}
  lastB=new Uint8Array(e.data);draw(lastB);frames++}
}
connect();
const down=new Set();
function key(k,d){if(d)down.add(k);else down.delete(k);if(s&&s.readyState===1)s.send((d?'D:':'U:')+k)}
document.querySelectorAll('[data-k]').forEach(b=>{const k=b.dataset.k;
 b.onpointerdown=e=>{e.preventDefault();key(k,1)};
 b.onpointerup=e=>{e.preventDefault();key(k,0)};
 b.onpointerleave=()=>{if(down.has(k))key(k,0)};
 b.onpointercancel=()=>key(k,0)});
function map(k){
 if(k==='ArrowUp'||k==='w'||k==='W')return'up';
 if(k==='ArrowDown'||k==='s'||k==='S')return'down';
 if(k==='ArrowLeft'||k==='a'||k==='A')return'left';
 if(k==='ArrowRight'||k==='d'||k==='D')return'right';
 if(k==='j'||k==='J')return'a';
 if(k==='k'||k==='K')return'b';
 if(k==='Enter')return'start';
 if(k==='Backspace')return'select';
 if(k==='Escape')return'menu';
 if(k==='p'||k==='P')return'pal';
 return null}
addEventListener('keydown',e=>{const k=map(e.key);if(k){e.preventDefault();if(!e.repeat)key(k,1)}});
addEventListener('keyup',e=>{const k=map(e.key);if(k){e.preventDefault();key(k,0)}});
addEventListener('blur',()=>{[...down].forEach(k=>key(k,0))});
setInterval(()=>{const now=performance.now(),fps=frames/((now-last)/1000);
 if(s&&s.readyState===1)st.textContent='CONNECTED · '+fps.toFixed(1)+' FPS'+(info?' · '+info:'');
 frames=0;last=now},1000);
</script></body></html>
)HTML";

// ----------------------------------------------------------------------------
// Palettes (index 0 = lightest). Must match the table in the web page.
// ----------------------------------------------------------------------------
static const uint8_t PALS[3][4][3] = {
  {{224, 248, 208}, {136, 192, 112}, {52, 104, 86}, {8, 24, 32}},      // DMG green
  {{255, 255, 255}, {176, 176, 176}, {96, 96, 96}, {0, 0, 0}},         // gray
  {{196, 207, 161}, {139, 149, 109}, {77, 83, 60}, {31, 31, 31}}       // pocket
};
static uint16_t pal565[4];
static int palIdx = 0;
static uint8_t quantLut[256];   // RGB332 -> nearest palette index (menu -> 2 bit)

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static void setPalette(int i) {
  palIdx = ((i % 3) + 3) % 3;
  for (int k = 0; k < 4; k++)
    pal565[k] = RGB565(PALS[palIdx][k][0], PALS[palIdx][k][1], PALS[palIdx][k][2]);
  for (int v = 0; v < 256; v++) {
    int r = ((v >> 5) & 7) * 255 / 7, g = ((v >> 2) & 7) * 255 / 7, b = (v & 3) * 255 / 3;
    int best = 0, bd = 1 << 30;
    for (int k = 0; k < 4; k++) {
      int dr = r - PALS[palIdx][k][0], dg = g - PALS[palIdx][k][1], db = b - PALS[palIdx][k][2];
      int d = dr * dr + dg * dg + db * db;
      if (d < bd) { bd = d; best = k; }
    }
    quantLut[v] = (uint8_t)best;
  }
}

// ----------------------------------------------------------------------------
// Shared state
// ----------------------------------------------------------------------------
static uint8_t fb[FB_BYTES];                 // 160x144, 2 bits/pixel, streamed to the browser
static volatile uint8_t webMask = 0;         // joypad bits pressed from the browser
static volatile bool webMenuReq = false;
static volatile bool webPalReq = false;
static volatile bool dirtySend = false;      // push fb to browsers (menu / new client)
static bool streamOn = true;

static uint16_t lineBuf[LOC_W];
static uint8_t xmapTab[LOC_W];

static void service() {
  http.handleClient();
  ws.loop();
}

static void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED) {
    char b[12];
    snprintf(b, sizeof b, "PAL:%d", palIdx);
    ws.sendTXT(num, b);
    ws.sendTXT(num, "MAMOGB READY");
    dirtySend = true;
    return;
  }
  if (type == WStype_DISCONNECTED) {
    webMask = 0;
    return;
  }
  if (type != WStype_TEXT || len < 3) return;
  char m[24];
  size_t n = len < sizeof(m) - 1 ? len : sizeof(m) - 1;
  memcpy(m, payload, n);
  m[n] = 0;
  bool down = (m[0] == 'D'), up = (m[0] == 'U');
  if ((!down && !up) || m[1] != ':') return;
  const char* k = m + 2;
  uint8_t bit = 0;
  if (!strcmp(k, "up")) bit = JOYPAD_UP;
  else if (!strcmp(k, "down")) bit = JOYPAD_DOWN;
  else if (!strcmp(k, "left")) bit = JOYPAD_LEFT;
  else if (!strcmp(k, "right")) bit = JOYPAD_RIGHT;
  else if (!strcmp(k, "a")) bit = JOYPAD_A;
  else if (!strcmp(k, "b")) bit = JOYPAD_B;
  else if (!strcmp(k, "start")) bit = JOYPAD_START;
  else if (!strcmp(k, "select")) bit = JOYPAD_SELECT;
  else if (!strcmp(k, "menu")) { if (down) webMenuReq = true; return; }
  else if (!strcmp(k, "pal")) { if (down) webPalReq = true; return; }
  if (bit) webMask = down ? (webMask | bit) : (webMask & ~bit);
}

// ----------------------------------------------------------------------------
// Input (Cardputer keyboard + browser)
// ----------------------------------------------------------------------------
static Keys readKeys() {
  Keys k;
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isPressed()) {
    const auto& st = M5Cardputer.Keyboard.keysState();
    k.any = true;
    for (char c : st.word) {
      switch (c) {
        case ';': case 'w': case 'W': k.pad |= JOYPAD_UP; break;
        case '.': case 's': case 'S': k.pad |= JOYPAD_DOWN; break;
        case ',': case 'a': case 'A': k.pad |= JOYPAD_LEFT; break;
        case '/': case 'd': case 'D': k.pad |= JOYPAD_RIGHT; break;
        case 'j': case 'J': k.pad |= JOYPAD_A; break;
        case 'k': case 'K': k.pad |= JOYPAD_B; break;
        case '`': k.esc = true; break;
        case 'p': case 'P': k.pal = true; break;
        case ' ': k.space = true; break;
        default: break;
      }
    }
    if (st.enter) k.pad |= JOYPAD_START;
    if (st.del) k.pad |= JOYPAD_SELECT;
  }
  k.pad |= webMask;
  if (webMenuReq) { k.esc = true; webMenuReq = false; }
  if (webPalReq) { k.pal = true; webPalReq = false; }
  return k;
}

// ----------------------------------------------------------------------------
// Menu canvas (8-bit sprite, 160x135). Rendered to the local screen and
// quantised into the 2-bit framebuffer so the browser sees the menu too.
// ----------------------------------------------------------------------------
static M5Canvas* canvas = nullptr;

static bool canvasBegin() {
  if (canvas) return true;
  canvas = new M5Canvas(&M5Cardputer.Display);
  canvas->setColorDepth(8);
  if (!canvas->createSprite(160, LOC_H)) {
    delete canvas;
    canvas = nullptr;
    return false;
  }
  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1);
  canvas->setTextWrap(false);
  return true;
}

static void canvasEnd() {
  if (!canvas) return;
  canvas->deleteSprite();
  delete canvas;
  canvas = nullptr;
}

static void present() {
  if (!canvas) return;
  const uint8_t* buf = (const uint8_t*)canvas->getBuffer();
  if (!buf) return;
  memset(fb, 0, sizeof(fb));  // 0 = lightest = menu background
  for (int y = 0; y < LOC_H; y++) {
    uint8_t* row = fb + (y + 4) * FB_STRIDE;
    const uint8_t* src = buf + y * 160;
    for (int i = 0; i < FB_STRIDE; i++) {
      const uint8_t* p = src + i * 4;
      row[i] = (quantLut[p[0]] << 6) | (quantLut[p[1]] << 4) | (quantLut[p[2]] << 2) | quantLut[p[3]];
    }
  }
  canvas->pushSprite(&M5Cardputer.Display, 40, 0);
  dirtySend = true;
}

static void drawHeader(const char* right) {
  const int rowH = canvas->fontHeight() + 1;
  canvas->fillRect(0, 0, 160, rowH, pal565[3]);
  canvas->setTextColor(pal565[0], pal565[3]);
  canvas->drawString("MAMO GAMEBOY", 3, 1);
  if (right)
    canvas->drawRightString(right, 157, 1);
}

static void showMessage(const char* l1, const char* l2 = nullptr, const char* l3 = nullptr) {
  if (!canvasBegin()) return;
  const int rowH = canvas->fontHeight() + 1;
  canvas->fillScreen(pal565[0]);
  drawHeader(nullptr);
  canvas->setTextColor(pal565[3], pal565[0]);
  int y = rowH + 14;
  if (l1) { canvas->drawString(l1, 6, y); y += rowH + 4; }
  if (l2) { canvas->drawString(l2, 6, y); y += rowH + 4; }
  if (l3) { canvas->drawString(l3, 6, y); }
  present();
}

static void waitAnyKey() {
  while (true) {  // release first
    service();
    Keys k = readKeys();
    if (!k.any && !k.pad && !k.esc) break;
    delay(10);
  }
  while (true) {
    service();
    if (dirtySend && ws.connectedClients()) { ws.broadcastBIN(fb, FB_BYTES); }
    dirtySend = false;
    Keys k = readKeys();
    if (k.any || k.pad || k.esc) break;
    delay(10);
  }
}

// ----------------------------------------------------------------------------
// SD card + ROM list
// ----------------------------------------------------------------------------
static bool sdOk = false;
static String romDir = "/roms";
static std::vector<String> roms;

static bool sdMount() {
  SD.end();
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);	//Comunica con MicroSD via SPI
  sdOk = SD.begin(SD_CS, SPI, 25000000);	//Clock a 25MHz per la microSD
  if (sdOk && SD.cardType() == CARD_NONE) sdOk = false;
  return sdOk;
}

//Cerca le ROM all'interno della directory /roms
static void scanRoms() {
  roms.clear();
  if (!sdOk) return;
  File d = SD.open("/roms");
  if (d && d.isDirectory()) {
    romDir = "/roms";
  } else {
    if (d) d.close();
    d = SD.open("/");
    romDir = "";
  }
  if (!d) return;
  File f = d.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String n = f.name();
      int s = n.lastIndexOf('/');
      if (s >= 0) n = n.substring(s + 1);
      String l = n;
      l.toLowerCase();
      if (!n.startsWith(".") && (l.endsWith(".gb") || l.endsWith(".gbc"))) roms.push_back(n);
    }
    f.close();
    if ((int)roms.size() >= MAX_ROMS) break;
    f = d.openNextFile();
  }
  d.close();
  std::sort(roms.begin(), roms.end(), [](const String& a, const String& b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
}

static String baseName(const String& n) {
  int dot = n.lastIndexOf('.');
  return dot > 0 ? n.substring(0, dot) : n;
}

// ----------------------------------------------------------------------------
// ROM block cache (ROM stays on the SD card)
// ----------------------------------------------------------------------------
static File romFile;
static uint32_t romSize = 0;
static uint8_t* cacheMem = nullptr;
static int cacheN = 0;
static int32_t* cacheTag = nullptr;
static uint32_t* cacheStamp = nullptr;
static uint32_t stampCtr = 0;
static int32_t hotTag[4] = {-1, -1, -1, -1};
static uint8_t* hotPtr[4] = {nullptr, nullptr, nullptr, nullptr};
static uint8_t hotNext = 0;
static uint32_t statMiss = 0;      // ROM cache misses (SD reads) in the last second
static uint32_t statMissMaxUs = 0; // slowest SD read in the last second
static void cacheFree() {
  free(cacheMem); cacheMem = nullptr;
  free(cacheTag); cacheTag = nullptr;
  free(cacheStamp); cacheStamp = nullptr;
  cacheN = 0;
  for (int i = 0; i < 4; i++) { hotTag[i] = -1; hotPtr[i] = nullptr; }
}

static bool cacheInit(uint32_t bytes) {
  cacheFree();
  size_t freeMax = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  int want = (int)((bytes + BLK_SIZE - 1) / BLK_SIZE);
  int fit = freeMax > HEAP_RESERVE ? (int)((freeMax - HEAP_RESERVE) / BLK_SIZE) : 0;
  int n = std::min(std::min(want, fit), CACHE_MAX_BLOCKS);
  if (n < std::min(want, CACHE_MIN_BLOCKS)) return false;
  cacheMem = (uint8_t*)malloc((size_t)n * BLK_SIZE);
  cacheTag = (int32_t*)malloc(sizeof(int32_t) * n);
  cacheStamp = (uint32_t*)malloc(sizeof(uint32_t) * n);
  if (!cacheMem || !cacheTag || !cacheStamp) { cacheFree(); return false; }
  cacheN = n;
  for (int i = 0; i < n; i++) { cacheTag[i] = -1; cacheStamp[i] = 0; }
  stampCtr = 0;
  return true;
}

static inline bool isHotTag(int32_t t) {
  return t == hotTag[0] || t == hotTag[1] || t == hotTag[2] || t == hotTag[3];
}

static uint8_t* cacheGet(uint32_t blk) {
  int victim = -1;
  uint32_t best = 0xFFFFFFFFu;
  bool empty = false;
  for (int i = 0; i < cacheN; i++) {
    int32_t t = cacheTag[i];
    if (t == (int32_t)blk) {
      cacheStamp[i] = ++stampCtr;
      return cacheMem + (size_t)i * BLK_SIZE;
    }
    if (t < 0) {
      if (!empty) { victim = i; empty = true; }
    } else if (!empty && !isHotTag(t) && cacheStamp[i] < best) {
      best = cacheStamp[i];
      victim = i;
    }
  }
  if (victim < 0) victim = 0;  // should not happen (cacheN >= 4 > hot entries... fallback)
  for (int h = 0; h < 4; h++)
    if (hotTag[h] == cacheTag[victim]) { hotTag[h] = -1; hotPtr[h] = nullptr; }
  uint8_t* p = cacheMem + (size_t)victim * BLK_SIZE;
  uint32_t off = blk << BLK_SHIFT;
  size_t got = 0;
  uint32_t t0 = micros();
  if (off < romSize && romFile.seek(off)) got = romFile.read(p, BLK_SIZE);
  uint32_t dt = micros() - t0;
  statMiss++;
  if (dt > statMissMaxUs) statMissMaxUs = dt;
  if (got < (size_t)BLK_SIZE) memset(p + got, 0xFF, BLK_SIZE - got);
  cacheTag[victim] = (int32_t)blk;
  cacheStamp[victim] = ++stampCtr;
  return p;
}

static uint8_t romRead(struct gb_s*, const uint_fast32_t addr) {
  const int32_t blk = (int32_t)(addr >> BLK_SHIFT);
  const uint32_t o = addr & (BLK_SIZE - 1);
  if (blk == hotTag[0]) return hotPtr[0][o];
  if (blk == hotTag[1]) return hotPtr[1][o];
  if (blk == hotTag[2]) return hotPtr[2][o];
  if (blk == hotTag[3]) return hotPtr[3][o];
  uint8_t* p = cacheGet((uint32_t)blk);
  hotTag[hotNext] = blk;
  hotPtr[hotNext] = p;
  hotNext = (hotNext + 1) & 3;
  return p[o];
}

// ----------------------------------------------------------------------------
// Cartridge RAM + saves
// ----------------------------------------------------------------------------
static uint8_t* cartRam = nullptr;
static size_t cartRamSize = 0;
static bool ramDirty = false;
static uint32_t ramDirtyAt = 0;

static uint8_t cartRamRead(struct gb_s*, const uint_fast32_t addr) {
  return addr < cartRamSize ? cartRam[addr] : 0xFF;
}

static void cartRamWrite(struct gb_s*, const uint_fast32_t addr, const uint8_t v) {
  if (addr < cartRamSize && cartRam[addr] != v) {
    cartRam[addr] = v;
    ramDirty = true;
    ramDirtyAt = millis();
  }
}

static bool loadSave(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t n = f.size();
  if (n > cartRamSize) n = cartRamSize;
  size_t got = f.read(cartRam, n);
  f.close();
  return got > 0;
}

static bool writeSave(const String& path) {
  if (!cartRam || !cartRamSize) return true;
  String tmp = path + ".tmp";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  size_t n = f.write(cartRam, cartRamSize);
  f.close();
  if (n != cartRamSize) { SD.remove(tmp); return false; }
  SD.remove(path);
  return SD.rename(tmp, path);
}

// ----------------------------------------------------------------------------
// Emulator glue
// ----------------------------------------------------------------------------
static struct gb_s gb;
static jmp_buf emuJmp;
static volatile int emuErr = 0;
static volatile uint16_t emuErrAddr = 0;
static volatile int initRes = 0;

// Peanut-GB requires this callback to NOT return.
static void gbError(struct gb_s*, const enum gb_error_e e, const uint16_t addr) {
  emuErr = (int)e;
  emuErrAddr = addr;
  longjmp(emuJmp, 1);
}

static bool runFrameSafe() {
  if (setjmp(emuJmp) == 0) {
    gb_run_frame(&gb);
    return true;
  }
  return false;
}

static bool initSafe() {
  if (setjmp(emuJmp) == 0) {
    initRes = (int)gb_init(&gb, romRead, cartRamRead, cartRamWrite, gbError, nullptr);
    return true;
  }
  return false;
}

static void lcdLine(struct gb_s*, const uint8_t* pixels, const uint_fast8_t line) {
  if (line >= GBH) return;
  // stream framebuffer (2 bits/pixel, 0 = lightest)
  uint8_t* d = fb + line * FB_STRIDE;
  for (int i = 0; i < FB_STRIDE; i++) {
    const uint8_t* p = pixels + i * 4;
    d[i] = ((p[0] & 3) << 6) | ((p[1] & 3) << 4) | ((p[2] & 3) << 2) | (p[3] & 3);
  }
  // local screen: 144 lines -> 135 rows, 160 px -> 150 px (nearest neighbour)
  const int dy = (line * LOC_H) / GBH;
  if (line > 0 && dy == (((int)line - 1) * LOC_H) / GBH) return;
  for (int i = 0; i < LOC_W; i++) lineBuf[i] = pal565[pixels[xmapTab[i]] & 3];
  M5Cardputer.Display.pushImage(LOC_X, dy, LOC_W, 1, lineBuf);
}

static void endGame(const String& savePath) {
  if (cartRam && ramDirty) writeSave(savePath);
  ramDirty = false;
  free(cartRam);
  cartRam = nullptr;
  cartRamSize = 0;
  cacheFree();
  if (romFile) romFile.close();
}

static void playRom(const String& name) {
  String path = romDir + "/" + name;
  String savePath = "/saves/" + baseName(name) + ".sav";

  romFile = SD.open(path, FILE_READ);
  if (!romFile) { showMessage("Errore apertura", "ROM"); waitAnyKey(); return; }
  romSize = romFile.size();
  if (romSize < 0x150) {
    romFile.close();
    showMessage("ROM non valida", "(file troppo piccolo)");
    waitAnyKey();
    return;
  }

  canvasEnd();  // free the menu canvas: we need the RAM for the ROM cache

  if (!cacheInit(romSize)) {
    romFile.close();
    showMessage("RAM insufficiente", "per la cache ROM");
    waitAnyKey();
    return;
  }

  if (!initSafe() || initRes != GB_INIT_NO_ERROR) {
    int ie = initRes;
    cacheFree();
    romFile.close();
    if (ie == GB_INIT_CARTRIDGE_UNSUPPORTED) showMessage("Cartuccia (MBC)", "non supportata");
    else showMessage("ROM non valida", "(checksum errato)");
    waitAnyKey();
    return;
  }

  char title[20];
  gb_get_rom_name(&gb, title);
  if (!title[0]) {
    strncpy(title, name.c_str(), sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
  }

  size_t rs = 0;
  if (gb_get_save_size_s(&gb, &rs) != 0) rs = 0;
  cartRamSize = rs;
  cartRam = nullptr;
  if (rs) {
    cartRam = (uint8_t*)malloc(rs);
    if (!cartRam) {
      cartRamSize = 0;
      cacheFree();
      romFile.close();
      showMessage("RAM cartuccia", "troppo grande");
      waitAnyKey();
      return;
    }
    memset(cartRam, 0, rs);
    SD.mkdir("/saves");
    loadSave(savePath);
  }
  ramDirty = false;

  gb_init_lcd(&gb, lcdLine);
  gb.direct.interlace = false;
  gb.direct.frame_skip = false;
  gb.direct.joypad = 0xFF;

  M5Cardputer.Display.fillScreen(TFT_BLACK);
  dirtySend = false;
  if (ws.connectedClients()) ws.broadcastTXT(title);

  bool prevEsc = false, prevSpace = false, prevPal = false;
  uint32_t next = micros();
  uint32_t fpsT = millis(), fpsN = 0, streamCtr = 0;
  uint32_t streamDiv = STREAM_DIV;      // adaptive: grows when the browser is slow to receive
  uint32_t wsMaxUs = 0, frameMaxUs = 0, goodSends = 0;
  float fps = 0;
  int lateRun = 0, earlyRun = 0;
  bool skip = false;
  bool crashed = false;


  while (true) {
    service();
    Keys k = readKeys();

    if (k.esc && !prevEsc) break;
    if (k.space && !prevSpace) streamOn = !streamOn;
    if (k.pal && !prevPal) {
      setPalette(palIdx + 1);
      char b[12];
      snprintf(b, sizeof b, "PAL:%d", palIdx);
      if (ws.connectedClients()) ws.broadcastTXT(b);
    }
    prevEsc = k.esc;
    prevSpace = k.space;
    prevPal = k.pal;

    gb.direct.joypad = (uint8_t)~k.pad;

    M5Cardputer.Display.startWrite();
    bool ok = runFrameSafe();
    M5Cardputer.Display.endWrite();
    if (!ok) { crashed = true; break; }

    fpsN++;
    if (streamOn && (++streamCtr % STREAM_DIV) == 0 && ws.connectedClients())
      ws.broadcastBIN(fb, FB_BYTES);

    uint32_t nowMs = millis();
    if (nowMs - fpsT >= 1000) {
      fps = fpsN * 1000.0f / (nowMs - fpsT);
      fpsT = nowMs;
      fpsN = 0;
      if (ws.connectedClients()) {
        char info[64];
        snprintf(info, sizeof info, "%s · emu %.1f FPS%s", title, fps, skip ? " (skip)" : "");
        ws.broadcastTXT(info);
      }
    }

    if (ramDirty && (nowMs - ramDirtyAt) > 2000) {  // autosave 2 s after the last write
      if (writeSave(savePath)) ramDirty = false;
      else ramDirtyAt = nowMs;
      next = micros();
    }

    // frame pacing
    next += FRAME_US;
    int32_t d = (int32_t)(next - micros());
    if (d < -(int32_t)(3 * FRAME_US)) { next = micros(); d = 0; }
    if (d > 0) {
      if (d > 2500) delay((d - 1500) / 1000);
      while ((int32_t)(next - micros()) > 0) { }
      lateRun = 0;
      if (d > 3000) { if (++earlyRun > 120 && skip) { skip = false; earlyRun = 0; } }
      else earlyRun = 0;
    } else {
      earlyRun = 0;
      if (++lateRun >= 4 && !skip) { skip = true; lateRun = 0; }  // auto frame skip when too slow
    }
    gb.direct.frame_skip = skip;
  }

  endGame(savePath);

  if (crashed) {
    char l2[24];
    snprintf(l2, sizeof l2, "tipo %d @ %04X", (int)emuErr, (unsigned)emuErrAddr);
    showMessage("Errore emulazione", l2, "premi un tasto");
    waitAnyKey();
  }
  M5Cardputer.Display.fillScreen(TFT_BLACK);
}

// ----------------------------------------------------------------------------
// Menu
// ----------------------------------------------------------------------------
static int selIdx = 0, topIdx = 0;

// Footer = 3 lines (URL / SSID / password) in a small 6x8 font.
constexpr int FOOT_LINE_H = 9;
constexpr int FOOT_H = 3 * FOOT_LINE_H + 2;

static int menuRows() {
  const int rowH = canvas->fontHeight() + 1;          // Font2 row height
  return (LOC_H - rowH - FOOT_H) / rowH;              // minus header and footer
}

static void drawMenu(int rows) {
  const int rowH = canvas->fontHeight() + 1;
  canvas->fillScreen(pal565[0]);

  //Mostra counter rom
  //char cnt[16];
  //snprintf(cnt, sizeof cnt, "%d/%d", roms.empty() ? 0 : selIdx + 1, (int)roms.size());
  //drawHeader(cnt);

  //Mostra indicatore batteria
  char bat[16];
  snprintf(bat, sizeof(bat), "%d%%", M5.Power.getBatteryLevel());
  drawHeader(bat);

  for (int r = 0; r < rows; r++) {
    int idx = topIdx + r;
    if (idx >= (int)roms.size()) break;
    int y = rowH + r * rowH;
    bool sel = (idx == selIdx);
    uint16_t bg = sel ? pal565[2] : pal565[0];
    uint16_t fg = sel ? pal565[0] : pal565[3];
    canvas->fillRect(0, y, 160, rowH, bg);
    canvas->setTextColor(fg, bg);
    String s = baseName(roms[idx]);
    while (s.length() > 1 && canvas->textWidth(s.c_str()) > 152) s.remove(s.length() - 1);
    canvas->drawString(s.c_str(), 4, y + 1);
  }

  // footer: three lines, small font
  const int fy = LOC_H - FOOT_H;
  canvas->fillRect(0, fy, 160, FOOT_H, pal565[3]);
  canvas->setFont(&fonts::Font0);
  canvas->setTextColor(pal565[0], pal565[3]);
  String url = "http://" + WiFi.softAPIP().toString();
  String ssid = "WiFi: " + String(AP_SSID);
  String pwd = "PWD: " + String(AP_PASS);
  canvas->drawString(url.c_str(), 3, fy + 2);
  canvas->drawString(ssid.c_str(), 3, fy + 2 + FOOT_LINE_H);
  canvas->drawString(pwd.c_str(), 3, fy + 2 + 2 * FOOT_LINE_H);
  canvas->setFont(&fonts::Font2);                     // restore for header/list/messages
  present();
}

static int menuPick() {
  if (!canvasBegin()) return -1;
  const int rows = menuRows();
  uint8_t prev = 0;
  bool prevPal = false;
  uint32_t repeatAt = 0;
  bool redraw = true;

  if (ws.connectedClients()) ws.broadcastTXT("MENU");
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  while (true) {
    service();
    Keys k = readKeys();
    uint8_t press = k.pad & ~prev;
    uint8_t mv = k.pad & (JOYPAD_UP | JOYPAD_DOWN);
    if (mv) {
      uint32_t now = millis();
      if (press & mv) repeatAt = now + 350;
      else if (now >= repeatAt) { press |= mv; repeatAt = now + 90; }
    }
    prev = k.pad;

    if (k.pal && !prevPal) {
      setPalette(palIdx + 1);
      char b[12];
      snprintf(b, sizeof b, "PAL:%d", palIdx);
      if (ws.connectedClients()) ws.broadcastTXT(b);
      redraw = true;
    }
    prevPal = k.pal;

    if (roms.empty()) {
      if (press || k.any) {  // retry (card inserted / ROMs copied)
        sdMount();
        scanRoms();
        selIdx = topIdx = 0;
        redraw = true;
      }
      if (redraw) {
        if (!sdOk) showMessage("Inserisci microSD", "poi premi un tasto");
        else showMessage("Nessuna ROM trovata", "metti i .gb in /roms", "poi premi un tasto");
        redraw = false;
      }
    } else {
      int n = (int)roms.size();
      if (press & JOYPAD_UP) { selIdx = (selIdx + n - 1) % n; redraw = true; }
      if (press & JOYPAD_DOWN) { selIdx = (selIdx + 1) % n; redraw = true; }
      if (press & JOYPAD_LEFT) { selIdx = std::max(0, selIdx - rows); redraw = true; }
      if (press & JOYPAD_RIGHT) { selIdx = std::min(n - 1, selIdx + rows); redraw = true; }
      if (selIdx < topIdx) topIdx = selIdx;
      if (selIdx >= topIdx + rows) topIdx = selIdx - rows + 1;
      if (press & (JOYPAD_A | JOYPAD_START)) {
        // wait for release so the game does not start with START pressed
        while (true) {
          service();
          Keys r = readKeys();
          if (!r.pad && !r.any) break;
          delay(10);
        }
        return selIdx;
      }
      if (redraw) { drawMenu(rows); redraw = false; }
    }

    if (dirtySend) {
      if (ws.connectedClients()) ws.broadcastBIN(fb, FB_BYTES);
      dirtySend = false;
    }
    delay(15);
  }
}

// ----------------------------------------------------------------------------
// Setup / loop
// ----------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE);
  M5Cardputer.Display.setCursor(5, 5);
  M5Cardputer.Display.println("MAMO GAMEBOY EMULATOR");
  M5Cardputer.Display.println("WiFi AP starting...");

  for (int i = 0; i < LOC_W; i++) xmapTab[i] = (uint8_t)((i * GBW) / LOC_W);
  setPalette(0);
  memset(fb, 0, sizeof(fb));

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASS);
  M5Cardputer.Display.print("SSID: ");
  M5Cardputer.Display.println(AP_SSID);
  M5Cardputer.Display.print("IP: ");
  M5Cardputer.Display.println(WiFi.softAPIP());

  http.on("/", HTTP_GET, []() { http.send_P(200, "text/html", PAGE); });
  http.begin();
  ws.begin();
  ws.onEvent(wsEvent);

  sdMount();
  scanRoms();
  M5Cardputer.Display.println(sdOk ? "SD ok" : "SD non trovata");
  delay(1200);
}

void loop() {
  int sel = menuPick();
  if (sel >= 0 && sel < (int)roms.size()) {
    String name = roms[sel];
    playRom(name);
  }
}
