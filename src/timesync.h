// ────────────────────────────────────────────────────────
// timesync.h — no-signal 扫码对时模块
// AP(WIFI:T:nopass 格式 QR) + Captive Portal + BM8563 RTC
// 复用 firefly-clock wifi_ntp.cpp 的三层时间恢复链，裁剪到只剩对时
// ────────────────────────────────────────────────────────
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include "qrcode.h"

// 本机 AP SSID（≤9 字符才能用 QR 版本 2）
#define AP_SSID "NO-SIGNAL"

static bool g_timeValid = false;    // 系统时间是否可信
static bool g_syncing = false;      // 扫码对时进行中（防重入）
static uint32_t g_syncStartMs = 0;  // 对时界面进入时刻（30s 超时判定）

// ── NVS / RTC ─────────────────────────────────────────

// 从板载 BM8563 RTC 恢复系统时间（开机第一优先）
static bool rtcTryLoad() {
    // ★ M5.begin() 已按 board 初始化 RTC（M5Unified.cpp:3316 内部调用
    //   M5.Rtc.begin(&M5.In_I2C, M5.getBoard())）
    // ★ 绝不能再调无参 M5.Rtc.begin()：i2c=nullptr → PCF8563_Class::begin
    //   I2C 写寄存器 LoadProhibited 空指针崩溃 → Guru Meditation 重启循环
    //   （现象=启动画面闪烁/黑屏；pitfall 家族 8.17）
    if (!M5.Rtc.isEnabled()) return false;

    m5::rtc_date_t rd;
    bool gotDate = false;
    for (int i = 0; i < 3; i++) {
        if (M5.Rtc.getDate(&rd)) { gotDate = true; break; }
        delay(20);
    }
    if (!gotDate) return false;
    if (rd.year < 2025 || rd.year > 2099) return false;   // 首启/电池耗尽=脏值

    setenv("TZ", "UTC0", 1);
    tzset();

    m5::rtc_time_t rt;
    if (!M5.Rtc.getTime(&rt)) return false;

    struct tm tm = {0};
    tm.tm_year = rd.year - 1900;
    tm.tm_mon  = rd.month - 1;
    tm.tm_mday = rd.date;
    tm.tm_hour = rt.hours;
    tm.tm_min  = rt.minutes;
    tm.tm_sec  = rt.seconds;
    tm.tm_isdst = -1;
    time_t epoch = mktime(&tm);
    if (epoch < 1735689600) return false;    // < 2025-01-01

    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, NULL);
    g_timeValid = true;
    return true;
}

// 系统时间 → RTC + NVS 后备（对时成功后调用）
static void rtcSave() {
    time_t now = time(nullptr);
    if (now < 1735689600) return;

    struct tm ti;
    gmtime_r(&now, &ti);
    M5.Rtc.setDateTime(&ti);

    Preferences p;
    p.begin("nosig", false);
    p.putULong64("last_epoch", (uint64_t)now);
    p.end();
}

// NVS 后备恢复（RTC 电池耗尽时用，时间会偏慢但大致正确）
static bool nvsFallbackLoad() {
    Preferences p;
    p.begin("nosig", true);
    uint64_t lastEpoch = p.getULong64("last_epoch", 0);
    p.end();
    if (lastEpoch < 1735689600) return false;

    struct timeval tv = { (time_t)lastEpoch, 0 };
    settimeofday(&tv, NULL);
    g_timeValid = true;
    return true;
}

static int g_tzOffset = 8;   // 手机浏览器上报的时区（UTC+8 默认）
static bool g_syncedOnce = false;   // NVS "synced" 永久标志：对时成功=1

// 恢复时区偏移（对时后用 getLocalTimeT 显示本地时间）
static void loadTzPrefs() {
    Preferences p;
    p.begin("nosig", true);
    g_tzOffset = p.getInt("tz", 8);
    p.end();
    if (g_tzOffset < -12 || g_tzOffset > 14) g_tzOffset = 8;   // 脏值兜底
}

