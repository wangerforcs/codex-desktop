/*
 * Prague photo postcard for Waveshare ESP32-S3-Touch-LCD-4.3C / LVGL 8.4.
 * Codex quota is fetched from a credential-free LAN bridge on the user's PC.
 */
#include "src/lvgl_port/lvgl_port.h"
#include "src/art/prague_images.h"
#include "quota_config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <string.h>

static const lv_color_t NAVY = LV_COLOR_MAKE(8, 28, 49);
static const lv_color_t BLUE = LV_COLOR_MAKE(22, 76, 108);
static const lv_color_t CYAN = LV_COLOR_MAKE(114, 216, 226);
static const lv_color_t WHITE = LV_COLOR_MAKE(248, 253, 255);
static const lv_color_t PALE = LV_COLOR_MAKE(199, 226, 231);
static const lv_color_t GOLD = LV_COLOR_MAKE(242, 203, 133);

static bool day_scene = true;
static int quota = -1, quota_window = 0, quota_reset = 0;
static int secondary_quota = -1, secondary_window = 0, secondary_reset = 0;
static bool quota_live = false;
static bool wifi_started = false;
static bool first_fetch = true;
static bool udp_started = false;
static uint8_t fetch_failures = 0;
static WiFiUDP discovery_udp;
static IPAddress bridge_ip;
static uint32_t last_wifi_attempt = 0, last_fetch_attempt = 0;
static uint32_t last_discovery = 0, last_success = 0;
static uint32_t message_until = 0;

static lv_obj_t *background_image;
static lv_obj_t *quota_value;
static lv_obj_t *quota_source;
static lv_obj_t *quota_bar;
static lv_obj_t *quota_detail;
static lv_obj_t *quota_window_label;
static lv_obj_t *secondary_label;
static lv_obj_t *secondary_reset_label;
static lv_obj_t *secondary_bar;
static lv_obj_t *status_label;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     lv_color_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       int w, int h, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(o, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(o, color, LV_PART_MAIN);
    return o;
}

static void status(const char *text)
{
    lv_label_set_text(status_label, text);
    message_until = millis() + 3500;
}

static bool valid_reset_code(int code)
{
    if (code == 0) return true;
    int month = code / 1000000;
    int day = (code / 10000) % 100;
    int hour = (code / 100) % 100;
    int minute = code % 100;
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
           hour <= 23 && minute <= 59;
}

static void format_reset(int code, char *output, size_t size)
{
    if (!code) {
        snprintf(output, size, "--/-- --:--");
        return;
    }
    snprintf(output, size, "%02d/%02d %02d:%02d", code / 1000000,
             (code / 10000) % 100, (code / 100) % 100, code % 100);
}

static void refresh_quota()
{
    bool configured = QUOTA_WIFI_SSID[0];
    if (quota < 0) {
        lv_label_set_text(quota_value, "--%");
        lv_label_set_text(quota_source, configured ? "WAITING / LAN" : "SET WIFI / PC");
        lv_label_set_text(quota_window_label, "NO DATA");
        lv_label_set_text(quota_detail, "RESET --/-- --:--");
        lv_bar_set_value(quota_bar, 0, LV_ANIM_OFF);
    } else {
        char value[10];
        snprintf(value, sizeof(value), "%d%%", quota);
        lv_label_set_text(quota_value, value);
        lv_label_set_text(quota_source, quota_live ? "LIVE / LAN" : "STALE / LAN");
        char detail[40];
        if (quota_window >= 60) {
            snprintf(detail, sizeof(detail), "%dH WINDOW", quota_window / 60);
        } else {
            snprintf(detail, sizeof(detail), "%dM WINDOW", quota_window);
        }
        lv_label_set_text(quota_window_label, detail);
        char reset_text[20];
        format_reset(quota_reset, reset_text, sizeof(reset_text));
        snprintf(detail, sizeof(detail), "RESET  %s", reset_text);
        lv_label_set_text(quota_detail, detail);
        lv_bar_set_value(quota_bar, quota, LV_ANIM_OFF);
    }
    if (secondary_quota >= 0) {
        char detail[32];
        int days = secondary_window / 1440;
        if (days > 0) {
            snprintf(detail, sizeof(detail), "%dD  %d%%", days, secondary_quota);
        } else {
            snprintf(detail, sizeof(detail), "%dH  %d%%",
                     secondary_window / 60, secondary_quota);
        }
        lv_label_set_text(secondary_label, detail);
        char reset_text[20];
        format_reset(secondary_reset, reset_text, sizeof(reset_text));
        snprintf(detail, sizeof(detail), "RESET %s", reset_text);
        lv_label_set_text(secondary_reset_label, detail);
        lv_bar_set_value(secondary_bar, secondary_quota, LV_ANIM_OFF);
    } else {
        lv_label_set_text(secondary_label, "--%");
        lv_label_set_text(secondary_reset_label, "RESET --/-- --:--");
        lv_bar_set_value(secondary_bar, 0, LV_ANIM_OFF);
    }
}

