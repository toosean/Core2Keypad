// -----------------------------------------------------------------------------
//  Core2Keypad v2 —— M5Stack Core2 当蓝牙「手势面板」
//
//  两个模式（左侧抽屉里切换，落盘 NVS）：
//    SLIDE  上下滑 = 滚轮上/下（滑动量换算成刻度，每 24px 一格，单次上限 12）
//    MOUSE  拖动 = 移动光标；单击 = 左键；按住 500ms = 右键
//
//  左缘（x<16）向右滑 => 拉出抽屉：VIBRATION（振动开关）、MODE（模式切换）。
//  两块大图标，点亮 = 该状态生效。振动开着时：滚动/左键/右键各给一次短震，
//  状态条电量左侧常驻一个小图标。
//
//  HID 仍是复合设备（Report ID 1 键盘 + Report ID 2 鼠标），只是不再发键盘报告；
//  描述符不变，主机不需要重新配对。
//
//  串口命令（921600）：
//    C2_INFO / C2_BLE / C2_CCCD / C2_MODE <0|1> / C2_VIB <0|1> / C2_BUZZ
//    C2_SCREEN                    整屏截图（tools/c2_screenshot.py）
//    C2_T <0|1> <x> <y>           注入触摸（端到端测试）；C2_T 9 交还真实触摸
// -----------------------------------------------------------------------------
#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <Preferences.h>

// ---------------------------------------------------------------- 屏幕几何
static constexpr int W = 320;
static constexpr int H = 240;

static constexpr int BAR_H   = 30;
static constexpr int PANEL_X = 8, PANEL_Y = 40, PANEL_W = 304, PANEL_H = 192;

static constexpr int DRAWER_W = 232, DRAWER_H = 184, DRAWER_Y = 44;
static constexpr int TILE_W = 96, TILE_H = 106, TILE_Y = 82;
static constexpr int TILE_A_X = 16, TILE_B_X = 124;

// ---------------------------------------------------------------- 手势参数
static constexpr int  EDGE_X      = 16;    // 左缘热区
static constexpr int  EDGE_SWIPE  = 60;    // 右滑多远算拉出抽屉
static constexpr int  MOVE_SLOP   = 10;    // 超过就不算点击
static constexpr int  NOTCH_PX    = 24;    // 每 24px 一格滚轮
static constexpr int  MAX_NOTCHES = 12;
static constexpr uint32_t TAP_MS  = 220;
static constexpr uint32_t HOLD_MS = 500;
static constexpr uint16_t BUZZ_MS = 40;
static constexpr uint32_t RIPPLE_MS = 260;
static constexpr uint32_t SCROLL_HOLD_MS = 500;
static constexpr int  MOUSE_DIV   = 10;    // 定点：1px = 12/10 个鼠标计数

// ---------------------------------------------------------------- 颜色
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static constexpr uint16_t C_BG      = rgb(0x0A, 0x0F, 0x15);
static constexpr uint16_t C_SURF    = rgb(0x0F, 0x19, 0x22);
static constexpr uint16_t C_LINE    = rgb(0x1B, 0x2C, 0x3A);
static constexpr uint16_t C_TEXT    = rgb(0xE8, 0xF2, 0xFF);
static constexpr uint16_t C_DIM     = rgb(0x4A, 0x63, 0x79);
static constexpr uint16_t C_HEAD    = rgb(0x54, 0x67, 0x7A);
static constexpr uint16_t C_FAINT   = rgb(0x3A, 0x51, 0x64);
static constexpr uint16_t C_MUTED   = rgb(0x7C, 0x93, 0xA6);
static constexpr uint16_t C_BLUE    = rgb(0x38, 0xBD, 0xF8);
static constexpr uint16_t C_BLUE_D  = rgb(0x10, 0x2A, 0x38);
static constexpr uint16_t C_BLUE_L  = rgb(0x7D, 0xD3, 0xFC);
static constexpr uint16_t C_AMBER   = rgb(0xFB, 0xBF, 0x24);
static constexpr uint16_t C_AMBER_D = rgb(0x2A, 0x21, 0x10);
static constexpr uint16_t C_AMBER_L = rgb(0xFC, 0xD3, 0x4D);
static constexpr uint16_t C_GREEN   = rgb(0x34, 0xD3, 0x99);
static constexpr uint16_t C_GREEN_T = rgb(0x5F, 0xCF, 0xA6);
static constexpr uint16_t C_CARD    = rgb(0x0F, 0x1A, 0x24);
static constexpr uint16_t C_TILE    = rgb(0x0C, 0x15, 0x20);
static constexpr uint16_t C_TILE_M  = rgb(0x0D, 0x18, 0x22);
static constexpr uint16_t C_CHEV_U  = rgb(0x31, 0x50, 0x6A);
static constexpr uint16_t C_CHEV_D  = rgb(0x6E, 0x58, 0x28);
static constexpr uint16_t C_DOT     = rgb(0x1C, 0x2D, 0x3A);
static constexpr uint16_t C_OFF_IC  = rgb(0x3B, 0x52, 0x65);

static inline uint16_t dimc(uint16_t c) { return (uint16_t)((c >> 1) & 0x7BEF); }

