#ifndef RUBI_FACE_H
#define RUBI_FACE_H

// Rubi's face (13 expressions) drawn with LVGL shapes: glowing white eyes, duck-bill mouth,
// red cheeks. Blinks now and then and moves the mouth while the robot is speaking.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <esp_random.h>
#include <lvgl.h>

#include "application.h"
#include "display/lcd_display.h"
#include "assets/lang_config.h"

#include "rubi_vi_font.h"
#include "rubi_mat_data.h"
#include "rubi_battery.h"
#include "rubi_wifisync.h"
#include <esp_heap_caps.h>

class RubiDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        if (face_ != nullptr || emoji_box_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        BuildFace();
        InitPhoto();
        Apply(Expr::kNeutral);
        timer_ = lv_timer_create(&RubiDisplay::TimerCb, 100, this);
        // Phone-style battery icon, top-right: outline + nub, 4 colour level segments, percentage inside.
        const lv_font_t* bf = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
        int lh = (bf != nullptr) ? lv_font_get_line_height(bf) : 16;
        bat_w_ = 36; bat_h_ = lh * 5 / 8 + 6;   // smaller icon: text is drawn at 62% scale
        bat_box_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(bat_box_);
        lv_obj_set_size(bat_box_, bat_w_, bat_h_);
        lv_obj_align(bat_box_, LV_ALIGN_TOP_RIGHT, -11, 4);
        lv_obj_set_style_border_width(bat_box_, 2, 0);
        lv_obj_set_style_border_color(bat_box_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(bat_box_, 5, 0);
        lv_obj_set_style_bg_color(bat_box_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(bat_box_, LV_OPA_60, 0);
        lv_obj_set_style_clip_corner(bat_box_, true, 0);
        lv_obj_remove_flag(bat_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(bat_box_, LV_OBJ_FLAG_SCROLLABLE);
        // 4 level segments (like a phone): each is lit by SetBatteryPercent()
        const int seg_w = (bat_w_ - 4 - 4 - 3 * 2) / 4, seg_h = bat_h_ - 4 - 4;
        for (int i = 0; i < 4; i++) {
            bat_seg_[i] = lv_obj_create(bat_box_);
            lv_obj_remove_style_all(bat_seg_[i]);
            lv_obj_set_size(bat_seg_[i], seg_w, seg_h);
            lv_obj_align(bat_seg_[i], LV_ALIGN_LEFT_MID, 2 + i * (seg_w + 2), 0);
            lv_obj_set_style_radius(bat_seg_[i], 2, 0);
            lv_obj_set_style_bg_opa(bat_seg_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bat_seg_[i], lv_color_hex(0x5A5A5A), 0);
            lv_obj_remove_flag(bat_seg_[i], LV_OBJ_FLAG_CLICKABLE);
        }
        bat_label_ = lv_label_create(bat_box_);
        lv_obj_set_style_text_color(bat_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(bat_label_, "--%");   // "--%" until the DevKit sends data
        lv_obj_set_style_transform_pivot_x(bat_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(bat_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_scale(bat_label_, 160, 0);   // 256 = 100%
        lv_obj_center(bat_label_);
        // Menu button, bottom-right: a tap opens the Rubi menu (no more press-and-hold).
        menu_btn_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(menu_btn_);
        lv_obj_set_size(menu_btn_, 36, 36);
        lv_obj_align(menu_btn_, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
        lv_obj_set_style_radius(menu_btn_, 18, 0);
        lv_obj_set_style_bg_opa(menu_btn_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(menu_btn_, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_border_width(menu_btn_, 2, 0);
        lv_obj_set_style_border_color(menu_btn_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_remove_flag(menu_btn_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(menu_btn_, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 3; i++) {
            lv_obj_t* bar = lv_obj_create(menu_btn_);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 18, 3);
            lv_obj_align(bar, LV_ALIGN_CENTER, 0, (i - 1) * 7);
            lv_obj_set_style_radius(bar, 2, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
        bat_nub_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(bat_nub_);
        lv_obj_set_size(bat_nub_, 4, bat_h_ / 2);
        lv_obj_align(bat_nub_, LV_ALIGN_TOP_RIGHT, -7, 4 + (bat_h_ - bat_h_ / 2) / 2);   // fixed position: align_to ran before layout and put the nub in the wrong place
        lv_obj_set_style_radius(bat_nub_, 2, 0);
        // dark like the box fill: the nub used to be white on the white face background and could not be seen
        lv_obj_set_style_bg_opa(bat_nub_, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(bat_nub_, lv_color_hex(0x202020), 0);
        if (battery_label_ != nullptr) lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);   // use our own icon only
        RubiBattery::Start([this](int pct) { SetBatteryPercent(pct); });
        RubiWifiSync::Start();
    }

    // Permanent battery percentage, top-right corner (always visible, also in the menu).
    void SetBatteryPercent(int pct) {
        DisplayLockGuard lock(this);
        if (bat_label_ == nullptr || bat_box_ == nullptr) return;
        int lit = 0;
        uint32_t col = 0x5A5A5A;
        if (pct < 0) {
            lv_label_set_text(bat_label_, "--%");
        } else {
            char t[12];
            snprintf(t, sizeof(t), "%d%%", pct);
            lv_label_set_text(bat_label_, t);
            lit = pct > 75 ? 4 : (pct > 50 ? 3 : (pct > 25 ? 2 : (pct > 2 ? 1 : 0)));
            col = pct > 50 ? 0x2FBF3F : (pct > 25 ? 0xFFC21A : (pct > 10 ? 0xFF7A00 : 0xE53935));
        }
        for (int i = 0; i < 4; i++) {
            if (bat_seg_[i] == nullptr) continue;
            lv_obj_set_style_bg_color(bat_seg_[i], lv_color_hex(i < lit ? col : 0x5A5A5A), 0);
        }
        lv_obj_center(bat_label_);
    }

    // No scrolling subtitles for what the child or Rubi says. "system" messages (activation code,
    // Wi-Fi setup, errors, upgrade) still show because the parents need them.
    void SetChatMessage(const char* role, const char* content) override {
        if (role != nullptr && strcmp(role, "system") == 0) {
            SpiLcdDisplay::SetChatMessage(role, content);
        }
    }

    // Hide the "Listening / Speaking / Standby" status words; every other status stays.
    void SetStatus(const char* status) override {
        if (status != nullptr && (strcmp(status, Lang::Strings::LISTENING) == 0 ||
                                  strcmp(status, Lang::Strings::SPEAKING) == 0 ||
                                  strcmp(status, Lang::Strings::STANDBY) == 0)) {
            SpiLcdDisplay::SetStatus("");
            return;
        }
        SpiLcdDisplay::SetStatus(status);
    }

    // Photo faces: 20 pictures built into the firmware (RLE). Only the one on screen is decoded
    // (into one of two 150 KB PSRAM buffers), so the memory use stays small. If the buffers cannot be
    // allocated, the drawn face is used instead.
    static constexpr int kPhotoCount = 20;
    bool InitPhoto() {
        for (int i = 0; i < 2; i++) {
            photo_buf_[i] = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (photo_buf_[i] == nullptr) return false;
            lv_image_dsc_t& d = photo_dsc_[i];
            memset(&d, 0, sizeof(d));
            d.header.magic = LV_IMAGE_HEADER_MAGIC;
            d.header.cf = LV_COLOR_FORMAT_RGB565;
            d.header.w = 320;
            d.header.h = 240;
            d.header.stride = 640;
            d.data_size = 320 * 240 * 2;
            d.data = reinterpret_cast<const uint8_t*>(photo_buf_[i]);
        }
        auto screen = lv_screen_active();
        photo_ = lv_image_create(screen);
        lv_obj_set_size(photo_, 320, 240);
        lv_obj_align(photo_, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_remove_flag(photo_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(photo_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);   // no scroll bar when the picture bobs
        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        lv_obj_move_background(photo_);
        lv_obj_move_background(face_);   // drawn face at the very back, hidden
        lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);
        // Park the stock emoji inside the hidden drawn face so nothing of it can ever show again.
        if (emoji_box_) lv_obj_set_parent(emoji_box_, face_);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
        lv_obj_t* dark_text[] = {network_label_, mute_label_, battery_label_, status_label_, notification_label_};
        for (auto* t : dark_text) {
            if (t) lv_obj_set_style_text_color(t, lv_color_hex(0x333333), 0);
        }
        photo_on_ = true;
        return true;
    }

    void SetEmotion(const char* emotion) override {
        if (face_ == nullptr) {
            SpiLcdDisplay::SetEmotion(emotion);
            return;
        }
        DisplayLockGuard lock(this);
        Apply(Parse(emotion));
    }

    // ---- Rubi menu overlay (icon tiles, music list, calibration). Drawn on lv_layer_top(). ----
    struct MenuTile {
        const lv_image_dsc_t* icon;
        std::string label;
    };
    static constexpr int kMaxTiles = 6;
    static constexpr int kMenuRows = 5;

    // Hit tests in screen coordinates (320x240). Return the index or -1.
    // Touch area of the menu button (screen coordinates, 320x240 landscape); a bit bigger than the drawn button.
    static bool InMenuButton(int x, int y) { return x >= 258 && y >= 178; }

    static int TileAt(int x, int y, int count) {
        for (int i = 0; i < count && i < kMaxTiles; i++) {
            int x0 = 8 + (i % 3) * 102, y0 = 4 + (i / 3) * 118;
            if (x >= x0 && x < x0 + 100 && y >= y0 && y < y0 + 114) return i;
        }
        return -1;
    }
    static int RowAt(int x, int y, int count) {
        for (int i = 0; i < count && i < kMenuRows; i++) {
            int y0 = 40 + i * 38;
            if (x >= 8 && x < 312 && y >= y0 && y < y0 + 36) return i;
        }
        return -1;
    }

    // Learning card: picture on the left, English and Vietnamese words on the right, 3 buttons below.
    // CardAt: 0 = previous, 1 = back, 2 = next, 3 = picture/words area (tap to hear), -1 = nothing.
    static int CardAt(int x, int y) {
        // generous hit zones: the whole bottom strip is split into three, with no gaps
        if (y >= 176) {
            if (x < 107) return 0;
            if (x < 214) return 1;
            return 2;
        }
        if (y >= 30) return 3;
        return -1;
    }

    // Picture-book page: the whole screen is the picture, with three faint round buttons on top.
    // StoryAt: 0 = previous page, 1 = close, 2 = next page, 3 = anywhere else (replay the page).
    static int StoryAt(int x, int y) {
        if (x >= 240 && y < 70) return 1;
        if (x < 107) return 0;
        if (x >= 213) return 2;
        return 3;
    }

    void MenuShowStoryPage(const lv_image_dsc_t* img) {
        DisplayLockGuard lock(this);
        EnsureMenu();
        MenuClearAll();
        if (img != nullptr) {
            lv_image_set_src(story_img_, img);
            lv_obj_clear_flag(story_img_, LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < 3; i++) lv_obj_clear_flag(story_btn_[i], LV_OBJ_FLAG_HIDDEN);
        ShowMenuLayer();
    }

    void MenuShowCard(const lv_image_dsc_t* img, const std::string& title, const std::string& en,
                      const std::string& vi, const std::string& counter) {
        DisplayLockGuard lock(this);
        EnsureMenu();
        MenuClearAll();
        lv_label_set_text(menu_title_, title.c_str());
        lv_obj_clear_flag(menu_title_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(card_count_, counter.c_str());
        lv_obj_clear_flag(card_count_, LV_OBJ_FLAG_HIDDEN);
        if (img != nullptr) {
            lv_image_set_src(card_img_, img);
            lv_obj_clear_flag(card_img_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(card_en_, en.c_str());
        lv_label_set_text(card_vi_, vi.c_str());
        lv_obj_clear_flag(card_en_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card_vi_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) lv_obj_clear_flag(card_btn_[i], LV_OBJ_FLAG_HIDDEN);
        ShowMenuLayer();
    }

    void MenuShowTiles(const std::vector<MenuTile>& tiles) {
        DisplayLockGuard lock(this);
        EnsureMenu();
        MenuClearAll();
        for (int i = 0; i < kMaxTiles; i++) {
            if (i < (int)tiles.size()) {
                if (tiles[i].icon != nullptr) {
                    lv_image_set_src(tile_img_[i], tiles[i].icon);
                    lv_obj_clear_flag(tile_img_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_bg_opa(tile_box_[i], LV_OPA_TRANSP, 0);
                    lv_obj_align(tile_lbl_[i], LV_ALIGN_TOP_MID, 0, 92);
                    // no label: the icon alone, centred in the tile
                    lv_obj_align(tile_img_[i], LV_ALIGN_TOP_MID, 0, tiles[i].label.empty() ? 12 : 0);
                } else {
                    // text-only tile (no icon drawn yet): soft blue card with the label centred
                    lv_obj_add_flag(tile_img_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_bg_opa(tile_box_[i], LV_OPA_COVER, 0);
                    lv_obj_set_style_bg_color(tile_box_[i], lv_color_hex(0xE3F2FD), 0);
                    lv_obj_set_style_radius(tile_box_[i], 14, 0);
                    lv_obj_align(tile_lbl_[i], LV_ALIGN_CENTER, 0, 0);
                }
                lv_label_set_text(tile_lbl_[i], tiles[i].label.c_str());
                if (tiles[i].label.empty()) lv_obj_add_flag(tile_lbl_[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_clear_flag(tile_lbl_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(tile_box_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        ShowMenuLayer();
    }

    void MenuShowList(const char* title, const std::vector<std::string>& rows, const char* hint, const std::vector<bool>* locks = nullptr) {
        DisplayLockGuard lock(this);
        EnsureMenu();
        MenuClearAll();
        lv_label_set_text(menu_title_, title);
        lv_obj_clear_flag(menu_title_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(menu_hint_, hint);
        lv_obj_clear_flag(menu_hint_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < kMenuRows && i < (int)rows.size(); i++) {
            lv_label_set_text(menu_text_[i], rows[i].c_str());
            lv_obj_clear_flag(menu_row_[i], LV_OBJ_FLAG_HIDDEN);
            bool lk = locks != nullptr && i < (int)locks->size() && (*locks)[i];
            if (lk) { lv_obj_clear_flag(lock_body_[i], LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(lock_arc_[i], LV_OBJ_FLAG_HIDDEN); }
            else { lv_obj_add_flag(lock_body_[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(lock_arc_[i], LV_OBJ_FLAG_HIDDEN); }
        }
        ShowMenuLayer();
    }

    // A message in the middle; cross_x >= 0 also draws a red + target at (cross_x, cross_y).
    void MenuShowMessage(const char* msg, int cross_x, int cross_y) {
        DisplayLockGuard lock(this);
        EnsureMenu();
        MenuClearAll();
        lv_label_set_text(menu_msg_, msg);
        lv_obj_clear_flag(menu_msg_, LV_OBJ_FLAG_HIDDEN);
        if (cross_x >= 0) {
            lv_obj_set_pos(cross_h_, cross_x - 16, cross_y - 2);
            lv_obj_set_pos(cross_v_, cross_x - 2, cross_y - 16);
            lv_obj_clear_flag(cross_h_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cross_v_, LV_OBJ_FLAG_HIDDEN);
        }
        ShowMenuLayer();
    }

    struct KbKey {
        int x, y, w, h;
        std::string label;
        int kind;          // 0 character, 1 shift, 2 backspace, 3 layer switch, 4 space, 5 OK, 6 cancel
        std::string val;   // character to type (kind 0)
        uint32_t color;
    };

    void MenuShowKeyboard(const std::string& note, const std::string& field, const std::vector<KbKey>& keys, bool pw_visible = false) {
        DisplayLockGuard lock(this);
        EnsureMenu();
        MenuClearAll();
        lv_label_set_text(kb_ssid_, note.c_str());
        lv_obj_clear_flag(kb_ssid_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(kb_field_lbl_, field.c_str());
        lv_obj_clear_flag(kb_field_, LV_OBJ_FLAG_HIDDEN);
        if (pw_visible) lv_obj_add_flag(eye_slash_, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(eye_slash_, LV_OBJ_FLAG_HIDDEN);   // slashed eye = password hidden
        for (int i = 0; i < kMaxKeys && i < (int)keys.size(); i++) {
            const KbKey& k = keys[i];
            lv_obj_set_pos(kb_key_[i], k.x, k.y);
            lv_obj_set_size(kb_key_[i], k.w, k.h);
            lv_obj_set_style_bg_color(kb_key_[i], lv_color_hex(k.color), 0);
            lv_label_set_text(kb_lbl_[i], k.label.c_str());
            lv_obj_center(kb_lbl_[i]);
            lv_obj_clear_flag(kb_key_[i], LV_OBJ_FLAG_HIDDEN);
        }
        ShowMenuLayer();
    }

    void MenuHide() {
        DisplayLockGuard lock(this);
        if (menu_ != nullptr) {
            lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        }
    }

private:
    lv_obj_t* menu_ = nullptr;
    lv_obj_t* menu_title_ = nullptr;
    lv_obj_t* menu_hint_ = nullptr;
    lv_obj_t* menu_msg_ = nullptr;
    lv_obj_t* cross_h_ = nullptr;
    lv_obj_t* cross_v_ = nullptr;
    lv_obj_t* menu_row_[kMenuRows] = {};
    lv_obj_t* menu_text_[kMenuRows] = {};
    lv_obj_t* lock_body_[kMenuRows] = {};   // padlock drawn at the right end of a row (secured Wi-Fi)
    lv_obj_t* lock_arc_[kMenuRows] = {};
    lv_obj_t* tile_box_[kMaxTiles] = {};
    lv_obj_t* tile_img_[kMaxTiles] = {};
    lv_obj_t* tile_lbl_[kMaxTiles] = {};
    lv_obj_t* card_img_ = nullptr;
    lv_obj_t* card_en_ = nullptr;
    lv_obj_t* card_vi_ = nullptr;
    lv_obj_t* card_count_ = nullptr;
    lv_obj_t* card_btn_[3] = {};
    lv_obj_t* story_img_ = nullptr;
    lv_obj_t* story_btn_[3] = {};
    static constexpr int kMaxKeys = 36;
    lv_obj_t* kb_key_[kMaxKeys] = {};
    lv_obj_t* kb_lbl_[kMaxKeys] = {};
    lv_obj_t* kb_ssid_ = nullptr;
    lv_obj_t* kb_field_ = nullptr;
    lv_obj_t* kb_field_lbl_ = nullptr;
    lv_obj_t* eye_out_ = nullptr;
    lv_obj_t* eye_pupil_ = nullptr;
    lv_obj_t* eye_slash_ = nullptr;

    void ShowMenuLayer() {
        lv_obj_clear_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(menu_);
    }

    void MenuClearAll() {
        for (int i = 0; i < kMaxTiles; i++) lv_obj_add_flag(tile_box_[i], LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < kMenuRows; i++) lv_obj_add_flag(menu_row_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_title_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_hint_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_msg_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cross_h_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cross_v_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_en_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_vi_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_count_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) lv_obj_add_flag(card_btn_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(story_img_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) lv_obj_add_flag(story_btn_[i], LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < kMaxKeys; i++) if (kb_key_[i]) lv_obj_add_flag(kb_key_[i], LV_OBJ_FLAG_HIDDEN);
        if (kb_ssid_) lv_obj_add_flag(kb_ssid_, LV_OBJ_FLAG_HIDDEN);
        if (kb_field_) lv_obj_add_flag(kb_field_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
        lv_obj_t* l = lv_label_create(parent);
        if (font) lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        return l;
    }

    lv_obj_t* MakeRect(lv_obj_t* parent, int w, int h, uint32_t color, int radius) {
        lv_obj_t* r = lv_obj_create(parent);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, w, h);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
        lv_obj_set_style_radius(r, radius, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        return r;
    }

    void EnsureMenu() {
        if (menu_ != nullptr) {
            return;
        }
        const lv_font_t* font = &rubi_vi_font;
        menu_ = MakeRect(lv_layer_top(), LV_HOR_RES, LV_VER_RES, 0xFFFFFF, 0);
        lv_obj_align(menu_, LV_ALIGN_TOP_LEFT, 0, 0);
        for (int i = 0; i < kMaxTiles; i++) {
            tile_box_[i] = lv_obj_create(menu_);
            lv_obj_remove_style_all(tile_box_[i]);
            lv_obj_set_size(tile_box_[i], 100, 112);
            lv_obj_set_pos(tile_box_[i], 8 + (i % 3) * 102, 4 + (i / 3) * 118);
            lv_obj_clear_flag(tile_box_[i], LV_OBJ_FLAG_SCROLLABLE);
            tile_img_[i] = lv_image_create(tile_box_[i]);
            lv_obj_align(tile_img_[i], LV_ALIGN_TOP_MID, 0, 0);
            tile_lbl_[i] = MakeLabel(tile_box_[i], font, 0x1E2A3A);
            lv_obj_set_width(tile_lbl_[i], 100);
            lv_obj_set_style_text_align(tile_lbl_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(tile_lbl_[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(tile_lbl_[i], LV_ALIGN_TOP_MID, 0, 92);
        }
        menu_title_ = MakeLabel(menu_, font, 0xD81B60);
        lv_obj_align(menu_title_, LV_ALIGN_TOP_MID, 0, 10);
        for (int i = 0; i < kMenuRows; i++) {
            menu_row_[i] = MakeRect(menu_, 304, 36, 0xE6F0FF, 8);
            lv_obj_set_pos(menu_row_[i], 8, 40 + i * 38);
            menu_text_[i] = MakeLabel(menu_row_[i], font, 0x1E2A3A);
            lv_label_set_long_mode(menu_text_[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_width(menu_text_[i], 262);
            lv_obj_align(menu_text_[i], LV_ALIGN_LEFT_MID, 10, 0);
            lock_arc_[i] = MakeRect(menu_row_[i], 10, 11, 0xE6F0FF, 5);
            lv_obj_set_pos(lock_arc_[i], 284, 5);
            lv_obj_set_style_border_width(lock_arc_[i], 2, 0);
            lv_obj_set_style_border_color(lock_arc_[i], lv_color_hex(0x37474F), 0);
            lock_body_[i] = MakeRect(menu_row_[i], 16, 12, 0x37474F, 3);
            lv_obj_set_pos(lock_body_[i], 281, 15);
            lv_obj_add_flag(lock_arc_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lock_body_[i], LV_OBJ_FLAG_HIDDEN);
        }
        menu_hint_ = MakeLabel(menu_, font, 0x5A6B80);
        lv_obj_align(menu_hint_, LV_ALIGN_BOTTOM_MID, 0, -2);
        menu_msg_ = MakeLabel(menu_, font, 0x1E2A3A);
        lv_obj_set_width(menu_msg_, 280);
        lv_obj_set_style_text_align(menu_msg_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(menu_msg_, LV_ALIGN_CENTER, 0, 0);
        cross_h_ = MakeRect(menu_, 32, 4, 0xE53935, 2);
        cross_v_ = MakeRect(menu_, 4, 32, 0xE53935, 2);
        card_img_ = lv_image_create(menu_);
        lv_obj_set_pos(card_img_, 10, 40);
        card_en_ = MakeLabel(menu_, font, 0x1565C0);
        lv_label_set_long_mode(card_en_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(card_en_, 150);
        lv_obj_set_pos(card_en_, 160, 70);
        card_vi_ = MakeLabel(menu_, font, 0xD81B60);
        lv_label_set_long_mode(card_vi_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(card_vi_, 150);
        lv_obj_set_pos(card_vi_, 160, 120);
        card_count_ = MakeLabel(menu_, font, 0x5A6B80);
        lv_obj_align(card_count_, LV_ALIGN_TOP_RIGHT, -8, 10);
        static const char* kBtn[3] = {"< Trước", "Về", "Sau >"};
        static const int kBtnX[3] = {8, 111, 214};
        for (int i = 0; i < 3; i++) {
            card_btn_[i] = MakeRect(menu_, 98, 44, i == 1 ? 0xFFE0E0 : 0xE6F0FF, 10);
            lv_obj_set_pos(card_btn_[i], kBtnX[i], 192);
            lv_obj_t* l = MakeLabel(card_btn_[i], font, 0x1E2A3A);
            lv_label_set_text(l, kBtn[i]);
            lv_obj_center(l);
        }
        // Picture-book page: full-screen picture + faint round buttons (previous, close, next)
        story_img_ = lv_image_create(menu_);
        lv_obj_set_pos(story_img_, 0, 0);
        static const char* kStoryBtn[3] = {"<", "X", ">"};
        static const int kStoryBtnX[3] = {4, 276, 276};
        static const int kStoryBtnY[3] = {100, 4, 100};
        for (int i = 0; i < 3; i++) {
            story_btn_[i] = MakeRect(menu_, 40, 40, 0x000000, 20);
            lv_obj_set_style_bg_opa(story_btn_[i], 90, 0);
            lv_obj_set_pos(story_btn_[i], kStoryBtnX[i], kStoryBtnY[i]);
            lv_obj_t* l = MakeLabel(story_btn_[i], font, 0xFFFFFF);
            lv_label_set_text(l, kStoryBtn[i]);
            lv_obj_center(l);
        }
        // On-screen keyboard (used by the Wi-Fi password screen)
        kb_ssid_ = MakeLabel(menu_, font, 0xD81B60);
        lv_label_set_long_mode(kb_ssid_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(kb_ssid_, 240);
        lv_obj_set_pos(kb_ssid_, 74, 4);
        kb_field_ = MakeRect(menu_, 308, 26, 0xFFFFFF, 6);
        lv_obj_set_pos(kb_field_, 6, 26);
        lv_obj_set_style_border_width(kb_field_, 2, 0);
        lv_obj_set_style_border_color(kb_field_, lv_color_hex(0x1E88E5), 0);
        kb_field_lbl_ = MakeLabel(kb_field_, font, 0x1E2A3A);
        lv_label_set_long_mode(kb_field_lbl_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(kb_field_lbl_, 262);
        lv_obj_align(kb_field_lbl_, LV_ALIGN_LEFT_MID, 4, 0);
        // eye icon at the right end of the field: tap the field to show / hide the password
        eye_out_ = MakeRect(kb_field_, 26, 16, 0xFFFFFF, 8);
        lv_obj_set_pos(eye_out_, 274, 3);
        lv_obj_set_style_border_width(eye_out_, 2, 0);
        lv_obj_set_style_border_color(eye_out_, lv_color_hex(0x37474F), 0);
        eye_pupil_ = MakeRect(kb_field_, 8, 8, 0x1E88E5, 4);
        lv_obj_set_pos(eye_pupil_, 283, 7);
        eye_slash_ = MakeRect(kb_field_, 32, 3, 0xE53935, 1);
        lv_obj_set_pos(eye_slash_, 271, 10);
        lv_obj_set_style_transform_pivot_x(eye_slash_, 16, 0);
        lv_obj_set_style_transform_pivot_y(eye_slash_, 1, 0);
        lv_obj_set_style_transform_rotation(eye_slash_, -350, 0);
        for (int i = 0; i < kMaxKeys; i++) {
            kb_key_[i] = MakeRect(menu_, 28, 38, 0xE6F0FF, 6);
            kb_lbl_[i] = MakeLabel(kb_key_[i], font, 0x1E2A3A);
            lv_obj_center(kb_lbl_[i]);
        }
        MenuClearAll();
    }

    enum class Expr {
        kNeutral, kHappy, kLaughing, kWinking, kLoving, kSurprised, kCrying,
        kAngry, kRelaxed, kCool, kEmbarrassed, kSleepy, kThinking, kSilly
    };

    static constexpr int kPanelW = 300;
    static constexpr int kPanelH = 168;
    static constexpr int kEyeX = 72;
    static constexpr int kEyeY = -22;
    static constexpr uint32_t kWhite = 0xFFFFFF;
    static constexpr uint32_t kGlow = 0x5AA9FF;
    static constexpr uint32_t kBeak = 0xFFC61A;
    static constexpr uint32_t kBeakDark = 0xC98A00;
    static constexpr uint32_t kMouthRed = 0xE8283C;
    static constexpr uint32_t kTongue = 0xFF8A8A;
    static constexpr uint32_t kBlush = 0xFF4D5A;
    static constexpr uint32_t kTear = 0x8FD3FF;
    static constexpr uint32_t kLid = 0x0E2A4A;
    static constexpr uint32_t kSpark = 0xFFC61A;
    static constexpr uint32_t kAnger = 0xFF3B30;

    lv_obj_t* face_ = nullptr;
    lv_obj_t* photo_ = nullptr;
    lv_image_dsc_t photo_dsc_[2] = {};
    uint16_t* photo_buf_[2] = {};
    int photo_slot_ = 0;
    bool photo_on_ = false;
    int photo_cur_ = -1;
    int bob_tick_ = 0;
    Expr photo_expr_ = Expr::kNeutral;
    Expr pending_ = Expr::kNeutral;
    bool has_pending_ = false;
    int64_t photo_time_ = 0;
    lv_obj_t* eye_[2] = {};      // white eyeball (or dark lid for the "cool" face)
    lv_obj_t* pupil_[2] = {};
    lv_obj_t* shine_[2] = {};
    lv_obj_t* bar_[2] = {};      // half-closed lid line
    lv_obj_t* eyearc_[2] = {};   // "^" or "u" shaped closed eyes
    lv_obj_t* brow_[2] = {};     // eyebrow arc
    lv_obj_t* brow_bar_[2] = {}; // straight eyebrow (angry)
    lv_obj_t* blush_[2] = {};
    lv_obj_t* tear_[2] = {};
    lv_obj_t* spark_[6] = {};
    lv_obj_t* beak_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* tongue_ = nullptr;
    lv_obj_t* frown_ = nullptr;
    lv_obj_t* tag_ = nullptr;
    lv_obj_t* bat_label_ = nullptr;
    lv_obj_t* bat_box_ = nullptr;
    lv_obj_t* bat_seg_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* bat_nub_ = nullptr;
    lv_obj_t* menu_btn_ = nullptr;
    int bat_w_ = 46, bat_h_ = 22;
    lv_timer_t* timer_ = nullptr;

    Expr expr_ = Expr::kNeutral;
    bool round_eyes_ = false;
    int eye_d_ = 62;
    int blink_countdown_ = 30;
    int idle_countdown_ = 60;  // ticks of 100 ms until the next idle expression change
    int blink_phase_ = 0;
    bool speaking_ = false;

    static Expr Parse(const char* e) {
        if (e == nullptr) return Expr::kNeutral;
        auto is = [&](const char* s) { return strcmp(e, s) == 0; };
        if (is("happy") || is("delicious")) return Expr::kHappy;
        if (is("laughing") || is("funny")) return Expr::kLaughing;
        if (is("winking")) return Expr::kWinking;
        if (is("silly")) return Expr::kSilly;
        if (is("loving") || is("kissy")) return Expr::kLoving;
        if (is("surprised") || is("shocked")) return Expr::kSurprised;
        if (is("crying") || is("sad")) return Expr::kCrying;
        if (is("angry")) return Expr::kAngry;
        if (is("relaxed")) return Expr::kRelaxed;
        if (is("cool") || is("confident")) return Expr::kCool;
        if (is("embarrassed")) return Expr::kEmbarrassed;
        if (is("sleepy")) return Expr::kSleepy;
        if (is("thinking") || is("confused")) return Expr::kThinking;
        return Expr::kNeutral;
    }

    // ---------- small helpers ----------
    static lv_obj_t* Box(lv_obj_t* parent, uint32_t color) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    }

    static lv_obj_t* Arc(lv_obj_t* parent) {
        lv_obj_t* a = lv_arc_create(parent);
        lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        return a;
    }

    static void Glow(lv_obj_t* o) {
        lv_obj_set_style_shadow_width(o, 12, 0);
        lv_obj_set_style_shadow_color(o, lv_color_hex(kGlow), 0);
        lv_obj_set_style_shadow_opa(o, LV_OPA_50, 0);
    }

    static void Put(lv_obj_t* o, int x, int y, int w, int h, int rot = 0, int radius = LV_RADIUS_CIRCLE) {
        lv_obj_set_size(o, w, h);
        lv_obj_align(o, LV_ALIGN_CENTER, x, y);
        lv_obj_set_style_radius(o, radius, 0);
        lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
        lv_obj_set_style_transform_rotation(o, rot * 10, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    static void PutArc(lv_obj_t* a, int x, int y, int d, int start, int end, int width, uint32_t color) {
        lv_obj_set_size(a, d, d);
        lv_obj_align(a, LV_ALIGN_CENTER, x, y);
        lv_arc_set_bg_angles(a, start, end);
        lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_HIDDEN);
    }

    static void Hide(lv_obj_t* o) { if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

    int Sx(int i) const { return i == 0 ? -1 : 1; }

    void BuildFace() {
        if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        // Full-screen face: sits behind the status icons, which turn white on black.
        auto screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        if (container_) lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
        if (top_bar_) lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        if (content_) lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* white_text[] = {network_label_, mute_label_, battery_label_, status_label_, notification_label_};
        for (auto* t : white_text) {
            if (t) lv_obj_set_style_text_color(t, lv_color_white(), 0);
        }

        face_ = lv_obj_create(screen);
        lv_obj_remove_style_all(face_);
        lv_obj_set_size(face_, LV_HOR_RES, LV_VER_RES);
        lv_obj_align(face_, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_move_background(face_);
        lv_obj_set_style_bg_opa(face_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(face_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_radius(face_, 0, 0);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < 2; i++) {
            blush_[i] = Box(face_, kBlush);
            lv_obj_set_style_bg_opa(blush_[i], LV_OPA_70, 0);
        }
        beak_ = Box(face_, kBeak);
        mouth_ = Box(face_, kMouthRed);
        tongue_ = Box(face_, kTongue);
        frown_ = Arc(face_);
        for (int i = 0; i < 2; i++) tear_[i] = Box(face_, kTear);
        for (int i = 0; i < 2; i++) {
            eye_[i] = Box(face_, kWhite);
            bar_[i] = Box(face_, kWhite);
            pupil_[i] = Box(face_, 0x000000);
            shine_[i] = Box(face_, kWhite);
            eyearc_[i] = Arc(face_);
            brow_[i] = Arc(face_);
            brow_bar_[i] = Box(face_, kWhite);
        }
        for (auto& s : spark_) s = Box(face_, kSpark);
        tag_ = lv_label_create(face_);
        lv_obj_set_style_text_color(tag_, lv_color_hex(kWhite), 0);
        lv_label_set_text(tag_, "");
    }

    void HideAll() {
        for (int i = 0; i < 2; i++) {
            Hide(eye_[i]); Hide(bar_[i]); Hide(pupil_[i]); Hide(shine_[i]); Hide(eyearc_[i]);
            Hide(brow_[i]); Hide(brow_bar_[i]); Hide(blush_[i]); Hide(tear_[i]);
        }
        for (auto s : spark_) Hide(s);
        Hide(beak_); Hide(mouth_); Hide(tongue_); Hide(frown_);
        lv_label_set_text(tag_, "");
        lv_obj_set_style_text_color(tag_, lv_color_hex(kWhite), 0);
    }

    // ---------- face parts ----------
    // Eyeball with pupil. look_x/look_y move the pupil, ratio = pupil size in percent.
    void Eye(int i, int d, int look_x = 0, int look_y = 0, int ratio = 66, int dy = 0) {
        int x = Sx(i) * kEyeX, y = kEyeY + dy;
        Put(eye_[i], x, y, d, d);
        Glow(eye_[i]);
        lv_obj_set_style_bg_color(eye_[i], lv_color_hex(kWhite), 0);
        int p = d * ratio / 100;
        Put(pupil_[i], x + look_x, y + look_y, p, p);
        int s = p / 4 < 6 ? 6 : p / 4;
        Put(shine_[i], x + look_x + p / 5, y + look_y - p / 4, s, s);
        round_eyes_ = true;
        eye_d_ = d;
    }

    void ClosedArc(int i, int d, bool smile_up, int width = 10, int dy = 0) {
        int x = Sx(i) * kEyeX, y = kEyeY + 14 + dy;
        if (smile_up) PutArc(eyearc_[i], x, y, d, 200, 340, width, kWhite);
        else PutArc(eyearc_[i], x, y - 14, d, 25, 155, width, kWhite);
        Glow(eyearc_[i]);
        round_eyes_ = false;
    }

    void Brow(int i, int rot = 0, int dy = 0, int dx = 0) {
        int x = Sx(i) * (kEyeX + dx), y = kEyeY - 40 + dy;
        PutArc(brow_[i], x, y, 46, 215, 325, 5, kWhite);
        lv_obj_set_style_transform_pivot_x(brow_[i], 23, 0);
        lv_obj_set_style_transform_pivot_y(brow_[i], 23, 0);
        lv_obj_set_style_transform_rotation(brow_[i], Sx(i) * rot * 10, 0);
    }

    void Cheeks(int a_percent = 70) {
        for (int i = 0; i < 2; i++) {
            Put(blush_[i], Sx(i) * (kEyeX + 38), 36, 40, 24);
            lv_obj_set_style_bg_opa(blush_[i], a_percent * 255 / 100, 0);
        }
    }

    // Open beak (smile): width w, beak height h.
    void BeakOpen(int w, int h, int y = 30) {
        Put(beak_, 0, y, w, h, 0, h / 2);
        lv_obj_set_style_bg_color(beak_, lv_color_hex(kBeak), 0);
        int mh = h - 14 > 8 ? h - 14 : 8;
        Put(mouth_, 0, y + 3, w - 22, mh, 0, LV_RADIUS_CIRCLE);
        if (mh > 16) Put(tongue_, 0, y + 3 + mh / 2 - 4, (w - 22) * 6 / 10, 8, 0, LV_RADIUS_CIRCLE);
    }

    // Closed beak with a little frown line.
    void BeakFrown(int w = 54, int h = 28, int y = 30) {
        Put(beak_, 0, y, w, h, 0, h / 2);
        lv_obj_set_style_bg_color(beak_, lv_color_hex(kBeak), 0);
        PutArc(frown_, 0, y + h / 2 + 4, 26, 215, 325, 3, kBeakDark);
    }

    // Beak opened like an "o" (surprised, yawning).
    void BeakO(int w, int h, int y = 34) {
        Put(beak_, 0, y, w, h, 0, LV_RADIUS_CIRCLE);
        Put(mouth_, 0, y + 1, w - 10, h - 12, 0, LV_RADIUS_CIRCLE);
    }

    // Which pictures (0-based, from the user's 5x4 sheet) fit each expression; one is picked at random.
    void ShowPhoto(Expr e) {
        // Same mood again (or changed less than 2.5 s ago): keep the current picture, so the face never flickers.
        int64_t now = lv_tick_get();
        if (photo_cur_ >= 0 && (e == photo_expr_ || now - photo_time_ < 2500)) {
            if (e != photo_expr_) pending_ = e, has_pending_ = true;
            return;
        }
        photo_expr_ = e;
        photo_time_ = now;
        has_pending_ = false;
        static const int8_t kMap[][4] = {
            /*Neutral*/ {0, -1}, /*Happy*/ {2, 3, 4, -1}, /*Laughing*/ {7, 19, -1},
            /*Winking*/ {1, 6, 15, -1}, /*Loving*/ {8, 9, -1}, /*Surprised*/ {5, -1},
            /*Crying*/ {10, 11, -1}, /*Angry*/ {12, -1}, /*Relaxed*/ {3, 17, -1}, /*Cool*/ {18, -1},
            /*Embarrassed*/ {3, -1}, /*Sleepy*/ {13, -1}, /*Thinking*/ {14, -1}, /*Silly*/ {16, -1}};
        const int8_t* list = kMap[static_cast<int>(e)];
        int n = 0;
        while (list[n] >= 0) n++;
        int pick = list[esp_random() % n];
        if (n > 1 && pick == photo_cur_) {
            for (int i = 0; i < n; i++) {
                if (list[i] == pick) { pick = list[(i + 1) % n]; break; }
            }
        }
        photo_cur_ = pick;
        int slot = 1 - photo_slot_;
        uint16_t* dst = photo_buf_[slot];
        RubiDecodePal(kRubiMatData[pick], kRubiMatSize[pick], dst, 320 * 240);
        photo_slot_ = slot;
        lv_image_set_src(photo_, &photo_dsc_[slot]);
        lv_obj_set_style_translate_y(photo_, 0, 0);
    }

    void Apply(Expr e) {
        expr_ = e;
        speaking_ = false;
        if (photo_on_) {
            ShowPhoto(e);
            return;
        }
        HideAll();
        round_eyes_ = false;
        switch (e) {
            case Expr::kNeutral:
                Eye(0, 62); Eye(1, 62);
                Brow(0); Brow(1);
                BeakOpen(54, 32);
                Cheeks();
                break;
            case Expr::kHappy:
                ClosedArc(0, 64, true); ClosedArc(1, 64, true);
                Brow(0); Brow(1);
                BeakOpen(56, 34);
                Cheeks();
                break;
            case Expr::kLaughing:
                ClosedArc(0, 78, true, 11); ClosedArc(1, 78, true, 11);
                Brow(0, 0, -2); Brow(1, 0, -2);
                BeakOpen(66, 42);
                Cheeks();
                break;
            case Expr::kWinking:
            case Expr::kSilly:
                Eye(0, 62, -4, 0); ClosedArc(1, 58, true);
                Brow(0); Brow(1, 0, 6);
                BeakOpen(54, 32);
                Cheeks();
                break;
            case Expr::kLoving:
                Eye(0, 62, -4, 0);
                // the right eye is a "<" wink
                Put(spark_[0], kEyeX + 6, kEyeY - 10, 36, 8, 28, 4); lv_obj_set_style_bg_color(spark_[0], lv_color_hex(kWhite), 0);
                Put(spark_[1], kEyeX + 6, kEyeY + 10, 36, 8, -28, 4); lv_obj_set_style_bg_color(spark_[1], lv_color_hex(kWhite), 0);
                Brow(0); Brow(1, 0, 6);
                BeakOpen(54, 32);
                Cheeks();
                break;
            case Expr::kSurprised:
                Eye(0, 70, 0, 0, 50, -2); Eye(1, 70, 0, 0, 50, -2);
                Brow(0, 0, -6); Brow(1, 0, -6);
                for (int k = 0; k < 3; k++) {
                    int a = (k - 1) * 22;
                    Put(spark_[k], -kEyeX - 56, kEyeY + a * 7 / 10, 20, 5, -a, 2);
                    Put(spark_[3 + k], kEyeX + 56, kEyeY + a * 7 / 10, 20, 5, a, 2);
                }
                BeakO(26, 44, 36);
                Cheeks();
                break;
            case Expr::kCrying:
                Eye(0, 62, 0, 4, 78); Eye(1, 62, 0, 4, 78);
                Brow(0, 14, 4); Brow(1, 14, 4);
                for (int i = 0; i < 2; i++) Put(tear_[i], Sx(i) * kEyeX, kEyeY + 34, 46, 16, 0, 8);
                BeakFrown();
                Cheeks();
                break;
            case Expr::kAngry:
                Eye(0, 58, 8, 4, 62); Eye(1, 58, -8, 4, 62);
                for (int i = 0; i < 2; i++) {
                    Put(brow_bar_[i], Sx(i) * kEyeX, kEyeY - 24, 64, 10, Sx(i) * 20, 5);
                    Glow(brow_bar_[i]);
                }
                Put(spark_[0], 128, -62, 24, 6, 45, 3); lv_obj_set_style_bg_color(spark_[0], lv_color_hex(kAnger), 0);
                Put(spark_[1], 128, -62, 24, 6, -45, 3); lv_obj_set_style_bg_color(spark_[1], lv_color_hex(kAnger), 0);
                BeakFrown();
                Cheeks(50);
                break;
            case Expr::kRelaxed:
                ClosedArc(0, 64, false, 9, 4); ClosedArc(1, 64, false, 9, 4);
                Brow(0); Brow(1);
                BeakFrown(50, 26);
                Cheeks();
                break;
            case Expr::kCool:
                for (int i = 0; i < 2; i++) {
                    Put(eye_[i], Sx(i) * kEyeX, kEyeY, 62, 62);
                    lv_obj_set_style_bg_color(eye_[i], lv_color_hex(kLid), 0);
                    Put(bar_[i], Sx(i) * kEyeX, kEyeY - 6, 68, 10, 0, 5);
                    Glow(bar_[i]);
                }
                Brow(0, 0, 4); Brow(1, 0, 4);
                BeakOpen(56, 34);
                Cheeks();
                break;
            case Expr::kEmbarrassed:
                ClosedArc(0, 64, false, 9, 4); ClosedArc(1, 64, false, 9, 4);
                for (int i = 0; i < 2; i++) {
                    for (int k = 0; k < 2; k++) {
                        Put(spark_[i * 2 + k], Sx(i) * (kEyeX + 36), kEyeY + 6 + k * 10, 14, 4, Sx(i) * (k ? -25 : 10), 2);
                        lv_obj_set_style_bg_color(spark_[i * 2 + k], lv_color_hex(kWhite), 0);
                    }
                }
                Brow(0, 0, 6); Brow(1, 0, 6);
                BeakFrown(50, 26);
                Cheeks(95);
                break;
            case Expr::kSleepy:
                ClosedArc(0, 64, false, 8, 4); ClosedArc(1, 64, false, 8, 4);
                Brow(0, 0, 6); Brow(1, 0, 6);
                BeakO(24, 38, 36);
                lv_label_set_text(tag_, "Z z");
                lv_obj_align(tag_, LV_ALIGN_TOP_RIGHT, -12, 6);
                Cheeks();
                break;
            case Expr::kThinking:
                Eye(0, 62, 10, -12, 62); Eye(1, 62, 10, -12, 62);
                Brow(0, 0, 0); Brow(1, -8, -8);
                Put(beak_, 20, 34, 34, 20, 0, 10);
                lv_obj_set_style_bg_color(beak_, lv_color_hex(kBeak), 0);
                lv_label_set_text(tag_, "...");
                lv_obj_align(tag_, LV_ALIGN_TOP_RIGHT, -14, 8);
                Cheeks();
                break;
        }
    }

    // Eyes close briefly now and then (round eyes only).
    void Blink(bool closed) {
        if (!round_eyes_) return;
        for (int i = 0; i < 2; i++) {
            if (lv_obj_has_flag(eye_[i], LV_OBJ_FLAG_HIDDEN)) continue;
            if (closed) {
                Hide(pupil_[i]); Hide(shine_[i]);
                lv_obj_set_height(eye_[i], 8);
                lv_obj_align(eye_[i], LV_ALIGN_CENTER, Sx(i) * kEyeX, kEyeY + 10);
            } else {
                Apply(expr_);
                return;
            }
        }
    }

    static void TimerCb(lv_timer_t* t) {
        static_cast<RubiDisplay*>(lv_timer_get_user_data(t))->Tick();
    }

    void Tick() {
        if (face_ == nullptr) return;
        bool speak_now = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
        if (photo_on_) {
            // Speaking: the picture bobs up and down. Idle: a new mood every 8-16 s (never angry/crying).
            if (speak_now) {
                speaking_ = true;
                static const int8_t kBob[] = {0, -2, -4, -2, 0, 2, 3, 2};
                lv_obj_set_style_translate_y(photo_, kBob[(bob_tick_++) & 7], 0);
                return;
            }
            if (speaking_) {
                speaking_ = false;
                lv_obj_set_style_translate_y(photo_, 0, 0);
            }
            if (has_pending_) ShowPhoto(pending_);   // a mood that arrived during the 2.5 s hold
            if (face_ && !lv_obj_has_flag(face_, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                if (--idle_countdown_ <= 0) {
                    static const Expr kIdle[] = {
                        Expr::kNeutral, Expr::kHappy, Expr::kLaughing, Expr::kWinking, Expr::kLoving,
                        Expr::kRelaxed, Expr::kCool, Expr::kThinking, Expr::kSurprised,
                        Expr::kEmbarrassed, Expr::kSleepy, Expr::kSilly, Expr::kNeutral};
                    Expr next = expr_;
                    for (int i = 0; i < 6 && next == expr_; i++) {
                        next = kIdle[esp_random() % (sizeof(kIdle) / sizeof(kIdle[0]))];
                    }
                    Apply(next);
                    idle_countdown_ = 80 + (int)(esp_random() % 80);
                }
            } else {
                idle_countdown_ = 50;
            }
            return;
        }
        if (speak_now) {
            // lip-sync: the beak opens and closes at random while speech is playing
            if (!speaking_) {
                speaking_ = true;
                Hide(frown_); Hide(beak_);
            }
            int h = 18 + (int)(esp_random() % 28);
            int w = 46 + (int)(esp_random() % 22);
            BeakOpen(w, h, 30);
            return;
        }
        if (speaking_) {
            Apply(expr_);  // back to the resting mouth of the current expression
            return;
        }
        // While waiting (idle) Rubi keeps changing mood every 8-16 s. No angry/crying here, so
        // a small child is never upset by the idle face. Other states keep the server's emotion.
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            if (--idle_countdown_ <= 0 && blink_phase_ == 0) {
                static const Expr kIdle[] = {
                    Expr::kNeutral, Expr::kHappy, Expr::kLaughing, Expr::kWinking, Expr::kLoving,
                    Expr::kRelaxed, Expr::kCool, Expr::kThinking, Expr::kSurprised,
                    Expr::kEmbarrassed, Expr::kSleepy, Expr::kNeutral};
                Expr next = expr_;
                for (int i = 0; i < 6 && next == expr_; i++) {
                    next = kIdle[esp_random() % (sizeof(kIdle) / sizeof(kIdle[0]))];
                }
                Apply(next);
                idle_countdown_ = 80 + (int)(esp_random() % 80);
            }
        } else {
            idle_countdown_ = 50;
        }
        if (--blink_countdown_ <= 0) {
            if (blink_phase_ == 0) {
                Blink(true); blink_phase_ = 1; blink_countdown_ = 2;
            } else {
                Blink(false); blink_phase_ = 0; blink_countdown_ = 25 + (int)(esp_random() % 30);
            }
        }
    }
};

#endif  // RUBI_FACE_H
