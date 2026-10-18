#ifndef RUBI_FACE_H
#define RUBI_FACE_H

// Rubi's face (13 expressions) drawn with LVGL shapes: glowing white eyes, duck-bill mouth,
// red cheeks. Blinks now and then and moves the mouth while the robot is speaking.

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
        kNeutral, kHappy, kLaughing, kWinking, kLoving, kSurprised, kCrying,
        kAngry, kRelaxed, kCool, kEmbarrassed, kSleepy, kThinking
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
    lv_timer_t* timer_ = nullptr;

    Expr expr_ = Expr::kNeutral;
    bool round_eyes_ = false;
    int eye_d_ = 62;
    int blink_countdown_ = 30;
    int blink_phase_ = 0;
    bool speaking_ = false;

    static Expr Parse(const char* e) {
        if (e == nullptr) return Expr::kNeutral;
        auto is = [&](const char* s) { return strcmp(e, s) == 0; };
        if (is("happy") || is("delicious")) return Expr::kHappy;
        if (is("laughing") || is("funny")) return Expr::kLaughing;
        if (is("winking") || is("silly")) return Expr::kWinking;
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

    void Apply(Expr e) {
        expr_ = e;
        speaking_ = false;
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