static uint16_t blendc(uint16_t a, uint16_t b, uint8_t t)   // 0 -> a, 255 -> b
{
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = ar + (br - ar) * t / 255;
    const int g = ag + (bg - ag) * t / 255;
    const int bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ---------------------------------------------------------------- 全局状态
enum : uint8_t { MODE_SLIDE = 0, MODE_MOUSE = 1, MODE_COUNT = 2 };
static const char *kModeName[MODE_COUNT] = { "SLIDE", "MOUSE" };

static uint8_t  g_mode      = MODE_SLIDE;
static bool     g_vib       = true;
static bool     g_drawer    = false;
static bool     g_connected = false;
static bool     g_advPending = false;   // 等待（重）开广播
static uint32_t g_advAt = 0;
static uint32_t g_advRetryAt = 0;
static uint8_t  g_battPct   = 0;
static uint16_t g_boots     = 0;   // 开机次数（NVS），用来抓意外重启
static Preferences g_prefs;

// 重绘调度
static bool     g_needRender = true;
static uint32_t g_renderAt   = 0;
static uint32_t g_lastRender = 0;

static void requestRedraw(uint16_t delayMs = 0)
{
    g_needRender = true;
    g_renderAt = millis() + delayMs;
}

static void setMode(uint8_t m)
{
    if (m >= MODE_COUNT || m == g_mode) return;
    g_mode = m;
    g_prefs.putUChar("mode", m);
    Serial.printf("[MODE] -> %s\n", kModeName[g_mode]);
    requestRedraw();
}

static void openDrawer()
{
    g_drawer = true;
    Serial.println("[UI] drawer open");
    requestRedraw();
}

static void closeDrawer()
{
    g_drawer = false;
    Serial.println("[UI] drawer close");
    requestRedraw();
}

static bool  g_vibMotor = false;
static uint32_t g_vibUntil = 0;

static void buzz(uint16_t ms)
{
    if (!g_vib || !g_connected || g_vibMotor) return;
    if (!M5.Power.setVibration(1)) return;
    g_vibMotor = true;
    g_vibUntil = millis() + ms;
}

static void toggleVib()
{
    g_vib = !g_vib;
    g_prefs.putUChar("vib", g_vib ? 1 : 0);
    if (!g_vib && g_vibMotor) {
        M5.Power.setVibration(0);
        g_vibMotor = false;
    }
    Serial.printf("[VIB] -> %d\n", g_vib ? 1 : 0);
    if (g_vib) buzz(BUZZ_MS);
    requestRedraw();
}

// ---------------------------------------------------------------- BLE
static BLEServer         *g_server = nullptr;
static BLEHIDDevice      *g_hid    = nullptr;
static BLECharacteristic *g_kb     = nullptr;
static BLECharacteristic *g_ms     = nullptr;

class SecCB : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 123456; }
    void     onPassKeyNotify(uint32_t) override {}
    bool     onSecurityRequest() override { return true; }
    bool     onConfirmPIN(uint32_t) override { return true; }
    void     onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        Serial.printf("[SEC] auth %s bonded=%d\n",
                      cmpl.success ? "OK" : "FAIL", cmpl.success ? 1 : 0);
    }
};

// HOGP 输入报告只有被订阅（CCCD 0x2902 写 0x0001）才会发出。Arduino BLE 的
// notify() 会先看本地 0x2902，为 0 就静默丢弃。把主机写入的状态落盘、重连时
// 回填，否则 reset 之后主机显示已连接却什么都收不到。
class CccdCB : public BLEDescriptorCallbacks {
    const char *m_tag;
    const char *m_key;
public:
    CccdCB(const char *tag, const char *key) : m_tag(tag), m_key(key) {}
    void onWrite(BLEDescriptor *d) override {
        const uint8_t *v = d->getValue();
        const bool on = (v[0] & 0x01) != 0;
        g_prefs.putChar(m_key, on ? 1 : 0);
        Serial.printf("[CCCD] %s HOST WRITE handle=%u value=%02X%02X -> nvs %s=%d\n",
                      m_tag, d->getHandle(), v[0], v[1], m_key, on ? 1 : 0);
    }
};

class HidCB : public BLECharacteristicCallbacks {
    const char *m_tag;
public:
    explicit HidCB(const char *tag) : m_tag(tag) {}
    void onStatus(BLECharacteristic *, Status s, uint32_t code) override {
        const char *name = "other";
        switch (s) {
        case SUCCESS_NOTIFY:          return;
        case ERROR_NOTIFY_DISABLED:   name = "DROP cccd-not-subscribed"; break;
        case ERROR_INDICATE_DISABLED: name = "DROP cccd-not-subscribed(ind)"; break;
        case ERROR_NO_CLIENT:         name = "DROP no-client"; break;
        case ERROR_GATT:              name = "DROP gatt-error"; break;
        default: break;
        }
        Serial.printf("[HID] %s %s code=%u\n", m_tag, name, (unsigned)code);
    }
};

static void cccdRestore();

class ServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        g_connected = true;
        g_advPending = false;
        Serial.println("[BLE] connected");
        cccdRestore();
        requestRedraw();
    }
    void onDisconnect(BLEServer *srv) override {
        (void)srv;
        g_connected = false;
        Serial.println("[BLE] disconnected");
        // 不要在回调里立刻开广播：协议栈还没拆完链路，startAdvertising 经常静默失败，
        // 结果就是「主机再也连不上，直到复位」。交回 loop 延迟重开，并带重试。
        g_advPending = true;
        g_advAt = millis() + 250;
        g_advRetryAt = millis() + 10000;
        requestRedraw();
    }
    // 断开原因：0x08 超时 / 0x13 0x16 主机主动断 / 0x3E 建链失败
    void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
        Serial.printf("[BLE] reason=0x%02X\n", param->disconnect.reason);
    }
};

