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
static int quota = -1, quota_reset = 0;
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

static lv_obj_t *background_image;
static lv_obj_t *dusk_image;
static lv_obj_t *pigeon_stand_image;
static lv_obj_t *pigeon_fly_image;
static bool pigeon_fluttering = false;
static lv_obj_t *quota_value;
static lv_obj_t *quota_value_shadow;
static lv_obj_t *quota_fresh_dot;
static lv_obj_t *quota_bar;
static lv_obj_t *quota_detail;
static lv_obj_t *quota_detail_shadow;
static lv_obj_t *secondary_label;
static lv_obj_t *secondary_label_shadow;
static lv_obj_t *secondary_reset_label;
static lv_obj_t *secondary_reset_shadow;
static lv_obj_t *secondary_bar;

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

// A dark offset copy keeps light text readable on both bright and dusk photos.
static lv_obj_t *photo_label(lv_obj_t *parent, const char *text, int x, int y,
                              int w, int h, const lv_font_t *font,
                              lv_color_t color, lv_obj_t **shadow)
{
    *shadow = label(parent, text, x + 2, y + 2, w, h, font, NAVY);
    return label(parent, text, x, y, w, h, font, color);
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
    lv_obj_set_style_bg_color(quota_fresh_dot, quota_live ? CYAN : PALE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(quota_fresh_dot,
                            quota < 0 ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    if (quota < 0) {
        lv_label_set_text(quota_value, "--%");
        lv_label_set_text(quota_value_shadow, "--%");
        lv_label_set_text(quota_detail, "--/-- --:--");
        lv_label_set_text(quota_detail_shadow, "--/-- --:--");
        lv_bar_set_value(quota_bar, 0, LV_ANIM_OFF);
    } else {
        char value[10];
        snprintf(value, sizeof(value), "%d%%", quota);
        lv_label_set_text(quota_value, value);
        lv_label_set_text(quota_value_shadow, value);
        char reset_text[20];
        format_reset(quota_reset, reset_text, sizeof(reset_text));
        lv_label_set_text(quota_detail, reset_text);
        lv_label_set_text(quota_detail_shadow, reset_text);
        lv_bar_set_value(quota_bar, quota, LV_ANIM_ON);
    }
    if (secondary_quota >= 0) {
        char detail[32];
        int days = secondary_window / 1440;
        if (days > 0) {
            snprintf(detail, sizeof(detail), "%d%%  %dD", secondary_quota, days);
        } else {
            snprintf(detail, sizeof(detail), "%d%%  %dH",
                     secondary_quota, secondary_window / 60);
        }
        lv_label_set_text(secondary_label, detail);
        lv_label_set_text(secondary_label_shadow, detail);
        char reset_text[20];
        format_reset(secondary_reset, reset_text, sizeof(reset_text));
        lv_label_set_text(secondary_reset_label, reset_text);
        lv_label_set_text(secondary_reset_shadow, reset_text);
        lv_bar_set_value(secondary_bar, secondary_quota, LV_ANIM_ON);
    } else {
        lv_label_set_text(secondary_label, "--%  7D");
        lv_label_set_text(secondary_label_shadow, "--%  7D");
        lv_label_set_text(secondary_reset_label, "--/-- --:--");
        lv_label_set_text(secondary_reset_shadow, "--/-- --:--");
        lv_bar_set_value(secondary_bar, 0, LV_ANIM_OFF);
    }
}

static void scene_fade_cb(void *obj, int32_t opacity)
{
    lv_obj_set_style_img_opa((lv_obj_t *)obj, (lv_opa_t)opacity, LV_PART_MAIN);
}

static void refresh_scene(bool animate)
{
    int32_t target = day_scene ? LV_OPA_TRANSP : LV_OPA_COVER;
    // The same pigeon sprites are gently darkened to match the dusk scene.
    lv_opa_t tint = day_scene ? LV_OPA_TRANSP : LV_OPA_30;
    lv_obj_set_style_img_recolor(pigeon_stand_image, NAVY, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(pigeon_stand_image, tint, LV_PART_MAIN);
    lv_obj_set_style_img_recolor(pigeon_fly_image, NAVY, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(pigeon_fly_image, tint, LV_PART_MAIN);
    lv_anim_del(dusk_image, scene_fade_cb);
    if (!animate) {
        scene_fade_cb(dusk_image, target);
        return;
    }
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, dusk_image);
    lv_anim_set_exec_cb(&fade, scene_fade_cb);
    lv_anim_set_values(&fade, lv_obj_get_style_img_opa(dusk_image, LV_PART_MAIN), target);
    lv_anim_set_time(&fade, 400);
    lv_anim_start(&fade);
}

static void pigeon_flutter_step(void *obj, int32_t progress)
{
    // Spread the wings in place, with only a small body lift during the flap.
    int32_t lift = 24 * progress * (1000 - progress) / 1000000;
    lv_obj_set_pos((lv_obj_t *)obj, 45, 261 - lift);
}

static void pigeon_flutter_done(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(pigeon_fly_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pigeon_stand_image, LV_OBJ_FLAG_HIDDEN);
    pigeon_fluttering = false;
}

static void pigeon_tap(lv_event_t *e)
{
    (void)e;
    if (pigeon_fluttering) return;
    pigeon_fluttering = true;
    lv_obj_add_flag(pigeon_stand_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pigeon_fly_image, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t flutter;
    lv_anim_init(&flutter);
    lv_anim_set_var(&flutter, pigeon_fly_image);
    lv_anim_set_exec_cb(&flutter, pigeon_flutter_step);
    lv_anim_set_values(&flutter, 0, 1000);
    lv_anim_set_time(&flutter, 500);
    lv_anim_set_ready_cb(&flutter, pigeon_flutter_done);
    lv_anim_start(&flutter);
}

static void scene_tap(lv_event_t *e)
{
    (void)e;
    day_scene = !day_scene;
    refresh_scene(true);
}

static void quota_tap(lv_event_t *e)
{
    (void)e;
    first_fetch = true;
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
    lv_obj_add_flag(background_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(background_image, scene_tap, LV_EVENT_CLICKED, NULL);
    dusk_image = lv_img_create(screen);
    lv_img_set_src(dusk_image, &prague_dusk);
    lv_obj_set_pos(dusk_image, 0, 0);
    lv_obj_set_style_img_opa(dusk_image, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(dusk_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dusk_image, scene_tap, LV_EVENT_CLICKED, NULL);

    // The pigeon is a separate touch target; background taps still change scene.
    pigeon_stand_image = lv_img_create(screen);
    lv_img_set_src(pigeon_stand_image, &pigeon_stand);
    lv_obj_set_pos(pigeon_stand_image, 75, 310);
    lv_obj_add_flag(pigeon_stand_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pigeon_stand_image, pigeon_tap, LV_EVENT_CLICKED, NULL);
    pigeon_fly_image = lv_img_create(screen);
    lv_img_set_src(pigeon_fly_image, &pigeon_fly);
    lv_obj_set_pos(pigeon_fly_image, 45, 261);
    lv_obj_add_flag(pigeon_fly_image, LV_OBJ_FLAG_HIDDEN);

    // Keep the event title on a dark strip; let the quota float on the photo.
    lv_obj_t *header = box(screen, 0, 0, 420, 68, NAVY, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_70, LV_PART_MAIN);
    box(header, 25, 15, 4, 33, GOLD, 2);
    label(header, "SOSP2026", 43, 17, 180, 32, &lv_font_montserrat_26, WHITE);
    label(header, "PRAGUE", 230, 17, 165, 32, &lv_font_montserrat_26, WHITE);
    box(header, 25, 65, 370, 1, GOLD, 0);

    lv_obj_t *quota_panel = box(screen, 0, 68, 360, 146, NAVY, 0);
    lv_obj_set_style_bg_opa(quota_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(quota_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quota_panel, quota_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *heading_shadow;
    photo_label(quota_panel, "CODEX REMAINING", 25, 5, 205, 23,
                &lv_font_montserrat_16, WHITE, &heading_shadow);
    quota_fresh_dot = box(quota_panel, 328, 11, 8, 8, PALE, 4);
    lv_obj_set_style_border_width(quota_fresh_dot, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(quota_fresh_dot, NAVY, LV_PART_MAIN);
    quota_value = photo_label(quota_panel, "--%", 25, 27, 140, 54,
                              &lv_font_montserrat_44, WHITE, &quota_value_shadow);
    quota_detail = photo_label(quota_panel, "--/-- --:--", 175, 47, 165, 25,
                               &lv_font_montserrat_16, GOLD, &quota_detail_shadow);
    quota_bar = lv_bar_create(quota_panel);
    lv_obj_set_pos(quota_bar, 25, 80);
    lv_obj_set_size(quota_bar, 310, 7);
    lv_bar_set_range(quota_bar, 0, 100);
    lv_obj_set_style_bg_color(quota_bar, BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(quota_bar, GOLD, LV_PART_INDICATOR);
    box(quota_panel, 25, 92, 310, 1, CYAN, 0);
    secondary_label = photo_label(quota_panel, "--%  7D", 25, 98, 150, 31,
                                  &lv_font_montserrat_26, WHITE, &secondary_label_shadow);
    secondary_reset_label = photo_label(quota_panel, "--/-- --:--", 175, 105, 165, 23,
                                        &lv_font_montserrat_16, PALE, &secondary_reset_shadow);
    secondary_bar = lv_bar_create(quota_panel);
    lv_obj_set_pos(secondary_bar, 25, 135);
    lv_obj_set_size(secondary_bar, 310, 6);
    lv_bar_set_range(secondary_bar, 0, 100);
    lv_obj_set_style_bg_color(secondary_bar, BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(secondary_bar, CYAN, LV_PART_INDICATOR);

    refresh_scene(false);
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
    quota = p; quota_reset = pr;
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
        lvgl_port_unlock();
    }
}

void loop()
{
    quota_network_tick();
    delay(20);
}
