#ifndef RUBI_FACE_H
#define RUBI_FACE_H

// Rubi's face: 13 expressions drawn with LVGL shapes, blinking, and a mouth that moves
// while the robot is speaking. Replaces the default emoji picture.

#include <cstring>
#include <esp_random.h>
#include <lvgl.h>

#include "application.h"
#include "display/lcd_display.h"

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
        Apply(Expr::kNeutral);
        timer_ = lv_timer_create(&RubiDisplay::TimerCb, 100, this);
    }

    void SetEmotion(const char* emotion) override {
        if (face_ == nullptr) {
            SpiLcdDisplay::SetEmotion(emotion);
            return;
        }
        DisplayLockGuard lock(this);
        Apply(Parse(emotion));
    }

private:
    enum class Expr {
        kNeutral, kHappy, kLaughing, kSad, kAngry, kCrying, kLoving,
        kSurprised, kThinking, kWinking, kSleepy, kConfused, kEmbarrassed
    };

    static constexpr int kPanelW = 300;
    static constexpr int kPanelH = 168;
    static constexpr uint32_t kCyan = 0x4DE1F0;
    static constexpr uint32_t kPink = 0xFF6FA5;
    static constexpr uint32_t kRed = 0xFF5252;
    static constexpr uint32_t kBlue = 0x6EC6FF;

    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {nullptr, nullptr};      // filled shapes
    lv_obj_t* eye_arc_[2] = {nullptr, nullptr};  // "^" shaped eyes
    lv_obj_t* mouth_arc_ = nullptr;              // smile / frown
    lv_obj_t* mouth_line_ = nullptr;
    lv_obj_t* mouth_open_ = nullptr;             // open mouth, used for speaking
    lv_obj_t* tear_[2] = {nullptr, nullptr};
    lv_obj_t* blush_[2] = {nullptr, nullptr};
    lv_obj_t* tag_ = nullptr;                    // "Z z" or "?"
    lv_timer_t* timer_ = nullptr;

    Expr expr_ = Expr::kNeutral;
    bool eyes_are_arcs_ = false;
    bool has_mouth_open_static_ = false;  // laughing / surprised / crying keep the mouth open
    int eye_w_ = 38, eye_h_ = 50;
    int eye_x_ = 62, eye_y_ = -22;
    int eye_h_right_ = 0;  // 0 = same as left
    int blink_countdown_ = 30;
    int blink_phase_ = 0;
    int speak_tick_ = 0;
    bool speaking_ = false;

    static Expr Parse(const char* e) {
        if (e == nullptr) return Expr::kNeutral;
        auto is = [&](const char* s) { return strcmp(e, s) == 0; };
        if (is("happy") || is("relaxed") || is("confident") || is("delicious") || is("cool")) return Expr::kHappy;
        if (is("laughing") || is("funny")) return Expr::kLaughing;
        if (is("sad")) return Expr::kSad;
        if (is("angry")) return Expr::kAngry;
        if (is("crying")) return Expr::kCrying;
        if (is("loving") || is("kissy")) return Expr::kLoving;
        if (is("surprised") || is("shocked")) return Expr::kSurprised;
        if (is("thinking")) return Expr::kThinking;
        if (is("winking") || is("silly")) return Expr::kWinking;
        if (is("sleepy")) return Expr::kSleepy;
        if (is("confused")) return Expr::kConfused;
        if (is("embarrassed")) return Expr::kEmbarrassed;
        return Expr::kNeutral;
    }

    static lv_obj_t* MakeBox(lv_obj_t* parent, uint32_t color) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        return o;
    }

    static lv_obj_t* MakeArc(lv_obj_t* parent) {
        lv_obj_t* a = lv_arc_create(parent);
        lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        return a;
    }

    static void Put(lv_obj_t* o, int x, int y, int w, int h, int rot_deg = 0, int radius = -1) {
        lv_obj_set_size(o, w, h);
        lv_obj_align(o, LV_ALIGN_CENTER, x, y);
        lv_obj_set_style_radius(o, radius < 0 ? LV_RADIUS_CIRCLE : radius, 0);
        lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
        lv_obj_set_style_transform_rotation(o, rot_deg * 10, 0);
    }

    static void PutArc(lv_obj_t* a, int x, int y, int d, int start, int end, int width, uint32_t color) {
        lv_obj_set_size(a, d, d);
        lv_obj_align(a, LV_ALIGN_CENTER, x, y);
        lv_arc_set_bg_angles(a, start, end);
        lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    }

    static void Show(lv_obj_t* o, bool v) {
        if (v) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    void BuildFace() {
        if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        face_ = MakeBox(emoji_box_, 0x000000);
        lv_obj_set_size(face_, kPanelW, kPanelH);
        lv_obj_set_style_radius(face_, 28, 0);
        for (int i = 0; i < 2; i++) {
            eye_[i] = MakeBox(face_, kCyan);
            eye_arc_[i] = MakeArc(face_);
            tear_[i] = MakeBox(face_, kBlue);
            blush_[i] = MakeBox(face_, kPink);
            lv_obj_set_style_bg_opa(blush_[i], LV_OPA_60, 0);
        }
        mouth_arc_ = MakeArc(face_);
        mouth_line_ = MakeBox(face_, kCyan);
        mouth_open_ = MakeBox(face_, kRed);
        tag_ = lv_label_create(face_);
        lv_obj_set_style_text_color(tag_, lv_color_hex(kCyan), 0);
        lv_label_set_text(tag_, "");
    }

    void HideAll() {
        for (int i = 0; i < 2; i++) {
            Show(eye_[i], false); Show(eye_arc_[i], false);
            Show(tear_[i], false); Show(blush_[i], false);
        }
        Show(mouth_arc_, false); Show(mouth_line_, false); Show(mouth_open_, false);
        lv_label_set_text(tag_, "");
    }

    // Place the two eyes. kind: 0 rounded box, 1 "^" arc
    void Eyes(int w, int h, int rot, int dy = 0, uint32_t color = kCyan, int radius = -1, int h_right = 0) {
        eyes_are_arcs_ = false;
        eye_w_ = w; eye_h_ = h; eye_h_right_ = h_right; eye_y_ = -22 + dy;
        for (int i = 0; i < 2; i++) {
            int sx = i == 0 ? -1 : 1;
            int hh = (i == 1 && h_right > 0) ? h_right : h;
            int yy = eye_y_ + ((i == 1 && h_right > 0) ? (h - h_right) / 2 : 0);
            lv_obj_set_style_bg_color(eye_[i], lv_color_hex(color), 0);
            Put(eye_[i], sx * eye_x_, yy, w, hh, sx * rot, radius);
            Show(eye_[i], true);
        }
    }

    void ArcEye(int i, int d, uint32_t color = kCyan) {
        int sx = i == 0 ? -1 : 1;
        PutArc(eye_arc_[i], sx * eye_x_, -4, d, 205, 335, 9, color);
        Show(eye_arc_[i], true);
    }

    void Smile(int d, int cy) { PutArc(mouth_arc_, 0, cy, d, 30, 150, 8, kCyan); Show(mouth_arc_, true); }
    void Frown(int d, int cy) { PutArc(mouth_arc_, 0, cy, d, 210, 330, 8, kCyan); Show(mouth_arc_, true); }
    void Line(int w, int x, int y, int rot = 0) {
        Put(mouth_line_, x, y, w, 7, rot, 3); Show(mouth_line_, true);
    }
    void OpenMouth(int w, int h, int y) {
        Put(mouth_open_, 0, y, w, h, 0, LV_RADIUS_CIRCLE); Show(mouth_open_, true);
    }

    void Apply(Expr e) {
        expr_ = e;
        HideAll();
        has_mouth_open_static_ = false;
        switch (e) {
            case Expr::kNeutral:
                Eyes(38, 50, 0, 0, kCyan, 14);
                Line(40, 0, 46);
                break;
            case Expr::kHappy:
                eyes_are_arcs_ = true; ArcEye(0, 60); ArcEye(1, 60);
                Smile(84, 16);
                break;
            case Expr::kLaughing:
                eyes_are_arcs_ = true; ArcEye(0, 60); ArcEye(1, 60);
                OpenMouth(80, 46, 44); has_mouth_open_static_ = true;
                break;
            case Expr::kSad:
                Eyes(34, 44, 14, 6, kCyan, 14);
                Frown(84, 88);
                break;
            case Expr::kAngry:
                Eyes(56, 28, 18, 4, kRed, 6);
                Line(46, 0, 50);
                break;
            case Expr::kCrying:
                Eyes(34, 44, 12, 6, kCyan, 14);
                OpenMouth(36, 24, 50); has_mouth_open_static_ = true;
                for (int i = 0; i < 2; i++) {
                    int sx = i == 0 ? -1 : 1;
                    Put(tear_[i], sx * (eye_x_ + 16), 26, 12, 30, 0, LV_RADIUS_CIRCLE);
                    Show(tear_[i], true);
                }
                break;
            case Expr::kLoving:
                Eyes(48, 48, 0, 0, kPink, LV_RADIUS_CIRCLE);
                Smile(84, 14);
                for (int i = 0; i < 2; i++) {
                    int sx = i == 0 ? -1 : 1;
                    Put(blush_[i], sx * (eye_x_ + 28), 26, 36, 20, 0, LV_RADIUS_CIRCLE);
                    Show(blush_[i], true);
                }
                break;
            case Expr::kSurprised:
                Eyes(54, 62, 0, -4, kCyan, LV_RADIUS_CIRCLE);
                OpenMouth(32, 40, 48); has_mouth_open_static_ = true;
                break;
            case Expr::kThinking:
                Eyes(32, 44, 0, -10, kCyan, 12, 34);
                Line(28, 34, 50, -10);
                lv_label_set_text(tag_, "...");
                lv_obj_align(tag_, LV_ALIGN_TOP_RIGHT, -14, 8);
                break;
            case Expr::kWinking:
                Eyes(38, 50, 0, 0, kCyan, 14);
                Show(eye_[1], false);
                ArcEye(1, 56);
                eyes_are_arcs_ = false;
                Smile(84, 14);
                break;
            case Expr::kSleepy:
                Eyes(48, 8, 0, 6, kCyan, 4);
                OpenMouth(22, 16, 50); has_mouth_open_static_ = true;
                lv_label_set_text(tag_, "Z z");
                lv_obj_align(tag_, LV_ALIGN_TOP_RIGHT, -14, 8);
                break;
            case Expr::kConfused:
                Eyes(38, 50, 0, 0, kCyan, 14, 30);
                Line(44, 0, 50, 8);
                lv_label_set_text(tag_, "?");
                lv_obj_align(tag_, LV_ALIGN_TOP_RIGHT, -14, 8);
                break;
            case Expr::kEmbarrassed:
                Eyes(30, 30, 0, 8, kCyan, LV_RADIUS_CIRCLE);
                Line(30, 0, 52, 0);
                for (int i = 0; i < 2; i++) {
                    int sx = i == 0 ? -1 : 1;
                    Put(blush_[i], sx * (eye_x_ + 28), 24, 40, 22, 0, LV_RADIUS_CIRCLE);
                    Show(blush_[i], true);
                }
                break;
        }
        speaking_ = false;
    }

    // Eyes close briefly now and then (only for filled-shape eyes).
    void Blink(bool closed) {
        if (eyes_are_arcs_ || expr_ == Expr::kSleepy) return;
        for (int i = 0; i < 2; i++) {
            if (lv_obj_has_flag(eye_[i], LV_OBJ_FLAG_HIDDEN)) continue;
            int sx = i == 0 ? -1 : 1;
            int hh = (i == 1 && eye_h_right_ > 0) ? eye_h_right_ : eye_h_;
            int h = closed ? 6 : hh;
            lv_obj_set_height(eye_[i], h);
            lv_obj_align(eye_[i], LV_ALIGN_CENTER, sx * eye_x_,
                         eye_y_ + ((i == 1 && eye_h_right_ > 0) ? (eye_h_ - eye_h_right_) / 2 : 0));
            lv_obj_set_style_transform_pivot_y(eye_[i], h / 2, 0);
        }
    }

    static void TimerCb(lv_timer_t* t) {
        static_cast<RubiDisplay*>(lv_timer_get_user_data(t))->Tick();
    }

    void Tick() {
        if (face_ == nullptr) return;
        bool speak_now = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
        if (speak_now) {
            // lip-sync: the mouth opens and closes at random while speech is playing
            if (!speaking_) {
                speaking_ = true;
                Show(mouth_arc_, false); Show(mouth_line_, false);
                Show(mouth_open_, true);
            }
            int h = 8 + (int)(esp_random() % 34);
            int w = 40 + (int)(esp_random() % 30);
            Put(mouth_open_, 0, 46, w, h, 0, LV_RADIUS_CIRCLE);
            return;
        }
        if (speaking_) {
            Apply(expr_);  // restore the resting mouth of the current expression
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
