// -----------------------------------------------------------------------------
//  Core2Keypad —— 把 M5Stack Core2 变成一只「两个键」的 BLE 输入设备
//
//  两种输出模式，右侧竖排开关切换（掉电保存）：
//    KEY    发 ↑ / ↓ 方向键       —— 文本编辑、列表导航
//    WHEEL  发鼠标滚轮上/下       —— 抖音这类只认滚动手势的竖屏信息流
//
//  为什么需要 WHEEL：抖音的竖屏流是触摸翻页容器，全屏播放器没有可聚焦控件，
//  触摸模式下系统会丢弃硬件方向键；滚轮是通用滚动事件，列表控件普遍响应。
//
//  HID 为复合设备（规范写法，报告值里带 Report ID）：
//    Report ID 1: 键盘 9 字节 [id, modifiers, reserved, key1..key6]
//    Report ID 2: 鼠标 5 字节 [id, buttons, dx, dy, wheel]
//  改动描述符后主机侧会缓存旧值，必须删设备重配对一次。
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

static constexpr int BTN_X = 14;
static constexpr int BTN_W = 228;      // 让出右侧 56px 给模式开关
static constexpr int BTN_H = 84;
static constexpr int BTN_R = 16;
static constexpr int UP_Y   = 40;      // 40..124，center 82
static constexpr int DOWN_Y = 132;     // 132..216，center 174

static constexpr int ACCENT_X = 28;
static constexpr int ACCENT_W = 4;
static constexpr int ACCENT_H = 52;

static constexpr int CHEV_CX = 52;
static constexpr int CHEV_HW = 12;
static constexpr int CHEV_HH = 9;

static constexpr int LABEL_X = 80;

// 右侧模式开关
static constexpr int RAIL_X = 250;
static constexpr int RAIL_Y = 40;
static constexpr int RAIL_W = 56;
static constexpr int RAIL_H = 176;
static constexpr int RAIL_R = 16;

static constexpr int SEG_X = 254;
static constexpr int SEG_W = 48;
static constexpr int SEG_H = 82;
static constexpr int SEG_KEY_Y   = 44;
static constexpr int SEG_WHEEL_Y = 130;
static constexpr int SEG_R = 12;

// 滚轮：一次按压发多少个刻度（每刻度一份独立报告，模拟真实滚轮）
static constexpr int WHEEL_NOTCHES = 4;
static constexpr int WHEEL_GAP_MS  = 6;

// ---------------------------------------------------------------- 模式
enum : int { MODE_KEY = 0, MODE_WHEEL = 1, MODE_COUNT = 2 };
static const char *kModeName[MODE_COUNT] = { "KEY", "WHEEL" };

static int g_mode = MODE_KEY;
static Preferences g_prefs;

// ---------------------------------------------------------------- BLE
static BLEServer         *g_server = nullptr;
static BLEHIDDevice      *g_hid    = nullptr;
static BLECharacteristic *g_kb     = nullptr;
static BLECharacteristic *g_ms     = nullptr;
static volatile bool      g_connected = false;

static void render();   // 前向声明：断连回调里要重绘状态条

// 只用来在串口上看配对/加密到底成没成
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

// ---------------------------------------------------------------- 诊断
// HOGP 的输入报告只能通过订阅（CCCD 0x2902 写 0x0001）才能发出。Arduino BLE 的
// BLECharacteristic::notify() 会先看本地 0x2902 的值，为 0 就静默丢掉，不报错。
// 这两个回调把「主机到底有没有重写 CCCD」和「notify 被什么理由拦掉」打出来。
class CccdCB : public BLEDescriptorCallbacks {
    const char *m_tag;
    const char *m_key;
public:
    CccdCB(const char *tag, const char *key) : m_tag(tag), m_key(key) {}
    // 只有主机真正写 0x2902 时才落盘；否则开机默认值会把「已订阅」冲掉
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
        case SUCCESS_NOTIFY:        return;                       // 正常发出去的不刷屏
        case ERROR_NOTIFY_DISABLED: name = "DROP cccd-not-subscribed"; break;
        case ERROR_INDICATE_DISABLED: name = "DROP cccd-not-subscribed(ind)"; break;
        case ERROR_NO_CLIENT:       name = "DROP no-client"; break;
        case ERROR_GATT:            name = "DROP gatt-error"; break;
        default: break;
        }
        Serial.printf("[HID] %s %s code=%u\n", m_tag, name, (unsigned)code);
    }
};

