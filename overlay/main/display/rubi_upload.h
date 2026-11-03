#ifndef RUBI_UPLOAD_H
#define RUBI_UPLOAD_H

// Phone -> Rubi music and picture-book upload. A tiny web server on the screen board, running ONLY while the "Tải nhạc từ điện thoại"
// screen is open. The phone (same Wi-Fi) opens the page, types the 4-digit code shown on Rubi's screen, picks songs;
// the page converts them to 16-bit mono WAV in the browser and sends them to /sdcard/rubi/music.
// Stories: the page turns pictures into 320x240 RGB565 (.bin) and voices into 16 kHz WAV, and sends them to
// /sdcard/rubi/truyen/<id>/ (ten.txt, 01.bin, 01.wav, ...), the same layout as tools/tao_truyen.py.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>

class RubiUpload {
public:
    static bool Start(const char* code) {
        if (Handle() != nullptr) Stop();
        Code() = code;
        mkdir("/sdcard/rubi", 0777);
        mkdir(kDir, 0777);
        mkdir(kStories, 0777);
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.stack_size = 8192;
        cfg.max_uri_handlers = 8;
        cfg.max_open_sockets = 4;
        cfg.recv_wait_timeout = 15;
        cfg.send_wait_timeout = 15;
        cfg.lru_purge_enable = true;
        httpd_handle_t h = nullptr;
        if (httpd_start(&h, &cfg) != ESP_OK) {
            ESP_LOGW("RubiUp", "httpd_start failed");
            return false;
        }
        const httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = Root, .user_ctx = nullptr},
            {.uri = "/api/list", .method = HTTP_GET, .handler = List, .user_ctx = nullptr},
            {.uri = "/api/del", .method = HTTP_GET, .handler = Del, .user_ctx = nullptr},
            {.uri = "/api/up", .method = HTTP_POST, .handler = Up, .user_ctx = nullptr},
            {.uri = "/api/stories", .method = HTTP_GET, .handler = Stories, .user_ctx = nullptr},
            {.uri = "/api/delstory", .method = HTTP_GET, .handler = DelStory, .user_ctx = nullptr},
            {.uri = "/api/upst", .method = HTTP_POST, .handler = UpStory, .user_ctx = nullptr},
        };
        for (const auto& u : uris) httpd_register_uri_handler(h, &u);
        Handle() = h;
        return true;
    }

    static void Stop() {
        if (Handle() != nullptr) {
            httpd_stop(Handle());
            Handle() = nullptr;
        }
        Code().clear();
    }

