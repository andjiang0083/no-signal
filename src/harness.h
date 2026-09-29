/*
 * Hardwired Test Harness  v1.0
 * ============================
 * Reusable remote test framework for ESP32+WebServer projects.
 *
 * Drop this ONE header into any project, add 3 lines of glue, and
 * you instantly get remote screenshot + button simulation + debug.
 *
 * ── Integration ──────────────────────────────────────────
 *   #include "harness.h"
 *   HardwiredTestHarness hrns;            // 1. instantiate
 *   void setup() {
 *     hrns.begin(server, framebuffer_fn); // 2. init
 *   }
 *   void loop() {
 *     hrns.tick();
 *     if (hrns.btnClicked(HRNS_BTN_A) || M5.BtnA.wasClicked())
 *       switchFrame();
 *   }
 *
 * ── HTTP Endpoints ──────────────────────────────────────
 *   GET  /hrns/bmp   → screen BMP (240×135, ~65KB)
 *   POST /hrns/btn   → simulate button press
 *     Content-Type: application/json
 *     Body: {"btn":0, "action":"click"}   (btn: 0=A, 1=B)
 *     Body: {"btn":1, "action":"long"}
 *   GET  /hrns/debug → JSON state dump
 *   POST /hrns/reset → ESP.restart()
 *   POST /hrns/data  → inject data (calls onData callback)
 *
 * ── PC Side ─────────────────────────────────────────────
 *   ./harness.sh <ip> snap        # screenshot
 *   ./harness.sh <ip> btn a       # press BtnA
 *   ./harness.sh <ip> btn b long  # BtnB long press
 *   ./harness.sh <ip> debug       # JSON state
 *   ./harness.sh <ip> reset       # restart device
 *
 * ── RAM / Flash ─────────────────────────────────────────
 *   RAM: ~240 bytes  Flash: ~2.5KB  (when no BMP request is active,
 *   the 65KB String is allocated on-heap and freed immediately)
 */

#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <esp_heap_caps.h>   // v0.0.17：BMP 缓冲显式分配（PSRAM 优先）

// ─── Config (override via build flags) ────────────────────
#ifndef HRNS_BMP_W
  #define HRNS_BMP_W 240
#endif
#ifndef HRNS_BMP_H
  #define HRNS_BMP_H 135
#endif

#define HRNS_BTN_A 0
#define HRNS_BTN_B 1

// ─── Forward declaration for static wrappers ──────────────
class HardwiredTestHarness;
static HardwiredTestHarness* g_hrns = nullptr;

class HardwiredTestHarness {
public:
  HardwiredTestHarness() {}

  // ── Setup ──────────────────────────────────────────────
  // Call in setup() after server is created.
  //   server   — reference to your WebServer instance
  //   getFb    — function returning uint16_t* to the sprite framebuffer
  void begin(WebServer& server, uint16_t* (*getFb)()) {
    _server = &server;
    _getFb = getFb;
    g_hrns = this;
    server.on("/hrns/bmp",   HTTP_GET,  _wrap_bmp);
    server.on("/hrns/btn",   HTTP_POST, _wrap_btn);
    server.on("/hrns/debug", HTTP_GET,  _wrap_debug);
    server.on("/hrns/reset", HTTP_POST, _wrap_reset);
    server.on("/hrns/data",  HTTP_POST, _wrap_data);
    server.on("/hrns/frame", HTTP_POST, _wrap_frame);
  }

  // ── Tick — call every loop() ──────────────────────────
  void tick() {}

  // ── Button query ──────────────────────────────────────
  // Returns true exactly once per simulated button press,
  // then clears the flag. OR with the real M5 button check:
  //   if (hrns.btnClicked(HRNS_BTN_A) || M5.BtnA.wasClicked())
  bool btnClicked(int id) {
    if (id < 0 || id > 1) return false;
    if (_btn[id].pending) { _btn[id].pending = false; return true; }
    return false;
  }

  // ── Data injection callback ───────────────────────────
  // Set by the project to receive POST /hrns/data payloads.
  void onData(void (*cb)(const String&)) { _dataCb = cb; }

