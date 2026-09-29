// ============================================================
//  prog_shapes.h — 剪影图元库 (no-signal v0.0.15)
//  节目池主体绘制用几何图元：三角/矩形/圆/穹顶/尖塔/方碑/柱阵/拱门/光锥/竖向渐变。
//  header-only，自包含（不依赖 main.cpp 的 rgb565/swap16/fillTri），命名带 ps 前缀。
//  设计原则：每个新主体 = 图元组合 + 参数（5~15 行/主体），统一剪影风格。
//  所有函数对屏幕边界自动裁剪（允许负坐标/越界）。
// ============================================================
#ifndef PROG_SHAPES_H
#define PROG_SHAPES_H

#include <stdint.h>
#include <math.h>

#ifndef PS_W
#define PS_W 240
#endif
#ifndef PS_H
#define PS_H 135
#endif

// ── 颜色工具 ──
static inline uint16_t psRgb565(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t psSwap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t psC(int r, int g, int b) { return psSwap16(psRgb565(r, g, b)); }

// 像素写入（带边界裁剪）
static inline void psPx(uint16_t* fb, int x, int y, uint16_t c) {
    if (x >= 0 && x < PS_W && y >= 0 && y < PS_H) fb[y * PS_W + x] = c;
}

// ── 竖向渐变（y0 顶 → y1 底，RGB 线性插值）──
static inline void psVgrad(uint16_t* fb, int y0, int y1,
                           int rT, int gT, int bT, int rB, int gB, int bB) {
    if (y1 <= y0) return;
    for (int y = y0; y < y1 && y < PS_H; y++) {
        if (y < 0) continue;
        int k = (y - y0) * 256 / (y1 - y0);
        uint16_t c = psC(rT + (rB - rT) * k / 256,
                         gT + (gB - gT) * k / 256,
                         bT + (bB - bT) * k / 256);
        uint16_t* row = &fb[y * PS_W];
        for (int x = 0; x < PS_W; x++) row[x] = c;
    }
}

// ── 实心矩形 ──
static inline void psRect(uint16_t* fb, int x0, int y0, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    for (int y = y0; y < y0 + h && y < PS_H; y++) {
        if (y < 0) continue;
        for (int x = x0; x < x0 + w && x < PS_W; x++)
            if (x >= 0) fb[y * PS_W + x] = c;
    }
}

// ── 实心三角形（扫描线：每行取三边交点的 min/max 区间）──
static inline void psTri(uint16_t* fb, int x0, int y0, int x1, int y1, int x2, int y2,
                         uint16_t c) {
    int ymin = y0, ymax = y0;
    if (y1 < ymin) ymin = y1; if (y2 < ymin) ymin = y2;
    if (y1 > ymax) ymax = y1; if (y2 > ymax) ymax = y2;
    for (int y = ymin; y <= ymax && y < PS_H; y++) {
        if (y < 0) continue;
        bool has = false;
        int lo = 0, hi = 0;
        // 三条边：y 在 [ya,yb] 闭区间内插值 x（水平边跳过）
        if (y0 != y1 && ((y0 <= y && y <= y1) || (y1 <= y && y <= y0))) {
            int ix = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            if (!has) { lo = hi = ix; has = true; }
            else { if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
        }
        if (y1 != y2 && ((y1 <= y && y <= y2) || (y2 <= y && y <= y1))) {
            int ix = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
            if (!has) { lo = hi = ix; has = true; }
            else { if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
        }
        if (y2 != y0 && ((y2 <= y && y <= y0) || (y0 <= y && y <= y2))) {
            int ix = x2 + (y - y2) * (x0 - x2) / (y0 - y2);
            if (!has) { lo = hi = ix; has = true; }
            else { if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
        }
        if (!has) continue;
        if (lo < 0) lo = 0;
        if (hi >= PS_W) hi = PS_W - 1;
        for (int x = lo; x <= hi; x++) fb[y * PS_W + x] = c;
    }
}

// ── 实心圆（扫描线，与项目现有 sqrtf 画法同风格）──
static inline void psCircle(uint16_t* fb, int cx, int cy, int r, uint16_t c) {
    for (int y = cy - r; y <= cy + r && y < PS_H; y++) {
        if (y < 0) continue;
        int dy = y - cy;
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        for (int x = cx - dx; x <= cx + dx; x++)
            if (x >= 0 && x < PS_W) fb[y * PS_W + x] = c;
    }
}

// ── 实心椭圆（扫描线；ry=0 安全）──
static inline void psEllipse(uint16_t* fb, int cx, int cy, int rx, int ry, uint16_t c) {
    for (int y = cy - ry; y <= cy + ry && y < PS_H; y++) {
        if (y < 0) continue;
        int dy = y - cy;
        int dx = rx;
        if (ry > 0) {
            float fy = (float)(dy * dy) / (float)(ry * ry);
            if (fy > 1.0f) fy = 1.0f;
            dx = (int)(sqrtf(1.0f - fy) * rx);
        }
        for (int x = cx - dx; x <= cx + dx; x++)
            if (x >= 0 && x < PS_W) fb[y * PS_W + x] = c;
    }
}

// ── 半圆穹顶（圆心 (cx, yBase)，上半圆）──
static inline void psDome(uint16_t* fb, int cx, int yBase, int r, uint16_t c) {
    for (int y = yBase - r; y < yBase && y < PS_H; y++) {
        if (y < 0) continue;
        int dy = y - yBase;
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        for (int x = cx - dx; x <= cx + dx; x++)
            if (x >= 0 && x < PS_W) fb[y * PS_W + x] = c;
    }
}

// ── 尖塔（等腰三角：顶点 (cx, yBase-h)，底边 (cx±w/2, yBase)）──
static inline void psSpire(uint16_t* fb, int cx, int yBase, int w, int h, uint16_t c) {
    psTri(fb, cx - w / 2, yBase, cx, yBase - h, cx + w / 2, yBase, c);
}

// ── 方碑（等腰梯形：底宽 wBot，顶宽 wTop）──
static inline void psObelisk(uint16_t* fb, int cx, int yBase, int wTop, int wBot, int h,
                             uint16_t c) {
    for (int y = yBase - h; y < yBase && y < PS_H; y++) {
        if (y < 0) continue;
        int k = (yBase - y);                       // 0..h 距底
        int w = wTop + (wBot - wTop) * k / h;      // 当前行宽
        for (int x = cx - w / 2; x <= cx + w / 2; x++)
            if (x >= 0 && x < PS_W) fb[y * PS_W + x] = c;
    }
}

// ── 柱阵（n 根等高矩形柱；变高请调用方多次 psRect）──
static inline void psPillar(uint16_t* fb, int x0, int yBase, int n, int w, int gap, int h,
                            uint16_t c) {
    for (int i = 0; i < n; i++)
        psRect(fb, x0 + i * (w + gap), yBase - h, w, h, c);
}

// ── 拱门（半圆穹顶 + 两条柱）──
static inline void psArch(uint16_t* fb, int cx, int yBase, int r, int wCol, int hCol,
                          uint16_t c) {
    psDome(fb, cx, yBase, r, c);
    psRect(fb, cx - r, yBase - hCol, wCol, hCol, c);
    psRect(fb, cx + r - wCol, yBase - hCol, wCol, hCol, c);
}

// ── 光锥/尾焰（上宽 wt@(x0,y0) → 下宽 wb@(x1,yBase) 的斜梯形）──
static inline void psBeam(uint16_t* fb, int x0, int y0, int wt, int x1, int yBase, int wb,
                          uint16_t c) {
    bool horiz = (yBase - y0) == 0;
    for (int yy = y0; yy <= yBase && yy < PS_H; yy++) {
        if (yy < 0) continue;
        int k = horiz ? 256 : (yy - y0) * 256 / (yBase - y0);
        int cx  = x0 + (x1 - x0) * k / 256;
        int w = wt + (wb - wt) * k / 256;
        for (int x = cx - w / 2; x <= cx + w / 2; x++)
            if (x >= 0 && x < PS_W) fb[yy * PS_W + x] = c;
    }
}

#endif // PROG_SHAPES_H