// ---------------------------------------------------------------- CCCD 持久化
// 绑定过的主机（Windows / Android / iOS）重连时不会重写 CCCD —— 规范要求外设替
// 它记住（Core 3.G §3.3.3.3：bonded 连接的 CCCD 值要跨连接保持）。而 ESP32 的
// 0x2902 值只活在 RAM 里，一 reset 就归零，BLECharacteristic::notify() 于是静默
// 丢弃所有报告。这正是「主机显示已连接、设备侧 connected=1，但按键毫无反应」。
// 对策：订阅状态落 NVS，连接建立后立刻回填到 GATT 数据库。
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
    d->setValue(v, 2);          // 同时写进协议栈的属性表，notify() 才会放行
    Serial.printf("[CCCD] set notify=%d handle=%u\n", on ? 1 : 0, d->getHandle());
}

static void cccdRestore()
{
    // 有绑定就默认「已订阅」：HOGP 主机配对时必定会打开输入报告通知，之后重连它
    // 不再写 CCCD，只有外设替它记住这一条路。主机显式退订过（存了 0）则尊重 0。
    const bool bonded = esp_ble_get_bond_device_num() > 0;
    const int8_t sk = (int8_t)g_prefs.getChar("cck_kb", (int8_t)-1);
    const int8_t sm = (int8_t)g_prefs.getChar("cck_ms", (int8_t)-1);
    const bool kb = (sk < 0) ? bonded : (sk != 0);
    const bool ms = (sm < 0) ? bonded : (sm != 0);
    cccdSet(g_kb, kb);
    cccdSet(g_ms, ms);
    Serial.printf("[CCCD] restore bonded=%d saved kb=%d ms=%d -> live kb=%d ms=%d\n",
                  bonded ? 1 : 0, sk, sm, cccdOn(g_kb) ? 1 : 0, cccdOn(g_ms) ? 1 : 0);
}

class ServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        g_connected = true;
        Serial.println("[BLE] connected");
        cccdRestore();          // 关键：替主机把订阅状态接回来，否则 notify 全被丢弃
        render();
    }
    void onDisconnect(BLEServer *srv) override {
        g_connected = false;
        Serial.println("[BLE] disconnected");
        srv->startAdvertising();
        render();
    }
};

// 键盘(Report ID 1) + 鼠标滚轮(Report ID 2) 的复合描述符
static const uint8_t kReportMap[] = {
    // ---- 键盘 ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) 修饰键
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const) 保留字节
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x05,        //   Usage Maximum (5)
    0x91, 0x02,        //   Output (Data,Var,Abs) LED
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array) 按键数组
    0xC0,              // End Collection

    // ---- 鼠标（只有滚轮）----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Buttons)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Const) 补齐 1 字节
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0xC0,              //   End Collection
    0xC0               // End Collection
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

    // HID 报告特征与 CCCD 都是 ENCRYPTED 权限（见 BLEHIDDevice::inputReport），
    // 因此必须让主机完成配对/绑定，否则读报告和订阅通知都会被拒绝 ——
    // 表现就是"连上了但按键完全没反应"。这里用 Just Works 绑定（无 MITM）。
    // 注意：不要调用 setEncryptionLevel(ESP_BLE_SEC_ENCRYPT)。
    // 那会让协议栈在连接建立后立刻强制加密，和主机写 CCCD（订阅通知）抢时序，
    // 表现为"每次配对后能用，设备一重启就收不到任何报告"。只做 Just Works 绑定即可。
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

    // 诊断挂载：CCCD 写入 + notify 状态
    static CccdCB cbKb("kb", "cck_kb"), cbMs("ms", "cck_ms");
    static HidCB  hbKb("kb"), hbMs("ms");
    BLEDescriptor *dKb = g_kb->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    BLEDescriptor *dMs = g_ms->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    if (dKb) dKb->setCallbacks(&cbKb);
    if (dMs) dMs->setCallbacks(&cbMs);
    g_kb->setCallbacks(&hbKb);
    g_ms->setCallbacks(&hbMs);
    Serial.printf("[BLE] cccd handles kb=%u ms=%u\n",
                  dKb ? dKb->getHandle() : 0, dMs ? dMs->getHandle() : 0);

    BLEAdvertising *adv = g_server->getAdvertising();
    adv->setAppearance(0x03C1);           // Generic Keyboard
    adv->addServiceUUID(g_hid->hidService()->getUUID());
    adv->setScanResponse(true);
    adv->start();

    Serial.printf("[BLE] hid svc handle=%u kb=%d ms=%d\n",
                  g_hid->hidService()->getHandle(), g_kb ? 1 : 0, g_ms ? 1 : 0);
}