// 读「是否对过时」NVS 标志（区分首次/再次启动路径）
static bool syncGetFlag() {
    Preferences p;
    p.begin("nosig", true);
    g_syncedOnce = (p.getUChar("synced", 0) != 0);
    p.end();
    return g_syncedOnce;
}

// 写「是否对过时」标志（对时成功后调用；超时失败不写→下次开机仍自动对时）
static void syncSetFlag(bool v) {
    Preferences p;
    p.begin("nosig", false);
    p.putUChar("synced", v ? 1 : 0);
    p.end();
    g_syncedOnce = v;
}

// ── AP + QR 对时 ─────────────────────────────────────

static WebServer  g_apServer(80);
static DNSServer  g_dnsServer;

// 绘 QR（左码右步骤，黑底白字——CRT 风格）
static void drawQRScreen() {
    QRCode qr;
    uint8_t qrData[qrcode_getBufferSize(2)];
    const char* txt = "WIFI:T:nopass;S:" AP_SSID ";;";
    if (qrcode_initText(&qr, qrData, 2, ECC_LOW, txt) < 0) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setCursor(20, 60);
        M5.Display.print("QR gen failed");
        return;
    }

    int mod = 3, qrPx = qr.size * mod;      // 75px
    int qrX = 8, qrY = (135 - qrPx) / 2;
    M5.Display.fillScreen(TFT_BLACK);
    for (int y = 0; y < qr.size; y++)
        for (int x = 0; x < qr.size; x++)
            if (qrcode_getModule(&qr, x, y))
                M5.Display.fillRect(qrX + x*mod, qrY + y*mod, mod, mod, TFT_WHITE);

    // 右侧步骤说明（Font2 紧凑）
    uint16_t cHi  = M5.Display.color565(255, 170, 60);   // 琥珀标题
    uint16_t cNum = M5.Display.color565(200, 200, 200);
    uint16_t cAp  = M5.Display.color565(120, 120, 120);
    int tx = qrX + qrPx + 8;
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(cHi);
    M5.Display.setCursor(tx, 10);
    M5.Display.print("Time Sync");
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(cNum);
    M5.Display.setCursor(tx, 32);  M5.Display.print("1. Scan QR");
    M5.Display.setCursor(tx, 46);  M5.Display.print("2. Auto join");
    M5.Display.setCursor(tx, 60);  M5.Display.print("3. Tap Sync");
    M5.Display.setTextColor(cAp);
    M5.Display.setCursor(tx, 90);  M5.Display.print("AP:");
    M5.Display.setCursor(tx, 102); M5.Display.print(AP_SSID);
    // 底部提示
    M5.Display.setCursor(6, 124);
    M5.Display.print("A/B = skip");   // v0.0.17：与 loop 里实际实现的按键行为对齐
}

