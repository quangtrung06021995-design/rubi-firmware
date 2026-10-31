#ifndef RUBI_MATH_H
#define RUBI_MATH_H

// Maths questions for a first grader (Vietnamese "lớp 1"): add / subtract within 10, 20, 100, and word problems.
// Pure logic, no display code (can be tested on a PC: g++ -DRUBI_MATH_TEST -x c++ rubi_math.h).
#include <cstdint>
#include <cstdlib>
#include <string>
#ifdef ESP_PLATFORM
#include <esp_random.h>
#endif

class RubiMath {
public:
    enum Mode { kWithin10 = 0, kWithin20 = 1, kWithin100 = 2, kWords = 3 };

    struct Q {
        std::string story;   // full word problem (empty for a plain sum)
        std::string ask;     // short line shown above the keypad, e.g. "7 + 5 = ?"
        std::string say;     // what the voice reads: "7 cộng 5 bằng mấy" or the whole story
        int answer = 0;
    };

    static Q Make(Mode mode, int index) {
        Q q;
        switch (mode) {
        case kWithin10: q = Plain(10); break;
        case kWithin20: q = Plain(20); break;
        case kWithin100: q = Tens(); break;
        default: q = Word(index < 4 ? 10 : 20); break;
        }
        q.say = Speech(q);
        return q;
    }

private:
    static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
        for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    }
    static std::string Speech(const Q& q) {
        std::string t = q.story.empty() ? q.ask : q.story;
        if (q.story.empty()) {
            ReplaceAll(t, "= ?", "bằng mấy");
            ReplaceAll(t, " + ", " cộng ");
            ReplaceAll(t, " - ", " trừ ");
            ReplaceAll(t, "= ", "bằng ");
            ReplaceAll(t, "?", "mấy");
        } else {
            ReplaceAll(t, "\n", " ");
        }
        return t;
    }

    static uint32_t R() {
#ifdef ESP_PLATFORM
        return esp_random();
#else
        return (uint32_t)rand();
#endif
    }
    static int Between(int lo, int hi) { return lo + (int)(R() % (uint32_t)(hi - lo + 1)); }   // inclusive
    template <size_t N> static const char* Pick(const char* const (&a)[N]) { return a[R() % N]; }

    // a + b = ?, a - b = ?, a + ? = c, ? + b = c, c - ? = b, within `max`.
    static Q Plain(int max) {
        Q q;
        bool add = R() % 2 == 0;
        int form = (int)(R() % 4);   // 0,1 plain; 2,3 find the missing number
        int a, b, c;
        if (add) {
            c = Between(max == 10 ? 3 : 11, max);
            a = Between(1, c - 1);
            b = c - a;
        } else {
            a = Between(max == 10 ? 3 : 11, max);
            b = Between(1, a - 1);
            c = a - b;
        }
        auto s = [](int v) { return std::to_string(v); };
        if (form < 2) {
            q.ask = s(a) + (add ? " + " : " - ") + s(b) + " = ?";
            q.answer = c;
        } else if (form == 2) {
            q.ask = s(a) + (add ? " + ? = " : " - ? = ") + s(c);
            q.answer = b;
        } else if (add) {
            q.ask = "? + " + s(b) + " = " + s(c);
            q.answer = a;
        } else {
            q.ask = "? - " + s(b) + " = " + s(c);
            q.answer = a;
        }
        return q;
    }

    // Within 100 without carrying / borrowing (what grade 1 learns): 34 + 25, 68 - 23, 40 + 30.
    static Q Tens() {
        Q q;
        bool add = R() % 2 == 0;
        int t1, u1, t2, u2;
        if (add) {
            t1 = Between(1, 8); u1 = Between(0, 8);
            t2 = Between(0, 9 - t1); u2 = Between(0, 9 - u1);
            if (t2 == 0 && u2 == 0) { if (u1 < 9) u2 = 1; else t2 = 1; }
        } else {
            t1 = Between(1, 9); u1 = Between(0, 9);
            t2 = Between(0, t1); u2 = Between(0, u1);
            if (t2 == 0 && u2 == 0) { if (u1 > 0) u2 = 1; else t2 = 1; }
        }
        int a = t1 * 10 + u1, b = t2 * 10 + u2;
        q.ask = std::to_string(a) + (add ? " + " : " - ") + std::to_string(b) + " = ?";
        q.answer = add ? a + b : a - b;
        return q;
    }

    static Q Word(int max) {
        static const char* const names[] = {"Lan", "Nam", "Mai", "Hùng", "Na", "Tí", "Hoa", "Minh", "An", "Bình"};
        static const char* const things[] = {"quả cam", "viên bi", "cái kẹo", "bông hoa", "quyển truyện", "cái bánh", "quả táo", "chiếc bút", "con tem", "cái kẹo"};
        static const char* const food[] = {"quả cam", "cái kẹo", "cái bánh", "quả táo"};
        static const char* const givers[] = {"Mẹ", "Bố", "Bà", "Ông", "Chị"};
        Q q;
        std::string A = Pick(names), B = Pick(names);
        while (B == A) B = Pick(names);
        std::string obj = Pick(things), giver = Pick(givers);
        int kind = (int)(R() % 7);
        int a, b;
        auto s = [](int v) { return std::to_string(v); };
        switch (kind) {
        case 0:   // add more
            a = Between(2, max - 2); b = Between(1, max - a);
            q.story = A + " có " + s(a) + " " + obj + ".\n" + giver + " cho thêm " + s(b) + " " + obj + ".\nHỏi " + A + " có tất cả bao nhiêu " + obj + "?";
            q.ask = A + " có tất cả mấy " + obj + "?"; q.answer = a + b; break;
        case 1:   // give away
            a = Between(3, max); b = Between(1, a - 1);
            q.story = A + " có " + s(a) + " " + obj + ".\n" + A + " cho bạn " + s(b) + " " + obj + ".\nHỏi " + A + " còn lại bao nhiêu " + obj + "?";
            q.ask = A + " còn lại mấy " + obj + "?"; q.answer = a - b; break;
        case 2:   // put together
            a = Between(1, max - 1); b = Between(1, max - a);
            q.story = A + " có " + s(a) + " " + obj + ", " + B + " có " + s(b) + " " + obj + ".\nHỏi hai bạn có tất cả bao nhiêu " + obj + "?";
            q.ask = "Hai bạn có tất cả mấy " + obj + "?"; q.answer = a + b; break;
        case 3:   // more than
            a = Between(1, max - 1); b = Between(1, max - a);
            q.story = A + " có " + s(a) + " " + obj + ".\n" + B + " có nhiều hơn " + A + " " + s(b) + " " + obj + ".\nHỏi " + B + " có bao nhiêu " + obj + "?";
            q.ask = B + " có mấy " + obj + "?"; q.answer = a + b; break;
        case 4:   // fewer than
            a = Between(3, max); b = Between(1, a - 1);
            q.story = A + " có " + s(a) + " " + obj + ".\n" + B + " có ít hơn " + A + " " + s(b) + " " + obj + ".\nHỏi " + B + " có bao nhiêu " + obj + "?";
            q.ask = B + " có mấy " + obj + "?"; q.answer = a - b; break;
        case 5: { // eat
            std::string f = Pick(food);
            a = Between(3, max); b = Between(1, a - 1);
            q.story = "Trong rổ có " + s(a) + " " + f + ".\n" + A + " ăn " + s(b) + " " + f + ".\nHỏi trong rổ còn lại mấy " + f + "?";
            q.ask = "Trong rổ còn lại mấy " + f + "?"; q.answer = a - b; break;
        }
        default: { // how many more / fewer (compare)
            a = Between(2, max); b = Between(1, a - 1);
            q.story = A + " có " + s(a) + " " + obj + ", " + B + " có " + s(b) + " " + obj + ".\nHỏi " + A + " nhiều hơn " + B + " bao nhiêu " + obj + "?";
            q.ask = A + " nhiều hơn " + B + " mấy " + obj + "?"; q.answer = a - b; break;
        }
        }
        return q;
    }
};