// ---------------------------------------------------------------- 发送
// Report ID 前缀：HOGP 规范说报告首字节应带 Report ID，但 Android/Windows 的
// 主机栈都把「特征值」整体当作报告本体。之前能用的实现（8 字节不带 ID）就是
// 这个约定；带 ID 会让首字节被当成修饰键，整条报告错位。
// 默认不带，可用 C2_REPORTID 1 切回去对比。
static bool g_idPrefix = false;
static int  g_notches  = WHEEL_NOTCHES;

static void sendKeyboard(uint8_t keycode, bool down)
{
    uint8_t buf[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (g_idPrefix) {
        buf[0] = 1;
        if (down) buf[3] = keycode;
        g_kb->setValue(buf, 9);
    } else {
        if (down) buf[2] = keycode;
        g_kb->setValue(buf, 8);
    }
    g_kb->notify();
}

static void sendWheel(int8_t dir)
{
    for (int i = 0; i < g_notches; ++i) {
        uint8_t buf[5] = { 0, 0, 0, 0, (uint8_t)dir };
        if (g_idPrefix) {
            buf[0] = 2;
            g_ms->setValue(buf, 5);
        } else {
            buf[0] = 0;                       // buttons
            g_ms->setValue(buf + 1, 4);
        }
        g_ms->notify();
        delay(WHEEL_GAP_MS);
    }
}

// ---------------------------------------------------------------- 亮度
// 规则：
//   * 开机 / 任何触摸   -> 100%，并重新开始 3 秒计时
//   * 空闲满 3 秒       -> 在 1 秒内线性降到 20%
//   * 触摸随时打断渐变   -> 立即回到 100%
static constexpr uint8_t  kBrightMax = 255;   // 100%
static constexpr uint8_t  kBrightDim = 51;    // 20%
static constexpr uint32_t kHoldMs    = 3000;
static constexpr uint32_t kFadeMs    = 1000;

static uint32_t g_lastInputMs = 0;
static uint8_t  g_brightness  = kBrightMax;

static void applyBrightness(uint8_t v)
{
    if (v == g_brightness) return;
    g_brightness = v;
    M5.Display.setBrightness(v);
    Serial.printf("[BRI] %3u  t=%lu\n", v, (unsigned long)millis());
}

static void updateBrightness()
{
    const uint32_t idle = millis() - g_lastInputMs;
    if (idle <= kHoldMs) { applyBrightness(kBrightMax); return; }

    const uint32_t e = idle - kHoldMs;
    if (e >= kFadeMs) { applyBrightness(kBrightDim); return; }

    const uint32_t span = (uint32_t)(kBrightMax - kBrightDim);
    applyBrightness((uint8_t)(kBrightMax - span * e / kFadeMs));
}

// ---------------------------------------------------------------- 绘制
static M5Canvas canvas(&M5.Display);
static bool  g_canvas = false;
static int   g_active = -1;

static inline uint16_t px565(uint16_t raw) { return (uint16_t)((raw >> 8) | (raw << 8)); }
static inline uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); }