static BLEDescriptor *cccdOf(BLECharacteristic *c)
{
    return c ? c->getDescriptorByUUID(BLEUUID((uint16_t)0x2902)) : nullptr;
}

static bool cccdOn(BLECharacteristic *c)
{
    BLEDescriptor *d = cccdOf(c);
    return d && d->getLength() > 0 && (d->getValue()[0] & 0x01);
}

static void cccdSet(BLECharacteristic *c, bool on)
{
    BLEDescriptor *d = cccdOf(c);
    if (!d || cccdOn(c) == on) return;
    uint8_t v[2] = { (uint8_t)(on ? 0x01 : 0x00), 0x00 };
    d->setValue(v, 2);
    Serial.printf("[CCCD] set notify=%d handle=%u\n", on ? 1 : 0, d->getHandle());
}

static void cccdRestore()
{
    const bool bonded = esp_ble_get_bond_device_num() > 0;
    const int8_t sk = (int8_t)g_prefs.getChar("cck_kb", (int8_t)-1);
    const int8_t sm = (int8_t)g_prefs.getChar("cck_ms", (int8_t)-1);
    cccdSet(g_kb, (sk < 0) ? bonded : (sk != 0));
    cccdSet(g_ms, (sm < 0) ? bonded : (sm != 0));
    Serial.printf("[CCCD] restore bonded=%d saved kb=%d ms=%d -> live kb=%d ms=%d\n",
                  bonded ? 1 : 0, sk, sm, cccdOn(g_kb) ? 1 : 0, cccdOn(g_ms) ? 1 : 0);
}

// 键盘(Report ID 1) + 鼠标(Report ID 2)。v2 不再发键盘报告，但描述符保持原样：
// 换描述符主机会缓存旧值，必须重新配对，代价太大。
static const uint8_t kReportMap[] = {
    // ---- 键盘 ----
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    // ---- 鼠标（三键 + 滚轮）----
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0
};

static void bleInit()
{
    BLEDevice::init("Core2 Keypad");
    g_server = BLEDevice::createServer();
    g_server->setCallbacks(new ServerCB());
    BLEDevice::setSecurityCallbacks(new SecCB());

    g_hid = new BLEHIDDevice(g_server);
    g_kb  = g_hid->inputReport(1);
    g_ms  = g_hid->inputReport(2);

    BLESecurity *sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    g_hid->manufacturer()->setValue("M5Stack");
    g_hid->pnp(0x02, 0x05AC, 0x024F, 0x0100);
    g_hid->hidInfo(0x00, 0x01);
    g_hid->reportMap((uint8_t *)kReportMap, sizeof(kReportMap));
    g_hid->startServices();

    static CccdCB cbKb("kb", "cck_kb"), cbMs("ms", "cck_ms");
    static HidCB  hbKb("kb"), hbMs("ms");
    BLEDescriptor *dKb = g_kb->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    BLEDescriptor *dMs = g_ms->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    if (dKb) dKb->setCallbacks(&cbKb);
    if (dMs) dMs->setCallbacks(&cbMs);
    g_kb->setCallbacks(&hbKb);
    g_ms->setCallbacks(&hbMs);

    BLEAdvertising *adv = g_server->getAdvertising();
    adv->setAppearance(0x03C1);
    adv->addServiceUUID(g_hid->hidService()->getUUID());
    adv->setScanResponse(true);
    adv->start();
    Serial.println("[BLE] advertising");
}

// ---------------------------------------------------------------- HID 发送
// 报告值不带 Report ID 前缀（主机栈把特征值整体当报告本体）。
static void sendMouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!g_connected || !g_ms) return;
    uint8_t buf[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
    g_ms->setValue(buf, 4);
    g_ms->notify();
}



// ---------------------------------------------------------------- 手势状态
static bool     g_pressed = false;
static int16_t  g_x0 = 0, g_y0 = 0;
static int16_t  g_px = 0, g_py = 0;
static uint32_t g_t0 = 0;
static bool     g_edge = false;
static bool     g_moved = false;
static bool     g_rightFired = false;
static int16_t  g_accum = 0;
static int8_t   g_scrollDir = 0;
static int      g_notches = 0;
static uint32_t g_scrollUntil = 0;
static int32_t  g_accX = 0, g_accY = 0;

static void mouseMove(int dx, int dy)
{
    g_accX += dx * 12;          // 1px ≈ 1.2 计数，1/10 定点
    g_accY += dy * 12;
    int sx = g_accX / MOUSE_DIV;
    int sy = g_accY / MOUSE_DIV;
    if (!sx && !sy) return;
    g_accX -= sx * MOUSE_DIV;
    g_accY -= sy * MOUSE_DIV;
    if (sx > 127) sx = 127; else if (sx < -127) sx = -127;
    if (sy > 127) sy = 127; else if (sy < -127) sy = -127;
    sendMouse(0, (int8_t)sx, (int8_t)sy, 0);
}

static uint32_t g_rippleAt = 0;
static int16_t  g_rippleX = 0, g_rippleY = 0;
static bool     g_rippleRight = false;

static bool     g_inject = false;
static bool     g_injPressed = false;
static int16_t  g_injX = 0, g_injY = 0;
static uint32_t g_injUntil = 0;