#ifdef RUBI_MATH_TEST
#include <cstdio>
int main() {
    srand(7);
    int bad = 0;
    for (int m = 0; m < 4; m++) {
        for (int i = 0; i < 20000; i++) {
            auto q = RubiMath::Make((RubiMath::Mode)m, i % 10);
            int lim = m == 0 ? 10 : m == 1 ? 20 : 100;
            if (m == 3) lim = (i % 10) < 4 ? 10 : 20;
            if (q.answer < 0 || q.answer > lim || q.ask.empty()) { if (bad++ < 5) printf("BAD m=%d %s ans=%d\n", m, q.ask.c_str(), q.answer); }
            // check plain sums by evaluating the text
            if (m != 3) {
                int a = 0, b = 0, c = 0; char op = 0; char t1[8], t2[8], t3[8];
                if (sscanf(q.ask.c_str(), "%7s %c %7s = %7s", t1, &op, t2, t3) == 4) {
                    bool u1 = t1[0] == '?', u2 = t2[0] == '?', u3 = t3[0] == '?';
                    a = atoi(t1); b = atoi(t2); c = atoi(t3);
                    if (u1) a = q.answer; if (u2) b = q.answer; if (u3) c = q.answer;
                    int want = op == '+' ? a + b : a - b;
                    if (want != c) { if (bad++ < 5) printf("WRONG %s ans=%d\n", q.ask.c_str(), q.answer); }
                    if (m == 2 && !u3 == false) {}
                }
            }
        }
    }
    for (int m = 0; m < 4; m++) { auto q = RubiMath::Make((RubiMath::Mode)m, 5); printf("[%d] %s | %s | %s | %d\n", m, q.story.c_str(), q.ask.c_str(), q.say.c_str(), q.answer); }
    printf("bad=%d\n", bad);
    return bad != 0;
}
#endif

#endif