static void chevron(int cx, int cy, bool up, uint16_t color)
{
    const int sign = up ? -1 : 1;
    const int xL = cx - CHEV_HW, xR = cx + CHEV_HW;
    const int yTip = cy + sign * CHEV_HH;
    const int yEnd = cy - sign * CHEV_HH;

    canvas.drawWideLine(xL, yEnd, cx, yTip, 7, color);
    canvas.drawWideLine(cx, yTip, xR, yEnd, 7, color);
    canvas.fillCircle(xL, yEnd, 3, color);
    canvas.fillCircle(xR, yEnd, 3, color);
    canvas.fillCircle(cx, yTip, 3, color);
}

static const char *subLabel(int up)
{
    if (g_mode == MODE_WHEEL) return up ? "SCROLL UP" : "SCROLL DOWN";
    return up ? "ARROW UP" : "ARROW DOWN";
}

static void drawButton(int y, bool upArrow, const char *label, const char *sub,
                       uint16_t accent, bool pressed)
{
    const uint16_t face   = pressed ? col(0x17, 0x2A, 0x3B) : col(0x0E, 0x16, 0x20);
    const uint16_t border = pressed ? accent : col(0x1B, 0x2C, 0x3A);

    canvas.fillRoundRect(BTN_X, y, BTN_W, BTN_H, BTN_R, face);
    canvas.drawRoundRect(BTN_X, y, BTN_W, BTN_H, BTN_R, border);
    canvas.fillRoundRect(ACCENT_X, y + (BTN_H - ACCENT_H) / 2, ACCENT_W, ACCENT_H, 2, accent);

    const int yc = y + BTN_H / 2;
    chevron(CHEV_CX, yc, upArrow, accent);

    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextSize(1);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(pressed ? TFT_WHITE : col(0xE8, 0xF2, 0xFF));
    canvas.drawString(label, LABEL_X, yc - 2);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(col(0x4A, 0x63, 0x79));
    canvas.drawString(sub, LABEL_X, yc + 24);
}

// 右侧竖排模式开关：上=键盘，下=滚轮
static void drawSegment(int y, const char *text, bool active, uint16_t accent)
{
    const uint16_t face   = active ? col(0x10, 0x2A, 0x38) : col(0x0E, 0x16, 0x20);
    const uint16_t border = active ? accent : col(0x1B, 0x2C, 0x3A);
    const uint16_t ink    = active ? TFT_WHITE : col(0x4A, 0x63, 0x79);

    canvas.fillRoundRect(SEG_X, y, SEG_W, SEG_H, SEG_R, face);
    canvas.drawRoundRect(SEG_X, y, SEG_W, SEG_H, SEG_R, border);

    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(ink);
    canvas.drawString(text, SEG_X + SEG_W / 2, y + SEG_H / 2);
}

static void drawRail()
{
    canvas.fillRoundRect(RAIL_X, RAIL_Y, RAIL_W, RAIL_H, RAIL_R, col(0x0B, 0x12, 0x1A));
    canvas.drawRoundRect(RAIL_X, RAIL_Y, RAIL_W, RAIL_H, RAIL_R, col(0x1B, 0x2C, 0x3A));

    drawSegment(SEG_KEY_Y,   "KEY",   g_mode == MODE_KEY,   col(0x38, 0xBD, 0xF8));
    drawSegment(SEG_WHEEL_Y, "WHEEL", g_mode == MODE_WHEEL, col(0xFB, 0xBF, 0x24));
}

