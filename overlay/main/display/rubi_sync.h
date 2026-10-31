#ifndef RUBI_SYNC_H
#define RUBI_SYNC_H

// "Kho" (content store) sync: Rubi reads a small catalog from the internet and copies new songs / picture books /
// lessons to its SD card by itself, and deletes the ones the owner marked for removal.
//
// Catalog (JSON) at kCatalogUrl:
//   {"v":1,
//    "items":[{"p":"music/Ba oi.wav","u":"https://.../file","s":8123456},
//             {"p":"truyen/tho-va-rua/01.bin","u":"https://...","s":153600}, ...],
//    "del":["music/old.wav","truyen/old-story"]}
// "p" is the path under /sdcard/rubi (only music/, truyen/, hoc/ are allowed), "u" the download address, "s" the exact size in bytes.
// A file is downloaded only when it is missing or its size differs.
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include "board.h"

class RubiSync {
public:
    static constexpr const char* kCatalogUrl = "https://rubi-web-eight.vercel.app/api/catalog";
    static constexpr const char* kRoot = "/sdcard/rubi/";
    static constexpr uint64_t kKeepFree = 200ull * 1024 * 1024;   // never fill the card below 200 MB free

    struct Result {
        bool catalog_ok = false;
        bool low_space = false;
        bool interrupted = false;
        int downloaded = 0, deleted = 0, failed = 0;
        std::string err;
    };

    // keep_going() returns false when Rubi became busy (voice, music, menu...): the sync stops and tries again later.
    static Result Once(const std::function<bool()>& keep_going) {
        Result r;
        std::string body;
        if (!FetchCatalog(body, r.err)) return r;
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) { r.err = "danh muc loi"; return r; }
        r.catalog_ok = true;
        mkdir("/sdcard/rubi", 0777);

        cJSON* del = cJSON_GetObjectItem(root, "del");
        if (cJSON_IsArray(del)) {
            cJSON* d = nullptr;
            cJSON_ArrayForEach(d, del) {
                if (!cJSON_IsString(d) || !SafePath(d->valuestring)) continue;
                std::string full = std::string(kRoot) + d->valuestring;
                struct stat st;
                if (stat(full.c_str(), &st) == 0) {
                    RemoveTree(full);
                    r.deleted++;
                }
            }
        }

        cJSON* items = cJSON_GetObjectItem(root, "items");
        if (cJSON_IsArray(items)) {
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, items) {
                if (!keep_going()) { r.interrupted = true; break; }
                cJSON* p = cJSON_GetObjectItem(it, "p");
                cJSON* u = cJSON_GetObjectItem(it, "u");
                cJSON* s = cJSON_GetObjectItem(it, "s");
                if (!cJSON_IsString(p) || !cJSON_IsString(u) || !cJSON_IsNumber(s) || !SafePath(p->valuestring)) continue;
                if (strncmp(u->valuestring, "https://", 8) != 0) continue;
                size_t want = (size_t)s->valuedouble;
                std::string full = std::string(kRoot) + p->valuestring;
                struct stat st;
                if (stat(full.c_str(), &st) == 0 && (size_t)st.st_size == want) continue;   // already there
                uint64_t total = 0, freeb = 0;
                if (esp_vfs_fat_info("/sdcard", &total, &freeb) == ESP_OK && freeb < want + kKeepFree) {
                    r.low_space = true;
                    break;
                }
                MakeParents(full);
                if (Download(u->valuestring, full, want, keep_going)) r.downloaded++;
                else if (!keep_going()) { r.interrupted = true; break; }
                else r.failed++;
            }
        }
        cJSON_Delete(root);
        return r;
    }

private:
    static bool SafePath(const char* path) {
        std::string p = path;
        if (p.size() < 3 || p.size() > 180) return false;
        if (p.rfind("music/", 0) != 0 && p.rfind("truyen/", 0) != 0 && p.rfind("hoc/", 0) != 0) return false;
        size_t i = 0;
        while (true) {
            size_t j = p.find('/', i);
            if (j == std::string::npos) j = p.size();
            std::string seg = p.substr(i, j - i);
            if (seg.empty() || seg[0] == '.' || seg.size() > 80) return false;
            for (unsigned char c : seg) {
                if (c < 32 || strchr("\\:*?\"<>|", c) != nullptr) return false;
            }
            if (j == p.size()) break;
            i = j + 1;
        }
        return true;
    }

    static void MakeParents(const std::string& full) {
        for (size_t i = strlen("/sdcard/"); i < full.size(); i++) {
            if (full[i] == '/') mkdir(full.substr(0, i).c_str(), 0777);
        }
    }

    static void RemoveTree(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return;
        if (S_ISDIR(st.st_mode)) {
            if (DIR* d = opendir(path.c_str())) {
                while (dirent* e = readdir(d)) {
                    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                    RemoveTree(path + "/" + e->d_name);
                }
                closedir(d);
            }
            rmdir(path.c_str());
        } else {
            unlink(path.c_str());
        }
    }

    static bool FetchCatalog(std::string& body, std::string& err) {
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (auto opened = http->Open("GET", kCatalogUrl); !opened) { err = "khong noi duoc mang"; return false; }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            err = status ? ("ma loi " + std::to_string(*status)) : "khong doc duoc";
            http->Close();
            return false;
        }
        if (http->GetBodyLength() > 256 * 1024) { err = "danh muc qua lon"; http->Close(); return false; }
        body = http->ReadAll();
        http->Close();
        return !body.empty();
    }

    static bool Download(const char* url, const std::string& dst, size_t expect, const std::function<bool()>& keep_going) {
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (auto opened = http->Open("GET", url); !opened) return false;
        auto status = http->GetStatusCode();
        if (!status || *status != 200) { http->Close(); return false; }
        std::string tmp = dst + ".part";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f == nullptr) { http->Close(); return false; }
        constexpr size_t kBuf = 4096;
        char* buf = (char*)heap_caps_malloc(kBuf, MALLOC_CAP_INTERNAL);
        if (buf == nullptr) { fclose(f); unlink(tmp.c_str()); http->Close(); return false; }
        size_t total = 0;
        bool ok = true;
        while (true) {
            if (!keep_going()) { ok = false; break; }
            auto r = http->Read(buf, kBuf);
            if (!r) { ok = false; break; }
            int n = *r;
            if (n <= 0) break;
            if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
            total += (size_t)n;
        }
        heap_caps_free(buf);
        fclose(f);
        http->Close();
        if (!ok || (expect > 0 && total != expect)) { unlink(tmp.c_str()); return false; }
        unlink(dst.c_str());
        if (rename(tmp.c_str(), dst.c_str()) != 0) { unlink(tmp.c_str()); return false; }
        return true;
    }
};

#endif
