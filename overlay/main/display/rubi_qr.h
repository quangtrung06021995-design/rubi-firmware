#ifndef RUBI_QR_H
#define RUBI_QR_H

// Tiny QR code generator for Rubi's screen: one fixed symbol size, version 2 (25 x 25 modules), error correction
// level M, byte mode, so it holds up to 26 bytes - enough for "http://255.255.255.255" (22 bytes).
// No dependencies. Make() fills m[y][x] with 1 = dark module, 0 = light module.
#include <cstdint>
#include <cstring>

class RubiQr {
public:
    static constexpr int kSize = 25;
    static constexpr int kMaxBytes = 26;

    // Returns false when the text is longer than kMaxBytes.
    static bool Make(const char* text, uint8_t m[kSize][kSize]) {
        size_t n = strlen(text);
        if (n > (size_t)kMaxBytes) return false;

        // 1) data codewords: mode 0100 | count (8 bits) | bytes | terminator | padding -> 28 bytes
        uint8_t cw[44] = {0};
        int bits = 0;
        auto put = [&](uint32_t v, int len) {
            for (int i = len - 1; i >= 0; i--, bits++)
                if ((v >> i) & 1) cw[bits >> 3] |= (uint8_t)(0x80 >> (bits & 7));
        };
        put(4, 4);
        put((uint32_t)n, 8);
        for (size_t i = 0; i < n; i++) put((uint8_t)text[i], 8);
        int cap = 28 * 8;
        int term = cap - bits < 4 ? cap - bits : 4;
        bits += term;                       // terminator (zeros)
        bits = (bits + 7) & ~7;             // pad to a byte boundary
        for (int pad = 0xEC; bits < cap; pad ^= (0xEC ^ 0x11)) { cw[bits >> 3] = (uint8_t)pad; bits += 8; }

        // 2) 16 error-correction codewords (Reed-Solomon over GF(256), polynomial 0x11D)
        uint8_t gen[16] = {0};
        gen[15] = 1;
        uint8_t root = 1;
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 16; j++) {
                gen[j] = Mul(gen[j], root);
                if (j + 1 < 16) gen[j] ^= gen[j + 1];
            }
            root = Mul(root, 0x02);
        }
        uint8_t* ec = cw + 28;
        for (int i = 0; i < 28; i++) {
            uint8_t f = cw[i] ^ ec[0];
            memmove(ec, ec + 1, 15);
            ec[15] = 0;
            for (int j = 0; j < 16; j++) ec[j] ^= Mul(gen[j], f);
        }

        // 3) function patterns
        uint8_t fn[kSize][kSize];
        memset(fn, 0, sizeof(fn));
        memset(m, 0, kSize * kSize);
        for (int i = 0; i < kSize; i++) { Set(m, fn, 6, i, i % 2 == 0); Set(m, fn, i, 6, i % 2 == 0); }   // timing
        const int fc[3][2] = {{3, 3}, {kSize - 4, 3}, {3, kSize - 4}};
        for (auto& c : fc) {                                                                              // finders + separators
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++) {
                    int x = c[0] + dx, y = c[1] + dy;
                    if (x < 0 || y < 0 || x >= kSize || y >= kSize) continue;
                    int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
                    Set(m, fn, x, y, d != 2 && d != 4);
                }
        }
        for (int dy = -2; dy <= 2; dy++)                                                                  // alignment pattern at (18,18)
            for (int dx = -2; dx <= 2; dx++) {
                int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
                Set(m, fn, 18 + dx, 18 + dy, d != 1);
            }
        FormatBits(m, fn, 0);                                                                              // reserve the format areas
        // 4) place data bits in the zigzag order
        int bi = 0, total = 44 * 8;
        for (int right = kSize - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < kSize; vert++)
                for (int j = 0; j < 2; j++) {
                    int x = right - j;
                    bool up = ((right + 1) & 2) == 0;
                    int y = up ? kSize - 1 - vert : vert;
                    if (!fn[y][x] && bi < total) {
                        m[y][x] = (cw[bi >> 3] >> (7 - (bi & 7))) & 1;
                        bi++;
                    }
                }
        }
        // 5) choose the mask with the lowest penalty
        int best = 0, bestScore = 1 << 30;
        for (int k = 0; k < 8; k++) {
            ApplyMask(m, fn, k);
            FormatBits(m, fn, k);
            int s = Penalty(m);
            if (s < bestScore) { bestScore = s; best = k; }
            ApplyMask(m, fn, k);            // undo (XOR)
        }
        ApplyMask(m, fn, best);
        FormatBits(m, fn, best);
        return true;
    }