// ── HTML 页面（同步时间按钮）──
static void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NO-SIGNAL Sync</title><style>
body{background:#0a0a06;color:#e8c060;font-family:-apple-system,sans-serif;text-align:center;padding:24px 16px}
h1{font-size:20px;margin-bottom:6px} p{color:#a09060;font-size:13px;margin-bottom:18px}
.box{background:#141008;border:1px solid #3a3010;border-radius:10px;padding:18px;margin-bottom:14px}
button{background:#e8a830;color:#1a1200;border:none;padding:14px 32px;font-size:16px;border-radius:8px;cursor:pointer;font-weight:700}
button:active{background:#c89020}.ok{color:#8f8;font-size:15px;margin-top:14px}
#t{font-size:26px;color:#fff;margin:10px 0}
</style></head><body>
<h1>NO SIGNAL</h1><p>Tap to sync this clock's time to your phone.</p>
<div class="box"><div id="t">--:--</div>
<button onclick="sync()">Sync Now</button><div class="ok" id="st"></div></div>
<script>
var tz=-new Date().getTimezoneOffset();   // 手机上自动取当地时区
setInterval(function(){var d=new Date();
 document.getElementById('t').textContent=
  ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2);},1000);
function sync(){var b=document.getElementById('st');
 b.textContent='Sending...';
 fetch('/sync',{method:'POST',
  headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:'ep='+Math.floor(Date.now()/1000)+'&tz='+tz})
 .then(function(r){return r.text()}).then(function(m){b.textContent=m;b.style.color='#8f8'})
 .catch(function(e){b.textContent='X '+e;b.style.color='#f88'})}
</script></body></html>
)rawliteral";
    g_apServer.send(200, "text/html", html);
}

// POST /sync: 接收 epoch + tz → 写系统时间 + RTC + NVS
static void handleSyncPost() {
    if (!g_apServer.hasArg("ep")) {
        g_apServer.send(400, "text/plain", "missing ep");
        return;
    }
    time_t ep = (time_t)atoll(g_apServer.arg("ep").c_str());
    int tz = g_apServer.hasArg("tz") ? g_apServer.arg("tz").toInt() : 480;
    g_tzOffset = tz / 60;

    struct timeval tv = { ep, 0 };
    settimeofday(&tv, NULL);
    rtcSave();
    Preferences p;
    p.begin("nosig", false);
    p.putInt("tz", g_tzOffset);
    p.end();
    g_timeValid = true;
    g_apServer.send(200, "text/plain", "Synced! OK");
    // 无 delay：非阻塞——页面已收到响应，由状态机下一 tick 检测 g_timeValid 收尾
}

static void handleRedirect() {
    g_apServer.sendHeader("Location", "/", true);
    g_apServer.send(302, "text/plain", "");
}

// ── 主入口：非阻塞状态机三件套（由 loop 调用，绝不阻塞 setup）──
//  syncAPBegin()  → 进对时界面时调用一次（开 AP + QR + server）
//  syncAPTick()   → loop 每帧调用；返回 SYNC_ACTIVE=进行中 / SYNC_DONE=成功 / SYNC_TIMEOUT=30s 超时
//  syncAPEnd()    → 无论成功与否，离开对时界面时调用（关 AP 省电）
enum SyncRet { SYNC_ACTIVE, SYNC_DONE, SYNC_TIMEOUT };
static const uint32_t SYNC_TIMEOUT_MS = 60000;   // ⏱ 60s 无人扫码 → 自动回应用界面

static void syncAPBegin() {
    if (g_syncing) return;
    g_syncing = true;
    g_syncStartMs = millis();
    // ★ 关键：拉低 g_timeValid——否则 RTC/NVS 已有可信时间（上次对过时），
    //   syncAPTick() 首帧检测 g_timeValid==true 误判 SYNC_DONE，
    //   QR 只闪一帧就跳进雪花界面（用户实测：按 A 闪一下就进入雪花）
    //   进入对时后必须等手机 POST /sync 真正写入时间才算完成
    g_timeValid = false;

    IPAddress apIP(192, 168, 4, 1);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID);

    g_dnsServer.start(53, "*", apIP);
    g_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);

    g_apServer.on("/", handleRoot);
    g_apServer.on("/sync", HTTP_POST, handleSyncPost);
    g_apServer.onNotFound(handleRedirect);
    g_apServer.begin();

    drawQRScreen();
}

static SyncRet syncAPTick() {
    if (!g_syncing) return SYNC_TIMEOUT;
    g_dnsServer.processNextRequest();
    g_apServer.handleClient();
    M5.update();
    if (g_timeValid) return SYNC_DONE;                       // 手机点了 Sync
    if (millis() - g_syncStartMs >= SYNC_TIMEOUT_MS) return SYNC_TIMEOUT;   // 超时（60s：30s 不够扫码+开浏览器）
    return SYNC_ACTIVE;
}

static void syncAPEnd() {
    if (!g_syncing) return;
    g_syncing = false;
    g_apServer.stop();
    g_dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);        // 关 WiFi 省电（桌搭核心诉求）
}

// ── 取本地时间（含时区）──
static time_t getLocalTimeT() {
    if (!g_timeValid) return 0;
    return time(nullptr) + g_tzOffset * 3600L;
}