static void mouseClick(bool right, int x, int y)
{
    sendMouse(right ? 0x02 : 0x01, 0, 0, 0);
    delay(20);                              // 按下停留一下，主机才不会当毛刺
    sendMouse(0, 0, 0, 0);
    g_rippleAt = millis();
    g_rippleX = (int16_t)x;
    g_rippleY = (int16_t)y;
    g_rippleRight = right;
    buzz(BUZZ_MS);
    Serial.printf("[HID] %s click at %d,%d\n", right ? "right" : "left", x, y);
    requestRedraw();
}

static void onPress(int x, int y, uint32_t now)
{
    g_pressed = true;
    g_x0 = (int16_t)x; g_y0 = (int16_t)y;
    g_px = (int16_t)x; g_py = (int16_t)y;
    g_t0 = now;
    g_edge = (x < EDGE_X);
    g_moved = false;
    g_rightFired = false;
    g_notches = 0;
    g_accum = 0;
    g_accX = g_accY = 0;
    g_scrollDir = 0;
    if (g_drawer) requestRedraw();
}

static void onRelease(int x, int y, uint32_t now)
{
    const int tx = x - g_x0;
    const int ty = y - g_y0;
    const uint32_t dur = now - g_t0;

    if (g_drawer) {
        if (x >= DRAWER_W || y < DRAWER_Y || y > DRAWER_Y + DRAWER_H) { closeDrawer(); return; }
        if (x >= TILE_A_X && x < TILE_A_X + TILE_W && y >= TILE_Y && y < TILE_Y + TILE_H) {
            toggleVib();
            return;
        }
        if (x >= TILE_B_X && x < TILE_B_X + TILE_W && y >= TILE_Y && y < TILE_Y + TILE_H) {
            setMode(g_mode == MODE_SLIDE ? MODE_MOUSE : MODE_SLIDE);
            return;
        }
        return;
    }

    if (g_edge && tx >= EDGE_SWIPE && ty > -60 && ty < 60) { openDrawer(); return; }

    if (g_mode == MODE_MOUSE) {
        if (!g_moved && !g_rightFired && dur < TAP_MS) mouseClick(false, g_px, g_py);
        return;
    }

    if (g_notches) g_scrollUntil = now + SCROLL_HOLD_MS;
    requestRedraw();
}

static void feedTouch(bool pressed, int x, int y)
{
    const uint32_t now = millis();

    if (pressed && !g_pressed) { onPress(x, y, now); return; }
    if (!pressed) {
        if (!g_pressed) return;
        g_pressed = false;
        onRelease(x, y, now);
        return;
    }

    const int dx = x - g_px;
    const int dy = y - g_py;
    g_px = (int16_t)x; g_py = (int16_t)y;
    const int tx = x - g_x0;
    const int ty = y - g_y0;

    if (!g_moved && (abs(tx) + abs(ty) > MOVE_SLOP)) g_moved = true;

    if (g_drawer || g_edge) return;        // 这两类只在抬手时判定
    if (g_mode == MODE_MOUSE) {
        if (g_rightFired || !g_moved) return;
        mouseMove(dx, dy);
        return;
    }

    if (!g_moved) return;
    if (abs(ty) <= abs(tx)) return;        // 以纵向为主才算滚动

    g_accum += dy;
    while (g_notches < MAX_NOTCHES) {
        if (g_accum >= NOTCH_PX) {
            g_accum -= NOTCH_PX;
            g_notches++;
            g_scrollDir = -1;
            sendMouse(0, 0, 0, -1);
        } else if (g_accum <= -NOTCH_PX) {
            g_accum += NOTCH_PX;
            g_notches++;
            g_scrollDir = +1;
            sendMouse(0, 0, 0, +1);
        } else {
            break;
        }
        if (g_notches == 1) buzz(BUZZ_MS);
        g_scrollUntil = now + SCROLL_HOLD_MS;
        requestRedraw(40);
    }
    if (g_notches >= MAX_NOTCHES) g_accum = 0;
}

static bool holdCandidate(uint32_t now)
{
    return g_pressed && !g_drawer && !g_edge && g_mode == MODE_MOUSE &&
           !g_moved && !g_rightFired && (now - g_t0) < HOLD_MS;
}

static void pollHold(uint32_t now)
{
    if (!g_pressed || g_drawer || g_edge) return;
    if (g_mode != MODE_MOUSE || g_moved || g_rightFired) return;
    if (now - g_t0 < HOLD_MS) return;
    g_rightFired = true;
    mouseClick(true, g_px, g_py);
}

// ---------------------------------------------------------------- 绘制
static M5Canvas canvas(&M5.Display);
static bool     g_canvas = false;

static void drawVibGlyph(int cx, int cy, uint16_t c)
{
    canvas.fillRoundRect(cx - 2, cy - 10, 5, 20, 2, c);
    canvas.drawArc(cx, cy, 9, 11, -52, 52, c);
    canvas.drawArc(cx, cy, 9, 11, 128, 232, c);
}