private:
    static constexpr const char* kDir = "/sdcard/rubi/music";
    static constexpr const char* kStories = "/sdcard/rubi/truyen";

    static httpd_handle_t& Handle() { static httpd_handle_t h = nullptr; return h; }
    static std::string& Code() { static std::string c; return c; }

    static std::string UrlDecode(const char* s) {
        std::string o;
        for (; *s; s++) {
            if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
                char h[3] = {s[1], s[2], 0};
                o += (char)strtol(h, nullptr, 16);
                s += 2;
            } else if (*s == '+') {
                o += ' ';
            } else {
                o += *s;
            }
        }
        return o;
    }

    static bool Param(httpd_req_t* req, const char* key, std::string& out) {
        char q[400];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
        char v[360];
        if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return false;
        out = UrlDecode(v);
        return true;
    }

    static bool Auth(httpd_req_t* req) {
        std::string c;
        return !Code().empty() && Param(req, "c", c) && c == Code();
    }

    static bool SafeName(const std::string& n) {
        if (n.size() < 5 || n.size() > 100 || n[0] == '.') return false;
        for (unsigned char ch : n) {
            if (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return false;
        }
        const char* e = n.c_str() + n.size() - 4;
        return tolower(e[0]) == '.' && tolower(e[1]) == 'w' && tolower(e[2]) == 'a' && tolower(e[3]) == 'v';
    }

    static bool SafeId(const std::string& n) {
        if (n.empty() || n.size() > 40) return false;
        for (unsigned char ch : n) if (!(islower(ch) || isdigit(ch) || ch == '-' || ch == '_')) return false;
        return true;
    }

    // Files of a story: ten.txt, or 1-2 digits followed by .bin (picture) or .wav (voice).
    static bool SafeStoryFile(const std::string& n) {
        if (n == "ten.txt") return true;
        size_t d = 0;
        while (d < n.size() && isdigit((unsigned char)n[d])) d++;
        if (d < 1 || d > 2 || n.size() != d + 4) return false;
        return n.compare(d, 4, ".bin") == 0 || n.compare(d, 4, ".wav") == 0;
    }

    static esp_err_t Fail(httpd_req_t* req, const char* status) {
        httpd_resp_set_status(req, status);
        httpd_resp_send(req, status, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    static esp_err_t Root(httpd_req_t* req) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, Page(), HTTPD_RESP_USE_STRLEN);
    }

    static std::string JsonEsc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c >= 32) o += c;
        }
        return o;
    }

    static esp_err_t List(httpd_req_t* req) {
        if (!Auth(req)) return Fail(req, "403 Forbidden");
        std::string j = "{\"songs\":[";
        bool first = true;
        if (DIR* d = opendir(kDir)) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (!SafeName(n)) continue;
                struct stat st;
                std::string p = std::string(kDir) + "/" + n;
                long sz = (stat(p.c_str(), &st) == 0) ? (long)st.st_size : 0;
                if (!first) j += ',';
                first = false;
                j += "{\"n\":\"" + JsonEsc(n) + "\",\"s\":" + std::to_string(sz) + "}";
            }
            closedir(d);
        }
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info("/sdcard", &total, &freeb) != ESP_OK) freeb = 0;
        j += "],\"free\":" + std::to_string((unsigned long long)freeb) + "}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, j.c_str(), j.size());
    }

    static esp_err_t Del(httpd_req_t* req) {
        if (!Auth(req)) return Fail(req, "403 Forbidden");
        std::string n;
        if (!Param(req, "n", n) || !SafeName(n)) return Fail(req, "400 Bad Request");
        unlink((std::string(kDir) + "/" + n).c_str());
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t Up(httpd_req_t* req) {
        if (!Auth(req)) return Fail(req, "403 Forbidden");
        std::string n;
        if (!Param(req, "n", n) || !SafeName(n)) return Fail(req, "400 Bad Request");
        return Receive(req, std::string(kDir) + "/" + n);
    }

    static esp_err_t Stories(httpd_req_t* req) {
        if (!Auth(req)) return Fail(req, "403 Forbidden");
        std::string j = "{\"stories\":[";
        bool first = true;
        if (DIR* d = opendir(kStories)) {
            while (dirent* e = readdir(d)) {
                std::string id = e->d_name;
                if (!SafeId(id)) continue;
                std::string dir = std::string(kStories) + "/" + id;
                long total = 0;
                std::string title = id;
                if (DIR* d2 = opendir(dir.c_str())) {
                    while (dirent* e2 = readdir(d2)) {
                        struct stat st;
                        std::string p = dir + "/" + e2->d_name;
                        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) total += (long)st.st_size;
                    }
                    closedir(d2);
                }
                if (FILE* tf = fopen((dir + "/ten.txt").c_str(), "rb")) {
                    char line[100] = {0};
                    if (fgets(line, sizeof(line), tf)) {
                        title = line;
                        while (!title.empty() && (title.back() == '\n' || title.back() == '\r')) title.pop_back();
                        if (title.empty()) title = id;
                    }
                    fclose(tf);
                }
                if (!first) j += ',';
                first = false;
                j += "{\"id\":\"" + id + "\",\"t\":\"" + JsonEsc(title) + "\",\"s\":" + std::to_string(total) + "}";
            }
            closedir(d);
        }
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info("/sdcard", &total, &freeb) != ESP_OK) freeb = 0;
        j += "],\"free\":" + std::to_string((unsigned long long)freeb) + "}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, j.c_str(), j.size());
    }

    static esp_err_t DelStory(httpd_req_t* req) {
        if (!Auth(req)) return Fail(req, "403 Forbidden");
        std::string id;
        if (!Param(req, "s", id) || !SafeId(id)) return Fail(req, "400 Bad Request");
        std::string dir = std::string(kStories) + "/" + id;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                unlink((dir + "/" + n).c_str());
            }
            closedir(d);
        }
        rmdir(dir.c_str());
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t UpStory(httpd_req_t* req) {
        if (!Auth(req)) return Fail(req, "403 Forbidden");
        std::string id, fn;
        if (!Param(req, "s", id) || !SafeId(id) || !Param(req, "f", fn) || !SafeStoryFile(fn)) return Fail(req, "400 Bad Request");
        mkdir(kStories, 0777);
        std::string dir = std::string(kStories) + "/" + id;
        mkdir(dir.c_str(), 0777);
        return Receive(req, dir + "/" + fn);
    }

    // Writes the request body to `path` (through a .part file, so a broken upload never leaves half a file).
    static esp_err_t Receive(httpd_req_t* req, const std::string& path) {
        size_t remaining = req->content_len;
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info("/sdcard", &total, &freeb) == ESP_OK && remaining + 1024 * 1024 > freeb) return Fail(req, "507 Insufficient Storage");
        std::string tmp = path + ".part";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f == nullptr) return Fail(req, "500 Internal Server Error");
        const size_t kBuf = 8192;
        char* buf = (char*)heap_caps_malloc(kBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == nullptr) buf = (char*)malloc(kBuf);
        if (buf == nullptr) { fclose(f); unlink(tmp.c_str()); return Fail(req, "500 Internal Server Error"); }
        bool ok = true;
        int idle = 0;
        while (remaining > 0) {
            int r = httpd_req_recv(req, buf, remaining < kBuf ? remaining : kBuf);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++idle > 4) { ok = false; break; }
                continue;
            }
            if (r <= 0) { ok = false; break; }
            idle = 0;
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
            remaining -= (size_t)r;
        }
        free(buf);
        fclose(f);
        if (!ok) {
            unlink(tmp.c_str());
            return Fail(req, "500 Internal Server Error");
        }
        unlink(path.c_str());
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
            return Fail(req, "500 Internal Server Error");
        }
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

    static const char* Page() {
        return R"RAW(<!doctype html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Rubi - Tải nhạc và truyện</title>
<style>
body{margin:0;padding:16px;background:#101820;color:#eaf1f7;font:16px system-ui,sans-serif}
h1{font-size:20px;margin:6px 0 14px}input,button{font:inherit;padding:10px 14px;border-radius:10px;border:0;margin:4px 0}
input{background:#24384a;color:#fff}button{background:#2a8c5a;color:#fff}li{margin:8px 0;list-style:none;background:#1a2a38;padding:8px 10px;border-radius:10px}
li button{background:#a33;padding:4px 10px;float:right}ul{padding:0}#ms,#sm,.mu{color:#9fb0c0}hr{border:0;border-top:1px solid #2a3a4a;margin:18px 0}
</style></head><body>
<h1>Tải nhạc và truyện cho Rubi</h1>
<div id="lg"><p>Nhập mã 4 số đang hiện trên màn hình Rubi</p><input id="cd" inputmode="numeric" maxlength="4" size="6"> <button id="go">Vào</button></div>
<div id="mn" hidden>
<p>Chọn bài hát (MP3, M4A, WAV...). Trang tự đổi sang WAV rồi gửi vào thẻ nhớ.</p>
<input type="file" id="f" accept="audio/*" multiple><br><button id="up">Tải lên</button>
<p id="ms"></p><p id="fr"></p><ul id="ls"></ul>
<hr><h1>Thêm truyện tranh</h1>
<p class="mu">Gõ tên truyện, chọn ảnh các trang (xếp theo tên file: 01, 02, 03...) và lời đọc từng trang nếu có (đặt tên trùng ảnh: 01.mp3 cho 01.jpg).</p>
<input type="text" id="st" placeholder="Tên truyện" size="28"><br>
<p class="mu">Ảnh các trang:</p><input type="file" id="si" accept="image/*" multiple>
<p class="mu">Lời đọc (nếu có):</p><input type="file" id="sa" accept="audio/*" multiple><br>
<button id="sup">Tải truyện lên</button><p id="sm"></p><ul id="sl"></ul></div>
<script>
var C='',$=function(i){return document.getElementById(i)};
function api(p,o){return fetch(p+(p.indexOf('?')<0?'?':'&')+'c='+C,o||{cache:'no-store'})}
function show(j){$('lg').hidden=true;$('mn').hidden=false;$('fr').textContent='Còn trống: '+Math.round(j.free/1048576)+' MB';
 $('ls').innerHTML='';j.songs.sort(function(a,b){return a.n<b.n?-1:1}).forEach(function(s){var li=document.createElement('li');li.textContent=s.n+' ('+(s.s/1048576).toFixed(1)+' MB)';
 var b=document.createElement('button');b.textContent='Xóa';b.onclick=function(){if(confirm('Xóa '+s.n+'?'))api('/api/del?n='+encodeURIComponent(s.n)).then(ref)};li.appendChild(b);$('ls').appendChild(li)})}
function ref(){return api('/api/list').then(function(r){return r.json()}).then(show).then(refSt)}
function refSt(){return api('/api/stories').then(function(r){return r.json()}).then(function(j){
 $('sl').innerHTML='';j.stories.forEach(function(s){var li=document.createElement('li');li.textContent=s.t+' ('+(s.s/1048576).toFixed(1)+' MB)';
 var b=document.createElement('button');b.textContent='Xóa';b.onclick=function(){if(confirm('Xóa truyện '+s.t+'?'))api('/api/delstory?s='+encodeURIComponent(s.id)).then(refSt).then(ref)};li.appendChild(b);$('sl').appendChild(li)})})}
$('go').onclick=function(){C=$('cd').value.trim();api('/api/list').then(function(r){if(r.status==403){alert('Sai mã');throw 0}return r.json()}).then(show).catch(function(){})};
async function toWav(file,R){R=R||22050;
 var ab=await file.arrayBuffer(),ac=new (window.AudioContext||window.webkitAudioContext)(),buf=await ac.decodeAudioData(ab);if(ac.close)ac.close();
 var len=Math.ceil(buf.duration*R),oc=new (window.OfflineAudioContext||window.webkitOfflineAudioContext)(1,len,R),src=oc.createBufferSource();
 src.buffer=buf;src.connect(oc.destination);src.start();
 var out=await oc.startRendering(),d=out.getChannelData(0),n=d.length,w=new DataView(new ArrayBuffer(44+n*2));
 function s(o,t){for(var i=0;i<t.length;i++)w.setUint8(o+i,t.charCodeAt(i))}
 s(0,'RIFF');w.setUint32(4,36+n*2,true);s(8,'WAVE');s(12,'fmt ');w.setUint32(16,16,true);w.setUint16(20,1,true);w.setUint16(22,1,true);
 w.setUint32(24,R,true);w.setUint32(28,R*2,true);w.setUint16(32,2,true);w.setUint16(34,16,true);s(36,'data');w.setUint32(40,n*2,true);
 for(var i=0;i<n;i++){var v=Math.max(-1,Math.min(1,d[i]));w.setInt16(44+i*2,v<0?v*32768:v*32767,true)}
 return w.buffer}
$('up').onclick=async function(){
 var fs=$('f').files;if(!fs.length){$('ms').textContent='Chưa chọn bài nào';return}
 $('up').disabled=true;
 for(var i=0;i<fs.length;i++){var f=fs[i],nm=f.name.replace(/\.[^.]*$/,'').replace(/[\\\/:*?"<>|]/g,' ').trim().slice(0,60)+'.wav';
  try{$('ms').textContent='('+(i+1)+'/'+fs.length+') Đang đổi: '+f.name;var wav=await toWav(f);
   $('ms').textContent='('+(i+1)+'/'+fs.length+') Đang gửi: '+nm;
   var r=await api('/api/up?n='+encodeURIComponent(nm),{method:'POST',body:wav});if(!r.ok)throw new Error(r.status==507?'thẻ nhớ đầy':'lỗi '+r.status)}
  catch(e){$('ms').textContent='Lỗi với '+f.name+': '+(e.message||e);$('up').disabled=false;return}}
 $('ms').textContent='Xong! Đã gửi '+fs.length+' bài.';$('up').disabled=false;$('f').value='';ref()};
function asc(s){return s.normalize('NFD').replace(/[\u0300-\u036f]/g,'').replace(/\u0111/g,'d').replace(/\u0110/g,'D')}
function slug(s){return asc(s).toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-+|-+$/g,'').slice(0,40)}
function bn(f){return f.name.replace(/\.[^.]*$/,'')}
function srt(a,b){return a.name.localeCompare(b.name,undefined,{numeric:true})}
async function toRgb565(file){
 var bmp=await createImageBitmap(file),c=document.createElement('canvas');c.width=320;c.height=240;
 var g=c.getContext('2d',{willReadFrequently:true}),k=Math.max(320/bmp.width,240/bmp.height),w=bmp.width*k,h=bmp.height*k;
 g.drawImage(bmp,(320-w)/2,(240-h)/2,w,h);if(bmp.close)bmp.close();
 var px=g.getImageData(0,0,320,240).data,o=new Uint8Array(320*240*2);
 for(var i=0,j=0;i<px.length;i+=4,j+=2){var v=((px[i]>>3)<<11)|((px[i+1]>>2)<<5)|(px[i+2]>>3);o[j]=v&255;o[j+1]=v>>8}
 return o}
function st(t){$('sm').textContent=t}
async function sendSt(id,fn,body){var r=await api('/api/upst?s='+encodeURIComponent(id)+'&f='+encodeURIComponent(fn),{method:'POST',body:body});
 if(!r.ok)throw new Error(r.status==507?'thẻ nhớ đầy':'lỗi '+r.status)}
$('sup').onclick=async function(){
 var title=$('st').value.trim(),id=slug(title),im=[].slice.call($('si').files).sort(srt),au=[].slice.call($('sa').files).sort(srt);
 if(!id){st('Hãy gõ tên truyện');return}if(!im.length){st('Chưa chọn ảnh nào');return}if(im.length>99){st('Tối đa 99 trang');return}
 $('sup').disabled=true;
 try{var by={};au.forEach(function(a){by[bn(a).toLowerCase()]=a});
  st('Đang gửi tên truyện...');await sendSt(id,'ten.txt',new Blob([title+'\n']));
  for(var i=0;i<im.length;i++){var pd=String(i+1).padStart(2,'0'),tag='Trang '+(i+1)+'/'+im.length+': ';
   st(tag+'đổi ảnh...');await sendSt(id,pd+'.bin',await toRgb565(im[i]));
   var a=by[bn(im[i]).toLowerCase()]||(au.length==im.length?au[i]:null);
   if(a){st(tag+'đổi lời đọc...');var wv=await toWav(a,16000);st(tag+'đang gửi lời đọc...');await sendSt(id,pd+'.wav',wv)}}
  st('Xong! Truyện "'+title+'" ('+im.length+' trang) đã vào thẻ nhớ.');$('st').value='';$('si').value='';$('sa').value=''}
 catch(e){st('Lỗi: '+(e.message||e))}
 $('sup').disabled=false;ref()};
</script></body></html>)RAW";
    }
};

#endif