  // ── Frame jump callback ───────────────────────────────
  // Set by the project to receive POST /hrns/frame requests.
  // Passes the requested frame index (0-based).
  void onSetFrame(void (*cb)(int)) { _setFrameCb = cb; }

private:
  struct { bool pending = false; } _btn[2];
  WebServer* _server = nullptr;
  uint16_t* (*_getFb)() = nullptr;
  void (*_dataCb)(const String&) = nullptr;
  void (*_setFrameCb)(int) = nullptr;

  // ── Static wrappers (no capturing lambdas — ESP32 compat) ──
  static void _wrap_bmp()   { if (g_hrns) g_hrns->_handleBmp(); }
  static void _wrap_btn()   { if (g_hrns) g_hrns->_handleBtn(); }
  static void _wrap_debug() { if (g_hrns) g_hrns->_handleDebug(); }
  static void _wrap_reset() { if (g_hrns) g_hrns->_handleReset(); }
  static void _wrap_data()  { if (g_hrns) g_hrns->_handleData(); }
  static void _wrap_frame() { if (g_hrns) g_hrns->_handleFrame(); }

  // ── GET /hrns/bmp ──────────────────────────────────────
  // Builds a BMP from the sprite framebuffer and sends it.
  // ~65KB heap during transmission, freed immediately after.
  void _handleBmp() {
    uint16_t* fb = _getFb ? _getFb() : nullptr;
    if (!fb) { _server->send(500, "text/plain", "ERR_NO_FB"); return; }

    int rowSize   = (HRNS_BMP_W * 2 + 3) & ~3;  // 4-byte row alignment
    int pixelSize = rowSize * HRNS_BMP_H;
    int fileSize  = 54 + pixelSize;

    // ── v0.0.17 修复：原来用 String 攒这 65KB ──
    //    本工程静态占用 81.8%（268KB / 327.7KB）→ 内部堆余量仅 ~59KB，
    //    String 增长失败会 **静默** 变短（历史"残缺陷流 5658/30070/35814 字节"很可能是堆不足，
    //    不是弱 WiFi）。改为显式缓冲：优先 PSRAM，其次内部堆；都失败就明确报错，不发半截图。
    char* buf = (char*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char*)heap_caps_malloc(fileSize, MALLOC_CAP_8BIT);
    if (!buf) { _server->send(500, "text/plain", "ERR_NO_HEAP"); return; }
    memset(buf, 0, fileSize);

    // ── BMP header (54 bytes) ──
    int w = HRNS_BMP_W;
    uint32_t h = HRNS_BMP_H;
    buf[0]='B'; buf[1]='M';
    memcpy(&buf[2], &fileSize, 4);
    buf[10]=54; buf[14]=40;
    memcpy(&buf[18], &w, 4);
    memcpy(&buf[22], &h, 4);
    buf[26]=1; buf[28]=16;
    memcpy(&buf[34], &pixelSize, 4);

    // ── Pixel data (bottom-to-top, RGB565) ──
    int o = 54;
    for (int y = HRNS_BMP_H - 1; y >= 0; y--) {
      for (int x = 0; HRNS_BMP_W > x; x++) {
        uint16_t px = fb[y * HRNS_BMP_W + x];
        px = (px >> 8) | (px << 8);   // fb 内存大端序 → 逻辑 RGB565（坑 8.41）
        buf[o++] = (char)( px        & 0xFF);
        buf[o++] = (char)((px >> 8)  & 0xFF);
      }
      o += rowSize - HRNS_BMP_W * 2;  // skip row padding
    }

    // 二进制安全发送：Content-Length + 原始字节（不走 String）
    _server->setContentLength(fileSize);
    _server->send(200, "image/bmp", "");
    _server->sendContent(buf, fileSize);
    free(buf);
  }