static void drawStatusBar(bool dim, uint32_t now)
{
    (void)now;
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(dim ? dimc(C_HEAD) : C_HEAD);
    canvas.drawString("CORE2 PANEL", 14, 11);

    const bool mouse = (g_mode == MODE_MOUSE);
    const uint16_t acc  = mouse ? C_AMBER : C_BLUE;
    const uint16_t accd = mouse ? C_AMBER_D : C_BLUE_D;
    canvas.fillRoundRect(100, 7, 54, 17, 8, dim ? dimc(accd) : accd);
    canvas.drawRoundRect(100, 7, 54, 17, 8, dim ? dimc(acc) : acc);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(dim ? dimc(acc) : acc);
    canvas.drawString(kModeName[g_mode], 127, 15);

    canvas.fillCircle(168, 15, 3, g_connected ? (dim ? dimc(C_GREEN) : C_GREEN) : C_DIM);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(g_connected ? (dim ? dimc(C_GREEN_T) : C_GREEN_T) : C_DIM);
    canvas.drawString(g_connected ? "CONNECTED" : "WAITING", 176, 11);

    if (g_vib) drawVibGlyph(264, 15, dim ? dimc(C_BLUE_L) : C_BLUE_L);

    canvas.setTextDatum(textdatum_t::top_right);
    canvas.setTextColor(dim ? dimc(C_DIM) : C_DIM);
    char batt[8];
    snprintf(batt, sizeof(batt), "%u%%", (unsigned)g_battPct);
    canvas.drawString(batt, 306, 11);
}

static void drawPanelBase(bool dim)
{
    canvas.fillRoundRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 18, dim ? dimc(C_SURF) : C_SURF);
    canvas.drawRoundRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 18, dim ? dimc(C_LINE) : C_LINE);
}

static void drawPullTab(bool dim)
{
    const uint16_t a = dim ? dimc(rgb(0x15, 0x22, 0x2D)) : rgb(0x15, 0x22, 0x2D);
    const uint16_t b = dim ? dimc(rgb(0x2F, 0x4B, 0x60)) : rgb(0x2F, 0x4B, 0x60);
    canvas.fillRoundRect(0, 102, 6, 36, 3, a);
    canvas.fillRoundRect(2, 110, 2, 20, 1, b);
    canvas.drawWideLine(10, 114, 14, 120, 0.7f, b);
    canvas.drawWideLine(14, 120, 10, 126, 0.7f, b);
}

static void drawSlideIdle(bool dim)
{
    const uint16_t up = dim ? dimc(C_CHEV_U) : C_CHEV_U;
    const uint16_t dn = dim ? dimc(C_CHEV_D) : C_CHEV_D;
    canvas.drawWideLine(146, 100, 160, 86, 1.3f, up);
    canvas.drawWideLine(160, 86, 174, 100, 1.3f, up);
    canvas.drawWideLine(146, 186, 160, 200, 1.3f, dn);
    canvas.drawWideLine(160, 200, 174, 186, 1.3f, dn);

    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(dim ? dimc(C_MUTED) : C_MUTED);
    canvas.drawString("SWIPE UP / DOWN", 160, 133);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(dim ? dimc(C_FAINT) : C_FAINT);
    canvas.drawString("SCROLL", 160, 152);
}

static void drawMouseIdle(bool dim)
{
    const uint16_t dot = dim ? dimc(C_DOT) : C_DOT;
    for (int y = 52; y <= 220; y += 16)
        for (int x = 20; x <= 300; x += 16)
            canvas.drawPixel(x, y, dot);

    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(dim ? dimc(C_FAINT) : C_FAINT);
    canvas.drawString("DRAG MOVE | TAP LEFT | HOLD RIGHT", 160, 219);
}

static void drawScrollOverlay()
{
    const bool down = (g_scrollDir < 0);
    const uint16_t acc = down ? C_AMBER : C_BLUE;

    if (down) {
        canvas.drawWideLine(128, 104, 160, 136, 4.5f, acc);
        canvas.drawWideLine(160, 136, 192, 104, 4.5f, acc);
    } else {
        canvas.drawWideLine(128, 136, 160, 104, 4.5f, acc);
        canvas.drawWideLine(160, 104, 192, 136, 4.5f, acc);
    }

    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(down ? "SCROLL DOWN" : "SCROLL UP", 160, 162);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(blendc(C_SURF, acc, 170));
    char buf[20];
    snprintf(buf, sizeof(buf), "%d NOTCHES", g_notches);
    canvas.drawString(buf, 160, 184);

    int prog = (int)((g_accum < 0 ? -g_accum : g_accum) * 100 / NOTCH_PX);
    if (prog > 100) prog = 100;
    canvas.fillRoundRect(110, 194, 100, 4, 2, C_LINE);
    if (prog > 2) canvas.fillRoundRect(110, 194, prog, 4, 2, acc);
}

static void drawHoldRing(uint32_t now)
{
    float p = (float)(now - g_t0) / (float)HOLD_MS;
    if (p > 1.0f) p = 1.0f;
    canvas.drawArc(g_x0, g_y0, 22, 26, 0, 360, C_LINE);
    if (p > 0.01f) canvas.drawArc(g_x0, g_y0, 22, 26, -90, -90 + 360 * p, C_AMBER);
}

static void drawRipple(uint32_t now)
{
    const int age = (int)(now - g_rippleAt);
    if (age < 0 || age > (int)RIPPLE_MS) return;
    const uint16_t acc = g_rippleRight ? C_AMBER : C_BLUE;
    const uint8_t fade = (uint8_t)(255 - age * 255 / (int)RIPPLE_MS);
    const int r = 8 + age * 20 / (int)RIPPLE_MS;
    canvas.drawCircle(g_rippleX, g_rippleY, r, blendc(C_SURF, acc, fade));
    canvas.drawCircle(g_rippleX, g_rippleY, r * 2 / 3, blendc(C_SURF, acc, (uint8_t)(fade * 3 / 4)));
}