static void drawStatus()
{
    const uint16_t line   = col(0x16, 0x23, 0x2E);
    const uint16_t header = col(0x54, 0x67, 0x7A);
    const uint16_t dim    = col(0x4A, 0x63, 0x79);
    const uint16_t okCol  = col(0x34, 0xD3, 0x99);
    const uint16_t warn   = col(0xFB, 0xBF, 0x24);

    canvas.fillRect(0, 0, W, 30, col(0x0A, 0x0F, 0x15));
    canvas.fillRect(0, 30, W, 1, line);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(header);
    canvas.drawString("CORE2 HID KEYPAD", 14, 15);

    char batt[16];
    const int level = M5.Power.getBatteryLevel();
    if (level >= 0) snprintf(batt, sizeof(batt), "%d%%", level);
    else            snprintf(batt, sizeof(batt), "--");

    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.setTextColor(dim);
    canvas.drawString(batt, W - 14, 15);

    const char *state = g_connected ? "CONNECTED" : "ADVERTISING";
    const uint16_t c  = g_connected ? okCol : warn;
    const int tw = canvas.textWidth(state);
    const int x = W - 14 - canvas.textWidth(batt) - 14 - tw;

    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(c);
    canvas.drawString(state, x, 15);
    canvas.fillCircle(x - 8, 15, 3, c);
}