static void refresh_scene()
{
    lv_img_set_src(background_image, day_scene ? &prague_day : &prague_dusk);
}

static void scene_tap(lv_event_t *e)
{
    (void)e;
    day_scene = !day_scene;
    refresh_scene();
    status(day_scene ? "DAYLIGHT / PRAGUE" : "GOLDEN HOUR / PRAGUE");
}

static void button(lv_obj_t *parent, const char *text, int x, int y, int w,
                   lv_event_cb_t cb)
{
    lv_obj_t *o = lv_btn_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 40);
    lv_obj_set_style_bg_color(o, BLUE, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = label(o, text, 0, 0, w - 4, 24, &lv_font_montserrat_16, WHITE);
    lv_obj_center(t);
}

static void build_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    background_image = lv_img_create(screen);
    lv_img_set_src(background_image, &prague_day);
    lv_obj_set_pos(background_image, 0, 0);

    // Keep the right-hand sky clear so the Old Town Bridge Tower spires show.
    lv_obj_t *header = box(screen, 0, 0, 420, 68, NAVY, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_70, LV_PART_MAIN);
    box(screen, 25, 15, 4, 33, GOLD, 2);
    label(screen, "SOSP2026", 43, 17, 180, 32, &lv_font_montserrat_26, WHITE);
    lv_obj_t *title = lv_img_create(screen);
    lv_img_set_src(title, &prague_title);
    lv_obj_set_pos(title, 230, 9);
    box(screen, 24, 66, 372, 2, GOLD, 1);

    lv_obj_t *hud = box(screen, 25, 88, 350, 206, NAVY, 12);
    lv_obj_set_style_border_width(hud, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(hud, GOLD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hud, LV_OPA_70, LV_PART_MAIN);
    label(hud, "CODEX REMAINING", 18, 12, 215, 23, &lv_font_montserrat_16, PALE);
    quota_source = label(hud, "WAITING / LAN", 234, 15, 102, 20,
                         &lv_font_montserrat_12, CYAN);
    quota_value = label(hud, "--%", 18, 39, 155, 54, &lv_font_montserrat_44, WHITE);
    quota_window_label = label(hud, "NO DATA", 184, 59, 148, 25,
                               &lv_font_montserrat_16, PALE);
    quota_detail = label(hud, "RESET --/-- --:--", 18, 100, 315, 25,
                         &lv_font_montserrat_16, GOLD);
    quota_bar = lv_bar_create(hud);
    lv_obj_set_pos(quota_bar, 18, 131);
    lv_obj_set_size(quota_bar, 314, 8);
    lv_bar_set_range(quota_bar, 0, 100);
    lv_obj_set_style_bg_color(quota_bar, BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(quota_bar, GOLD, LV_PART_INDICATOR);
    box(hud, 18, 149, 314, 1, BLUE, 0);
    secondary_label = label(hud, "--%", 18, 157, 132, 31,
                            &lv_font_montserrat_26, WHITE);
    secondary_reset_label = label(hud, "RESET --/-- --:--", 153, 160, 180, 26,
                                  &lv_font_montserrat_16, PALE);
    secondary_bar = lv_bar_create(hud);
    lv_obj_set_pos(secondary_bar, 18, 191);
    lv_obj_set_size(secondary_bar, 314, 7);
    lv_bar_set_range(secondary_bar, 0, 100);
    lv_obj_set_style_bg_color(secondary_bar, BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(secondary_bar, CYAN, LV_PART_INDICATOR);

    // Compact dock stays to the right of the pigeon and leaves the river open.
    lv_obj_t *dock = box(screen, 340, 417, 440, 63, NAVY, 10);
    lv_obj_set_style_bg_opa(dock, LV_OPA_70, LV_PART_MAIN);
    button(screen, "DAY / DUSK", 351, 428, 151, scene_tap);
    status_label = label(screen, "CHARLES BRIDGE", 520, 425, 242, 23,
                         &lv_font_montserrat_16, WHITE);
    label(screen, "PRAGUE / VLTAVA", 520, 449, 230, 20,
          &lv_font_montserrat_14, GOLD);
    refresh_scene();
    refresh_quota();
}

static bool parse_quota(const String &payload)
{
    int p, pw, pr, s, sw, sr, end = 0;
    if (sscanf(payload.c_str(), "%d,%d,%d,%d,%d,%d%n",
               &p, &pw, &pr, &s, &sw, &sr, &end) != 6 || end == 0) return false;
    if (p < -1 || p > 100 || s < -1 || s > 100 ||
        pw < 0 || sw < 0 || !valid_reset_code(pr) ||
        !valid_reset_code(sr)) return false;
    quota = p; quota_window = pw; quota_reset = pr;
    secondary_quota = s; secondary_window = sw; secondary_reset = sr;
    quota_live = true;
    last_success = millis();
    return true;
}

static void quota_network_tick()
{
    if (!QUOTA_WIFI_SSID[0]) return;
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        bridge_ip = IPAddress(0, 0, 0, 0);
        if (udp_started) discovery_udp.stop();
        udp_started = false;
        if (!wifi_started || now - last_wifi_attempt >= 15000) {
            WiFi.disconnect();
            WiFi.begin(QUOTA_WIFI_SSID, QUOTA_WIFI_PASSWORD);
            wifi_started = true;
            last_wifi_attempt = now;
        }
    } else {
        if (QUOTA_PC_IP[0]) {
            bridge_ip.fromString(QUOTA_PC_IP);
        } else {
            if (!udp_started) {
                udp_started = discovery_udp.begin(47888);
                last_discovery = 0;
            }
            if (udp_started && (last_discovery == 0 || now - last_discovery >= 10000)
                && bridge_ip == IPAddress(0, 0, 0, 0)) {
                discovery_udp.beginPacket(IPAddress(255, 255, 255, 255),
                                          QUOTA_DISCOVERY_PORT);
                discovery_udp.write((const uint8_t *)"PRAGUE_QUOTA_DISCOVER/1", 23);
                discovery_udp.endPacket();
                last_discovery = now;
            }
            int packet_size = udp_started ? discovery_udp.parsePacket() : 0;
            if (packet_size > 0 && packet_size < 32) {
                char reply[32];
                int n = discovery_udp.read(reply, sizeof(reply) - 1);
                reply[n > 0 ? n : 0] = '\0';
                if (strcmp(reply, "PRAGUE_QUOTA_HERE/1") == 0) {
                    bridge_ip = discovery_udp.remoteIP();
                    first_fetch = true;
                }
            } else if (packet_size > 0) {
                while (discovery_udp.available()) discovery_udp.read();
            }
        }
    }
    if (WiFi.status() == WL_CONNECTED && bridge_ip != IPAddress(0, 0, 0, 0) &&
        (first_fetch || now - last_fetch_attempt >= 30000)) {
        first_fetch = false;
        last_fetch_attempt = now;
        WiFiClient client;
        HTTPClient http;
        char url[96];
        String host = bridge_ip.toString();
        snprintf(url, sizeof(url), "http://%s:%d/quota", host.c_str(), QUOTA_PC_PORT);
        http.setTimeout(2500);
        bool ok = false;
        if (http.begin(client, url)) {
            if (http.GET() == HTTP_CODE_OK) {
                String body = http.getString();
                if (lvgl_port_lock(100)) {
                    ok = parse_quota(body);
                    if (ok) refresh_quota();
                    lvgl_port_unlock();
                }
            }
            http.end();
        }
        if (ok) {
            fetch_failures = 0;
        } else if (++fetch_failures >= 2 && !QUOTA_PC_IP[0]) {
            bridge_ip = IPAddress(0, 0, 0, 0);
            fetch_failures = 0;
        }
    }
    // Re-read millis(): a successful fetch updates last_success after 'now'
    // was captured above, otherwise unsigned subtraction marks fresh data stale.
    uint32_t check_now = millis();
    if (quota_live && (WiFi.status() != WL_CONNECTED ||
                       check_now - last_success >= 600000)) {
        quota_live = false;
        if (lvgl_port_lock(100)) {
            refresh_quota();
            lvgl_port_unlock();
        }
    }
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (message_until && (int32_t)(millis() - message_until) >= 0) {
        message_until = 0;
        lv_label_set_text(status_label, "CHARLES BRIDGE");
    }
}

void setup()
{
    Serial.begin(115200);
    DEV_I2C_Init();
    IO_EXTENSION_Init();
    esp_lcd_touch_handle_t touch = touch_gt911_init(DEV_I2C_Get_Bus_Device());
    esp_lcd_panel_handle_t panel = waveshare_esp32_s3_rgb_lcd_init();
    waveshare_rgb_lcd_bl_on();
    ESP_ERROR_CHECK(lvgl_port_init(panel, touch));
    if (lvgl_port_lock(-1)) {
        build_ui();
        lv_timer_create(tick_cb, 1000, NULL);
        lvgl_port_unlock();
    }
}

void loop()
{
    quota_network_tick();
    delay(20);
}