static void drawDrawer()
{
    canvas.fillRoundRect(-18, DRAWER_Y, DRAWER_W + 18, DRAWER_H, 18, C_CARD);
    canvas.drawRoundRect(-18, DRAWER_Y, DRAWER_W + 18, DRAWER_H, 18, C_LINE);

    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(C_HEAD);
    canvas.drawString("TOOLS", 20, 62);

    // 振动块
    const int acx = TILE_A_X + TILE_W / 2;
    canvas.fillRoundRect(TILE_A_X, TILE_Y, TILE_W, TILE_H, 18, g_vib ? C_BLUE_D : C_TILE);
    canvas.drawRoundRect(TILE_A_X, TILE_Y, TILE_W, TILE_H, 18, g_vib ? C_BLUE : C_LINE);
    drawVibGlyph(acx, TILE_Y + 32, g_vib ? C_BLUE_L : C_OFF_IC);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(g_vib ? C_TEXT : rgb(0x6E, 0x86, 0x99));
    canvas.drawString("VIBRATION", acx, TILE_Y + 68);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(g_vib ? C_BLUE_L : rgb(0x3E, 0x55, 0x68));
    canvas.drawString(g_vib ? "ON" : "OFF", acx, TILE_Y + 88);

    // 模式块
    const bool mouse = (g_mode == MODE_MOUSE);
    const uint16_t macc = mouse ? C_AMBER : C_BLUE;
    const int bcx = TILE_B_X + TILE_W / 2;
    const int bcy = TILE_Y + 32;
    canvas.fillRoundRect(TILE_B_X, TILE_Y, TILE_W, TILE_H, 18, mouse ? C_AMBER_D : C_TILE_M);
    canvas.drawRoundRect(TILE_B_X, TILE_Y, TILE_W, TILE_H, 18, mouse ? C_AMBER : C_LINE);
    if (mouse) {
        canvas.drawRoundRect(bcx - 15, bcy - 11, 30, 22, 7, macc);
        canvas.fillCircle(bcx, bcy, 3, macc);
    } else {
        canvas.drawWideLine(bcx - 8, bcy - 4, bcx, bcy - 12, 1.2f, macc);
        canvas.drawWideLine(bcx, bcy - 12, bcx + 8, bcy - 4, 1.2f, macc);
        canvas.drawWideLine(bcx - 8, bcy + 4, bcx, bcy + 12, 1.2f, macc);
        canvas.drawWideLine(bcx, bcy + 12, bcx + 8, bcy + 4, 1.2f, macc);
    }
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(mouse ? C_TEXT : rgb(0xC9, 0xD8, 0xE6));
    canvas.drawString("MODE", bcx, TILE_Y + 68);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(mouse ? C_AMBER_L : C_BLUE_L);
    canvas.drawString(kModeName[g_mode], bcx, TILE_Y + 88);

    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(rgb(0x33, 0x47, 0x5A));
    canvas.drawString("TAP A TILE TO TOGGLE", 20, 206);
}