private:
    static uint8_t Mul(uint8_t a, uint8_t b) {
        uint8_t z = 0;
        for (int i = 7; i >= 0; i--) {
            z = (uint8_t)((z << 1) ^ ((z >> 7) * 0x11D));
            z ^= (uint8_t)(((b >> i) & 1) * a);
        }
        return z;
    }

    static void Set(uint8_t m[kSize][kSize], uint8_t fn[kSize][kSize], int x, int y, bool dark) {
        m[y][x] = dark ? 1 : 0;
        fn[y][x] = 1;
    }

    // Writes the 15 format bits (error correction level M + mask) in both places and the always-dark module.
    static void FormatBits(uint8_t m[kSize][kSize], uint8_t fn[kSize][kSize], int mask) {
        uint32_t data = (0u << 3) | (uint32_t)mask;     // level M = 00
        uint32_t rem = data;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        uint32_t b = ((data << 10) | rem) ^ 0x5412;
        auto bit = [&](int i) { return ((b >> i) & 1) != 0; };
        for (int i = 0; i <= 5; i++) Set(m, fn, 8, i, bit(i));
        Set(m, fn, 8, 7, bit(6));
        Set(m, fn, 8, 8, bit(7));
        Set(m, fn, 7, 8, bit(8));
        for (int i = 9; i < 15; i++) Set(m, fn, 14 - i, 8, bit(i));
        for (int i = 0; i < 8; i++) Set(m, fn, kSize - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; i++) Set(m, fn, 8, kSize - 15 + i, bit(i));
        Set(m, fn, 8, kSize - 8, true);
    }

    static void ApplyMask(uint8_t m[kSize][kSize], uint8_t fn[kSize][kSize], int k) {
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                if (fn[y][x]) continue;
                bool inv;
                switch (k) {
                case 0: inv = (x + y) % 2 == 0; break;
                case 1: inv = y % 2 == 0; break;
                case 2: inv = x % 3 == 0; break;
                case 3: inv = (x + y) % 3 == 0; break;
                case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv) m[y][x] ^= 1;
            }
    }

    // The four penalty rules of the QR standard.
    static int Penalty(uint8_t m[kSize][kSize]) {
        int score = 0;
        for (int dir = 0; dir < 2; dir++) {                       // rule 1 (runs of 5+) and rule 3 (finder-like 1:1:3:1:1)
            for (int a = 0; a < kSize; a++) {
                int run = 1;
                uint8_t line[kSize];
                for (int b = 0; b < kSize; b++) line[b] = dir ? m[b][a] : m[a][b];
                for (int b = 1; b < kSize; b++) {
                    if (line[b] == line[b - 1]) { run++; if (run == 5) score += 3; else if (run > 5) score += 1; }
                    else run = 1;
                }
                for (int b = 0; b + 10 < kSize + 0 && b + 10 <= kSize - 1; b++) {
                    static const uint8_t p1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
                    static const uint8_t p2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                    bool m1 = true, m2 = true;
                    for (int t = 0; t < 11; t++) { if (line[b + t] != p1[t]) m1 = false; if (line[b + t] != p2[t]) m2 = false; }
                    if (m1) score += 40;
                    if (m2) score += 40;
                }
            }
        }
        for (int y = 0; y + 1 < kSize; y++)                        // rule 2 (2x2 blocks)
            for (int x = 0; x + 1 < kSize; x++)
                if (m[y][x] == m[y][x + 1] && m[y][x] == m[y + 1][x] && m[y][x] == m[y + 1][x + 1]) score += 3;
        int dark = 0;                                              // rule 4 (balance)
        for (int y = 0; y < kSize; y++) for (int x = 0; x < kSize; x++) dark += m[y][x];
        int pct = dark * 100 / (kSize * kSize);
        int dev = pct > 50 ? pct - 50 : 50 - pct;
        score += (dev / 5) * 10;
        return score;
    }
};

#endif