static void render()
{
    if (!g_canvas) return;

    canvas.fillScreen(col(0x0A, 0x0F, 0x15));
    drawStatus();
    drawButton(UP_Y,   true,  "UP",   subLabel(1), col(0x38, 0xBD, 0xF8), g_active == 0);
    drawButton(DOWN_Y, false, "DOWN", subLabel(0), col(0xFB, 0xBF, 0x24), g_active == 1);
    drawRail();

    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(col(0x3A, 0x48, 0x56));
    canvas.drawString("TOUCH TO SEND", W / 2 - 28, 231);

    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- 按键
static void pressKey(int which)
{
    Serial.printf("[KEY] press %s mode=%s conn=%d\n",
                  which == 0 ? "UP" : "DOWN", kModeName[g_mode], g_connected ? 1 : 0);
    if (!g_connected) return;

    if (g_mode == MODE_WHEEL) { sendWheel(which == 0 ? 1 : -1); return; }
    sendKeyboard(which == 0 ? 0x52 : 0x51, true);          // Up / Down
}

static void releaseKey(int which)
{
    if (!g_connected || g_mode == MODE_WHEEL) return;
    sendKeyboard(which == 0 ? 0x52 : 0x51, false);
}

static bool hitUp(int x, int y)
{
    return x >= BTN_X && x < BTN_X + BTN_W && y >= UP_Y && y < UP_Y + BTN_H;
}

static bool hitDown(int x, int y)
{
    return x >= BTN_X && x < BTN_X + BTN_W && y >= DOWN_Y && y < DOWN_Y + BTN_H;
}

static bool hitRailKey(int x, int y)
{
    return x >= SEG_X && x < SEG_X + SEG_W && y >= SEG_KEY_Y && y < SEG_KEY_Y + SEG_H;
}

static bool hitRailWheel(int x, int y)
{
    return x >= SEG_X && x < SEG_X + SEG_W && y >= SEG_WHEEL_Y && y < SEG_WHEEL_Y + SEG_H;
}

static void setMode(int m)
{
    if (m < 0 || m >= MODE_COUNT || m == g_mode) return;
    g_mode = m;
    g_prefs.putUChar("mode", (uint8_t)g_mode);
    Serial.printf("[MODE] -> %s\n", kModeName[g_mode]);
    render();
}

static void updateTouch()
{
    const auto &td = M5.Touch.getDetail();
    const bool pressed = td.isPressed();
    static bool railArmed = false;

    if (!pressed) railArmed = false;

    // 右侧开关优先处理，且按住不重复触发
    if (pressed && (hitRailKey(td.x, td.y) || hitRailWheel(td.x, td.y))) {
        if (!railArmed) {
            railArmed = true;
            if (g_active >= 0) { releaseKey(g_active); g_active = -1; }
            setMode(hitRailKey(td.x, td.y) ? MODE_KEY : MODE_WHEEL);
        }
        return;
    }

    int now = -1;
    if (pressed) {
        if (hitUp(td.x, td.y))        now = 0;
        else if (hitDown(td.x, td.y)) now = 1;
    }

    if (now == g_active) return;

    Serial.printf("[TOUCH] active %d -> %d  at (%d,%d)\n", g_active, now, td.x, td.y);
    if (g_active >= 0) releaseKey(g_active);
    g_active = now;
    if (g_active >= 0) pressKey(g_active);
    render();
}

// ---------------------------------------------------------------- 截图 / 命令
static void captureScreen(Stream &out)
{
    const uint16_t *buf = g_canvas
        ? static_cast<const uint16_t *>(canvas.getBuffer())
        : nullptr;
    if (!buf) { out.println("C2_ERROR no_framebuffer"); return; }

    out.printf("C2_SCREEN %d %d %u\n", W, H, (unsigned)(W * H * 3));

    static uint8_t row[W * 3];
    const uint32_t started = millis();
    for (int y = 0; y < H; ++y) {
        const uint16_t *src = buf + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            const uint16_t c = px565(src[x]);
            row[x * 3 + 0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
            row[x * 3 + 2] = (uint8_t)(( c        & 0x1F) * 255 / 31);
        }
        size_t sent = 0;
        const size_t total = (size_t)W * 3;
        while (sent < total) {
            if (millis() - started > 30000) { out.println("C2_ERROR transfer_timeout"); return; }
            const size_t n = out.write(row + sent, total - sent);
            sent += n;
            if (!n) delay(1);
        }
    }
    out.flush();
}

static void sendTestTap(int which)
{
    if (!g_connected) { Serial.println("C2_ERROR not_connected"); return; }
    pressKey(which);
    delay(60);
    releaseKey(which);
    Serial.printf("C2_TAP %s mode=%s\n", which == 0 ? "UP" : "DOWN", kModeName[g_mode]);
}

static void handleSerial()
{
    static String command;
    static uint32_t lastByte = 0;

    while (Serial.available()) {
        const char ch = (char)Serial.read();
        lastByte = millis();
        if (ch == '\n') {
            command.trim();
            if (command == "C2_SCREEN")      captureScreen(Serial);
            else if (command == "C2_INFO")   Serial.printf("C2_INFO core2keypad-1.4 %dx%d mode=%s connected=%d idprefix=%d notches=%d name=Core2 Keypad\n",
                                                           W, H, kModeName[g_mode], g_connected ? 1 : 0, g_idPrefix ? 1 : 0, g_notches);
            else if (command == "C2_BLE") {
                BLEService *hid = g_server ? g_server->getServiceByUUID(BLEUUID((uint16_t)0x1812)) : nullptr;
                Serial.printf("C2_BLE srv=%d hid=%d hid_handle=%u conn=%d\n",
                              g_server ? 1 : 0, hid ? 1 : 0,
                              hid ? hid->getHandle() : 0, g_connected ? 1 : 0);
            }
            else if (command == "C2_CCCD") {
                BLEDescriptor *dKb = g_kb ? g_kb->getDescriptorByUUID(BLEUUID((uint16_t)0x2902)) : nullptr;
                BLEDescriptor *dMs = g_ms ? g_ms->getDescriptorByUUID(BLEUUID((uint16_t)0x2902)) : nullptr;
                const uint8_t *vk = dKb ? dKb->getValue() : nullptr;
                const uint8_t *vm = dMs ? dMs->getValue() : nullptr;
                Serial.printf("C2_CCCD kb=%02X%02X ms=%02X%02X conn=%d peers=%d\n",
                              vk ? vk[0] : 0xFF, vk ? vk[1] : 0xFF,
                              vm ? vm[0] : 0xFF, vm ? vm[1] : 0xFF,
                              g_connected ? 1 : 0,
                              g_server ? (int)g_server->getConnectedCount() : -1);
            }
            else if (command == "C2_MODE")   setMode((g_mode + 1) % MODE_COUNT);
            else if (command.startsWith("C2_SETMODE ")) {
                int m = -1;
                if (sscanf(command.c_str(), "C2_SETMODE %d", &m) == 1 && m >= 0 && m < MODE_COUNT) {
                    if (m == g_mode) render();
                    setMode(m);
                    Serial.printf("C2_MODE %s\n", kModeName[g_mode]);
                } else {
                    Serial.println("C2_ERROR bad_mode");
                }
            }
            else if (command.startsWith("C2_REPORTID ")) {
                int v = -1;
                if (sscanf(command.c_str(), "C2_REPORTID %d", &v) == 1 && (v == 0 || v == 1)) {
                    g_idPrefix = v != 0;
                    Serial.printf("C2_REPORTID %d\n", g_idPrefix ? 1 : 0);
                } else {
                    Serial.println("C2_ERROR bad_value");
                }
            }
            else if (command.startsWith("C2_NOTCHES ")) {
                int v = -1;
                if (sscanf(command.c_str(), "C2_NOTCHES %d", &v) == 1 && v >= 1 && v <= 40) {
                    g_notches = v;
                    Serial.printf("C2_NOTCHES %d\n", g_notches);
                } else {
                    Serial.println("C2_ERROR bad_value");
                }
            }
            else if (command == "K_UP")      sendTestTap(0);
            else if (command == "K_DOWN")    sendTestTap(1);
            else if (command.startsWith("C2_TOUCH ")) {
                int x = 0, y = 0;
                if (sscanf(command.c_str(), "C2_TOUCH %d %d", &x, &y) == 2) {
                    const char *hit = hitUp(x, y) ? "UP"
                                    : hitDown(x, y) ? "DOWN"
                                    : hitRailKey(x, y) ? "MODE_KEY"
                                    : hitRailWheel(x, y) ? "MODE_WHEEL" : "NONE";
                    Serial.printf("C2_TOUCH %d %d -> %s\n", x, y, hit);
                } else {
                    Serial.println("C2_ERROR bad_touch_args");
                }
            }
            else if (command.length())       Serial.println("C2_ERROR unknown_command");
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
    // 主机（尤其 Android）会按 MAC 缓存 HID 报告描述符与特征句柄。改过描述符后，
    // 旧缓存会让输入设备映射到不存在的句柄，表现为"连着但一个事件都没有"。
    // 这里从出厂 MAC 派生一个稳定的新地址，让主机当成全新设备重新读取描述符。
    uint8_t baseMac[6];
    esp_efuse_mac_get_default(baseMac);
    baseMac[5] ^= 0x01;
    esp_base_mac_addr_set(baseMac);

    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(921600);
    delay(150);
    g_lastInputMs = millis();
    applyBrightness(kBrightMax);          // 开机先满亮，3 秒后开始变暗

    // 先占帧缓冲，再起 BLE（BLE 协议栈吃内存，顺序反了 sprite 会分配失败）
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    g_canvas = canvas.createSprite(W, H) != nullptr;
    if (!g_canvas) {
        canvas.setPsram(false);
        g_canvas = canvas.createSprite(W, H) != nullptr;
    }

    g_prefs.begin("keypad", false);
    g_mode = (int)g_prefs.getUChar("mode", MODE_KEY);
    if (g_mode < 0 || g_mode >= MODE_COUNT) g_mode = MODE_KEY;

    bleInit();
    render();

    uint8_t btMac[6];
    esp_read_mac(btMac, ESP_MAC_BT);
    Serial.printf("[KEY] ready  mode=%s  heap=%u  btMac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  kModeName[g_mode], (unsigned)ESP.getFreeHeap(),
                  btMac[0], btMac[1], btMac[2], btMac[3], btMac[4], btMac[5]);
    Serial.println("[KEY] commands: C2_INFO / C2_BLE / C2_CCCD / C2_SCREEN / C2_MODE / C2_SETMODE <0|1> / K_UP / K_DOWN");
}

void loop()
{
    M5.update();
    if (M5.Touch.getDetail().isPressed()) g_lastInputMs = millis();   // 触摸立刻满亮
    handleSerial();
    updateTouch();
    updateBrightness();

    static uint32_t lastPoll = 0;
    if (millis() - lastPoll > 1500) {
        lastPoll = millis();
        static bool lastConn = false;
        if (g_connected != lastConn) { lastConn = g_connected; render(); }
    }

    delay(5);
}