static void render(uint32_t now)
{
    if (!g_canvas) return;
    const bool dim = g_drawer;

    canvas.fillScreen(dim ? dimc(C_BG) : C_BG);
    drawStatusBar(dim, now);
    drawPanelBase(dim);
    if (!dim) drawPullTab(false);

    const bool scrolling = g_notches && (int32_t)(now - g_scrollUntil) < 0;
    if (g_mode == MODE_SLIDE) { if (!scrolling) drawSlideIdle(dim); }
    else                      drawMouseIdle(dim);

    if (!g_connected) {
        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(textdatum_t::middle_center);
        canvas.setTextColor(rgb(0x5A, 0x46, 0x1C));
        canvas.drawString("NOT CONNECTED", 160, 210);
    }

    if (!dim) {
        if (scrolling) drawScrollOverlay();
        if (g_rippleAt && (now - g_rippleAt) < RIPPLE_MS) drawRipple(now);
        if (holdCandidate(now)) drawHoldRing(now);
    }

    if (g_drawer) drawDrawer();

    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- 亮度
static constexpr uint8_t  kBrightMax = 255;
static constexpr uint8_t  kBrightDim = 51;
static constexpr uint32_t kHoldMs    = 3000;
static constexpr uint32_t kFadeMs    = 1000;

static uint32_t g_lastInputMs = 0;
static uint8_t  g_brightness  = kBrightMax;

static void applyBrightness(uint8_t v)
{
    if (v == g_brightness) return;
    g_brightness = v;
    M5.Display.setBrightness(v);
}

static void updateBrightness(uint32_t now)
{
    const uint32_t idle = now - g_lastInputMs;
    if (idle <= kHoldMs) { applyBrightness(kBrightMax); return; }
    const uint32_t e = idle - kHoldMs;
    if (e >= kFadeMs) { applyBrightness(kBrightDim); return; }
    const uint32_t span = (uint32_t)(kBrightMax - kBrightDim);
    applyBrightness((uint8_t)(kBrightMax - span * e / kFadeMs));
}

// ---------------------------------------------------------------- 截图（非阻塞）
// 一次 C2_SCREEN 要发 230KB；如果整段阻塞在 loop 里，BLE 链路会被拖到超时
// （实测 reason=0x08）。所以拆成小批：每次只写「串口缓冲还放得下」的量，
// 发完一行再回来服务触摸/BLE。期间冻结重绘，保证画面一致。
static bool     g_capActive = false;
static int      g_capY = 0;
static int      g_capOff = 0;
static uint32_t g_capStart = 0;
static uint8_t  g_capRow[W * 3];

static void captureBegin()
{
    const uint16_t *buf = g_canvas ? static_cast<const uint16_t *>(canvas.getBuffer()) : nullptr;
    if (!buf) { Serial.println("C2_ERROR no_framebuffer"); return; }
    Serial.printf("C2_SCREEN %d %d %u\n", W, H, (unsigned)(W * 3 * H));
    g_capActive = true;
    g_capY = 0;
    g_capOff = 0;
    g_capStart = millis();
}

static void capturePump()
{
    if (!g_capActive) return;
    if (millis() - g_capStart > 30000) {
        g_capActive = false;
        Serial.println("C2_ERROR transfer_timeout");
        return;
    }
    const uint16_t *buf = static_cast<const uint16_t *>(canvas.getBuffer());
    while (g_capY < H) {
        const int space = Serial.availableForWrite();
        if (space < 128) return;                       // 等主机读走
        if (g_capOff == 0) {
            const uint16_t *src = buf + (size_t)g_capY * W;
            for (int x = 0; x < W; ++x) {
                const uint16_t c = __builtin_bswap16(src[x]);   // sprite 是 swap565
                g_capRow[x * 3 + 0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
                g_capRow[x * 3 + 1] = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
                g_capRow[x * 3 + 2] = (uint8_t)(( c        & 0x1F) * 255 / 31);
            }
        }
        const int remain = W * 3 - g_capOff;
        int n = space - 64;
        if (n > remain) n = remain;
        if (n <= 0) return;
        Serial.write(g_capRow + g_capOff, n);
        g_capOff += n;
        if (g_capOff >= W * 3) {
            g_capOff = 0;
            g_capY++;
            if ((g_capY & 15) == 0) {        // 每 16 行喘口气
                Serial.flush();
                delay(30);
            }
        }
    }
    Serial.flush();
    g_capActive = false;
}
// ---------------------------------------------------------------- 串口
static void handleSerial()
{
    static String command;
    static uint32_t lastByte = 0;

    while (Serial.available()) {
        const char ch = (char)Serial.read();
        lastByte = millis();
        if (ch == '\n') {
            command.trim();
            if (command == "C2_SCREEN") {
                captureBegin();
            } else if (command == "C2_INFO") {
                Serial.printf("C2_INFO core2keypad-2.0 %dx%d mode=%s vib=%d connected=%d drawer=%d notches=%d up=%lus boot=%u heap=%u\n",
                              W, H, kModeName[g_mode], g_vib ? 1 : 0, g_connected ? 1 : 0,
                              g_drawer ? 1 : 0, g_notches, (unsigned long)(millis() / 1000), (unsigned)g_boots, (unsigned)ESP.getFreeHeap());
            } else if (command == "C2_BLE") {
                BLEService *hid = g_server ? g_server->getServiceByUUID(BLEUUID((uint16_t)0x1812)) : nullptr;
                Serial.printf("C2_BLE srv=%d hid=%d handle=%u conn=%d\n",
                              g_server ? 1 : 0, hid ? 1 : 0, hid ? hid->getHandle() : 0,
                              g_connected ? 1 : 0);
            } else if (command == "C2_CCCD") {
                BLEDescriptor *dKb = g_kb ? g_kb->getDescriptorByUUID(BLEUUID((uint16_t)0x2902)) : nullptr;
                BLEDescriptor *dMs = g_ms ? g_ms->getDescriptorByUUID(BLEUUID((uint16_t)0x2902)) : nullptr;
                const uint8_t *vk = dKb ? dKb->getValue() : nullptr;
                const uint8_t *vm = dMs ? dMs->getValue() : nullptr;
                Serial.printf("C2_CCCD kb=%02X%02X ms=%02X%02X conn=%d peers=%d\n",
                              vk ? vk[0] : 0xFF, vk ? vk[1] : 0xFF,
                              vm ? vm[0] : 0xFF, vm ? vm[1] : 0xFF,
                              g_connected ? 1 : 0,
                              g_server ? (int)g_server->getConnectedCount() : -1);
            } else if (command.startsWith("C2_MODE ")) {
                int m = -1;
                if (sscanf(command.c_str(), "C2_MODE %d", &m) == 1 && m >= 0 && m < MODE_COUNT) {
                    setMode((uint8_t)m);
                    Serial.printf("C2_MODE %s\n", kModeName[g_mode]);
                } else {
                    Serial.println("C2_ERROR bad_mode");
                }
            } else if (command.startsWith("C2_VIB ")) {
                int v = -1;
                if (sscanf(command.c_str(), "C2_VIB %d", &v) == 1 && (v == 0 || v == 1)) {
                    if ((bool)v != g_vib) toggleVib();
                    Serial.printf("C2_VIB %d\n", g_vib ? 1 : 0);
                } else {
                    Serial.println("C2_ERROR bad_value");
                }
            } else if (command == "C2_DISC") {
        if (g_connected) {
            g_server->disconnect(g_server->getConnId());
            Serial.println("C2_DISC sent");
        } else {
            Serial.println("C2_DISC not_connected");
        }
    } else if (command == "C2_BUZZ") {
                const bool ok = M5.Power.setVibration(1);
                if (ok) { delay(BUZZ_MS); M5.Power.setVibration(0); }
                Serial.printf("C2_BUZZ ok=%d vib=%d\n", ok ? 1 : 0, g_vib ? 1 : 0);
            } else if (command == "C2_T 9") {
                g_inject = false;
                Serial.println("C2_T real");
            } else if (command.startsWith("C2_T ")) {
                int p = -1, x = 0, y = 0;
                if (sscanf(command.c_str(), "C2_T %d %d %d", &p, &x, &y) == 3 && (p == 0 || p == 1)) {
                    g_inject = true;
                    g_injPressed = (p == 1);
                    g_injX = (int16_t)x;
                    g_injY = (int16_t)y;
                    g_injUntil = millis() + 15000;
                    Serial.printf("C2_T %d %d %d\n", p, x, y);
                } else {
                    Serial.println("C2_ERROR bad_touch");
                }
            } else if (command.length()) {
                Serial.println("C2_ERROR unknown_command");
            }
            command = "";
        } else if (command.length() < 64) {
            command += ch;
        } else {
            command = "";
        }
    }
    if (command.length() && lastByte && millis() - lastByte > 300) command = "";
}

// ---------------------------------------------------------------- 生命周期
void setup()
{
    // 主机按 MAC 缓存 HID 描述符；从出厂 MAC 派生一个稳定地址，重启不变。
    uint8_t baseMac[6];
    esp_efuse_mac_get_default(baseMac);
    baseMac[5] ^= 0x01;
    esp_base_mac_addr_set(baseMac);

    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.setTxBufferSize(4096);         // 截图分块发送要留出空间，避免阻塞写
    Serial.begin(921600);
    delay(150);
    g_lastInputMs = millis();
    applyBrightness(kBrightMax);

    // 先占帧缓冲，再起 BLE（BLE 协议栈吃内存，顺序反了 sprite 会分配失败）
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    g_canvas = canvas.createSprite(W, H) != nullptr;
    if (!g_canvas) {
        canvas.setPsram(false);
        g_canvas = canvas.createSprite(W, H) != nullptr;
    }

    g_prefs.begin("keypad", false);
    g_boots = (uint16_t)(g_prefs.getUShort("boots", 0) + 1);
    g_prefs.putUShort("boots", g_boots);
    g_mode = g_prefs.getUChar("mode", MODE_SLIDE);
    if (g_mode >= MODE_COUNT) g_mode = MODE_SLIDE;
    g_vib = g_prefs.getUChar("vib", 1) != 0;

    const int lvl = M5.Power.getBatteryLevel();
    g_battPct = (lvl < 0) ? 0 : (uint8_t)lvl;

    bleInit();
    render(millis());

    uint8_t btMac[6];
    esp_read_mac(btMac, ESP_MAC_BT);
    Serial.printf("[KEY] ready v2 mode=%s vib=%d heap=%u btMac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  kModeName[g_mode], g_vib ? 1 : 0, (unsigned)ESP.getFreeHeap(),
                  btMac[0], btMac[1], btMac[2], btMac[3], btMac[4], btMac[5]);
    Serial.println("[KEY] cmd: C2_INFO C2_BLE C2_CCCD C2_MODE C2_VIB C2_BUZZ C2_SCREEN C2_T");
}

void loop()
{
    M5.update();
    const uint32_t now = millis();

    capturePump();                       // 截图分批发，不阻塞链路
    if (g_capActive) {
        // 截图期间也要把马达按时关掉，否则一次截图能把震动拖成 3 秒
        if (g_vibMotor && (int32_t)(now - g_vibUntil) >= 0) {
            M5.Power.setVibration(0);
            g_vibMotor = false;
        }
        delay(2);
        return;
    }

    static bool blocked = false;      // 起点在状态条上的触摸整段忽略
    static uint32_t battAt = 0;

    if (g_inject && (int32_t)(now - g_injUntil) < 0) {
        feedTouch(g_injPressed, g_injX, g_injY);
        blocked = false;
    } else {
        g_inject = false;
        const auto &td = M5.Touch.getDetail();
        if (!td.isPressed()) {
            blocked = false;
            feedTouch(false, td.x, td.y);
        } else if (!g_pressed) {
            if (!blocked) {
                if (td.y >= BAR_H) feedTouch(true, td.x, td.y);
                else blocked = true;
            }
        } else {
            feedTouch(true, td.x, td.y);
        }
        if (td.isPressed() || td.wasPressed()) g_lastInputMs = now;
    }

    pollHold(now);

    if (g_advPending && (int32_t)(now - g_advAt) >= 0) {
        g_advPending = false;
        g_server->startAdvertising();
        Serial.println("[BLE] advertising restarted");
    }
    if (!g_connected && !g_advPending && (int32_t)(now - g_advRetryAt) >= 0) {
        g_advRetryAt = now + 10000;
        g_server->startAdvertising();
    }

    if (g_vibMotor && (int32_t)(now - g_vibUntil) >= 0) {
        M5.Power.setVibration(0);
        g_vibMotor = false;
    }

    if (now - battAt > 2000) {
        battAt = now;
        const int lvl = M5.Power.getBatteryLevel();
        if (lvl >= 0) g_battPct = (uint8_t)lvl;
    }

    handleSerial();
    updateBrightness(now);

    const bool anim = holdCandidate(now) ||
                      (g_rippleAt && (now - g_rippleAt) < RIPPLE_MS);
    const bool due  = g_needRender && (int32_t)(now - g_renderAt) >= 0;

    if ((due || (anim && now - g_lastRender >= 45)) && now - g_lastRender >= 12) {
        render(now);
        g_lastRender = now;
        g_needRender = false;
    }

    delay(4);
}