  // ── POST /hrns/btn ─────────────────────────────────────
  // Expects JSON body with "btn"(int) and "action"(string).
  // Reads request body as plain text (any Content-Type works).
  void _handleBtn() {
    // Read body directly (works with ANY Content-Type)
    if (!_server->hasArg("plain")) {
      _server->send(400, "text/plain", "ERR_NO_BODY");
      return;
    }
    String body = _server->arg("plain");
    if (body.length() == 0) {
      _server->send(400, "text/plain", "ERR_EMPTY");
      return;
    }

    // Parse JSON with zero dependencies (prefix search, whitespace tolerant)
    int btn = -1;
    char action[16] = "";

    auto findInt = [&](const char* key) -> int {
      String s = String("\"") + key + "\":";
      int pos = body.indexOf(s);
      if (pos < 0) return -1;
      const char* p = body.c_str() + pos + s.length();
      while (*p == ' ' || *p == '\t' || *p == '\"' || *p == '\n') p++;
      return atoi(p);
    };
    auto findStr = [&](const char* key, char* out, int maxLen) {
      // Try both "key":" and "key": " (with space)
      String s1 = String("\"") + key + "\":\"";
      String s2 = String("\"") + key + "\": \"";
      int pos = body.indexOf(s1);
      if (pos < 0) pos = body.indexOf(s2);
      if (pos < 0) return;
      const char* p = body.c_str() + pos + (s1.length() > body.length() - pos ? 0 : s1.length());
      // Handle case where s2 matched instead of s1
      if (*p == '\"') p++;
      // Past the opening quote, skip any spaces
      while (*p == ' ') p++;
      int i = 0;
      while (*p && *p != '\"' && i < maxLen - 1) out[i++] = *p++;
      out[i] = '\0';
    };

    btn = findInt("btn");
    findStr("action", action, sizeof(action));

    if (btn < 0 || btn > 1) {
      _server->send(400, "text/plain", "ERR_BTN_RANGE");
      return;
    }
    if (strcmp(action, "click") != 0 && strcmp(action, "long") != 0) {
      _server->send(400, "text/plain", "ERR_ACTION");
      return;
    }

    _btn[btn].pending = true;
    _server->send(200, "text/plain", "OK");
  }

  // ── GET /hrns/debug ────────────────────────────────────
  void _handleDebug() {
    String j = "{";
    j += "\"uptime\":"         + String(millis()) + ",";
    j += "\"free_heap\":"      + String(ESP.getFreeHeap()) + ",";
    j += "\"min_free_heap\":"  + String(ESP.getMinFreeHeap()) + ",";
    j += "\"psram_free\":"     + String(ESP.getFreePsram()) + ",";
    j += "\"btn_a_pending\":"  + String(_btn[0].pending ? "1" : "0") + ",";
    j += "\"btn_b_pending\":"  + String(_btn[1].pending ? "1" : "0") + ",";
    j += "\"w\":"             + String(HRNS_BMP_W) + ",";
    j += "\"h\":"             + String(HRNS_BMP_H);
    j += "}";
    _server->send(200, "application/json", j);
  }

  // ── POST /hrns/reset ───────────────────────────────────
  void _handleReset() {
    _server->send(200, "text/plain", "OK_RESETTING");
    delay(100);
    ESP.restart();
  }

  // ── POST /hrns/data ────────────────────────────────────
  void _handleData() {
    if (!_server->hasArg("plain")) {
      _server->send(400, "text/plain", "ERR_NO_BODY");
      return;
    }
    if (_dataCb) _dataCb(_server->arg("plain"));
    _server->send(200, "text/plain", "OK");
  }

  // ── POST /hrns/frame ──────────────────────────────────
  // Jumps to a specific frame index (avoids cycling through QR).
  // Body: {"frame": 0}
  void _handleFrame() {
    if (!_server->hasArg("plain")) {
      _server->send(400, "text/plain", "ERR_NO_BODY");
      return;
    }
    String body = _server->arg("plain");
    // Find "frame":N
    int pos = body.indexOf("\"frame\":");
    if (pos < 0) {
      _server->send(400, "text/plain", "ERR_NO_FRAME");
      return;
    }
    const char* p = body.c_str() + pos + 8;
    while (*p == ' ' || *p == '\t') p++;
    int f = atoi(p);
    if (f < 0 || f > 3) {
      _server->send(400, "text/plain", "ERR_FRAME_RANGE");
      return;
    }
    if (_setFrameCb) _setFrameCb(f);
    _server->send(200, "text/plain", "OK");
  }
};
