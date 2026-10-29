
(function(){
var views=["trang-chu","tinh-trang","cap-nhat","dieu-khien","choi-cung","bai-hat","truyen-tranh","the-nho","cai-dat"];
var names={"trang-chu":"Trang chủ","tinh-trang":"Tình trạng","cap-nhat":"Cập nhật","dieu-khien":"Điều khiển","choi-cung":"Chơi cùng Rubi","bai-hat":"Bài hát","truyen-tranh":"Truyện tranh","the-nho":"Quản lý thẻ nhớ","cai-dat":"Cài đặt"};
var $=function(i){return document.getElementById(i)},tt;
function toast(m){var t=$("toast");t.textContent=m;t.classList.add("on");clearTimeout(tt);tt=setTimeout(function(){t.classList.remove("on")},1600)}
var LIVE=location.protocol==="http:"&&!/vercel\.app$/.test(location.hostname),M=LIVE?"":" (mẫu)";
var TK="";try{TK=localStorage.getItem("rubi_t")||""}catch(e){}
function api(p){return fetch(location.origin+p+(TK?(p.indexOf("?")<0?"?":"&")+"t="+encodeURIComponent(TK):""),{cache:"no-store"}).then(function(r){return r.json()}).then(function(j){if(j&&j.err==="pin")askPin();return j})}
function askPin(){var b=$("pinbox");if(!b||!b.hidden)return;stopAll();b.hidden=false;$("pinmsg").textContent="Nhập mã PIN để điều khiển Rubi";$("pinin").value="";setTimeout(function(){$("pinin").focus()},50)}
function tryPin(){var v=$("pinin").value.trim();if(!v)return;
 fetch(location.origin+"/api/login?pin="+encodeURIComponent(v),{cache:"no-store"}).then(function(r){return r.json()}).then(function(j){
  if(j.ok){TK=j.t||"";try{localStorage.setItem("rubi_t",TK)}catch(e){}$("pinbox").hidden=true;toast("Đã mở khóa");api("/api/status").then(function(s){if(s.ver)toast("Đã nối Rubi · bản "+s.ver)})}
  else if(j.wait){$("pinmsg").textContent="Sai nhiều lần. Thử lại sau "+j.wait+" giây"}
  else{$("pinmsg").textContent="Sai mã"+(j.left?" (còn "+j.left+" lần)":"");$("pinin").value=""}
 }).catch(function(){$("pinmsg").textContent="Không gọi được Rubi"})}
var pingT=null,pl=0,pr=0;
function drive(l,r){var was=pl||pr;pl=l;pr=r;if(!LIVE)return;
 if(!l&&!r){if(was||pingT)stopAll();return}
 api("/api/drive?l="+l+"&r="+r).catch(function(){});
 if(!pingT)pingT=setInterval(function(){api("/api/drive?l="+pl+"&r="+pr).catch(function(){})},150)}
function stopAll(){pl=pr=0;clearInterval(pingT);pingT=null;if(LIVE)api("/api/stop").catch(function(){toast("Không gọi được Rubi")})}
function pulse(l,r,ms){drive(l,r);setTimeout(stopAll,ms)}
function show(){var h=(location.hash||"#trang-chu").slice(1);if(views.indexOf(h)<0)h="trang-chu";
 views.forEach(function(v){$("v-"+v).classList.toggle("on",v===h)});
 document.querySelectorAll(".drawer a").forEach(function(a){a.classList.toggle("on",a.dataset.v===h)});
 $("title").textContent=names[h];document.body.dataset.v=h;document.body.classList.remove("open");window.scrollTo(0,0)}
document.addEventListener("click",function(e){var a=e.target.closest&&e.target.closest('a[href^="#"]');if(a){e.preventDefault();location.hash=a.getAttribute("href")}});
addEventListener("hashchange",show);show();
if(LIVE){if($("pinok"))$("pinok").onclick=tryPin;if($("pinin"))$("pinin").onkeydown=function(e){if(e.key==="Enter")tryPin()};
 api("/api/status").then(function(j){if(j.pin&&!j.auth)askPin();else toast("Đã nối Rubi · bản "+j.ver)}).catch(function(){toast("Chưa nối được Rubi")})}
$("burger").onclick=function(){document.body.classList.toggle("open")};$("scrim").onclick=function(){document.body.classList.remove("open")};
$("stop2").onclick=function(){stopAll();$("drv").textContent="Đứng yên";toast("Đã gửi lệnh dừng"+M)};
$("estop").onclick=function(){stopAll();$("drv").textContent="Đứng yên";toast("Đã gửi lệnh dừng"+M)};
document.querySelectorAll(".pad button").forEach(function(b){b.onclick=function(){$("drv").textContent=b.dataset.m==="Dừng"?"Đứng yên":"Đang: "+b.dataset.m+" (mẫu)"}});
$("spd").oninput=function(){$("spv").textContent=this.value};$("br").oninput=function(){$("brv").textContent=this.value};
document.querySelectorAll("#leds button").forEach(function(b){b.onclick=function(){document.querySelectorAll("#leds button").forEach(function(x){x.classList.remove("on")});b.classList.add("on");toast("Đổi màu đèn (mẫu)")}});
document.querySelectorAll("#game .act").forEach(function(b){b.onclick=function(){document.querySelectorAll("#game .act").forEach(function(x){x.classList.remove("on")});b.classList.add("on");$("gst").textContent="Đang chơi: "+b.dataset.g+" (mẫu)";$("state").textContent=b.dataset.g}});
var LC={"l-chest":3,"l-fl":6,"l-fr":6,"l-ant":2};
function est(){var n=0;for(var k in LC)if($(k).checked)n+=LC[k];var ma=Math.round(n*60*($("br").value/100));$("est").textContent="Đang bật "+n+"/17 đèn, khoảng "+ma+" mA (mẫu).";}
document.querySelectorAll("[data-l]").forEach(function(c){c.onchange=est});
$("saver").onclick=function(){document.querySelectorAll("[data-l]").forEach(function(c){c.checked=false});est();toast("Đã tắt hết đèn (mẫu)")};
$("allon").onclick=function(){document.querySelectorAll("[data-l]").forEach(function(c){c.checked=true});est();toast("Đã bật lại đèn (mẫu)")};
$("br").addEventListener("input",est);est();
(function(){var joy=$("joy"),k=$("knob"),id=null;
function set(x,y){var R=joy.clientWidth/2-k.clientWidth/2;var m=Math.hypot(x,y);if(m>1){x/=m;y/=m;m=1}
 k.style.transform="translate(calc(-50% + "+(x*R)+"px),calc(-50% + "+(-y*R)+"px))";k.classList.toggle("act",m>.12);
 if(m<.12){$("drv").textContent="Đứng yên";drive(0,0);return}
 var sp=Math.round(m*$("spd").value),t=[];
 if(LIVE){var sc=$("spd").value,cl=function(v){return Math.round(Math.max(-1,Math.min(1,v))*sc)};drive(cl(y+x),cl(y-x))}
 if(Math.abs(y)>.3)t.push(y>0?"Tiến":"Lùi");
 if(Math.abs(x)>.3)t.push(x>0?"rẽ phải":"rẽ trái");
 if(!t.length)t.push(Math.abs(y)>=Math.abs(x)?(y>0?"Tiến":"Lùi"):(x>0?"rẽ phải":"rẽ trái"));
 $("drv").textContent="Đang: "+t.join(", ")+" · "+sp+"%"+M}
function pos(e){var r=joy.getBoundingClientRect(),R=joy.clientWidth/2-k.clientWidth/2;set((e.clientX-r.left-r.width/2)/R,-(e.clientY-r.top-r.height/2)/R)}
joy.addEventListener("pointerdown",function(e){id=e.pointerId;joy.setPointerCapture(id);pos(e)});
joy.addEventListener("pointermove",function(e){if(e.pointerId===id)pos(e)});
function end(e){if(e.pointerId===id){id=null;set(0,0)}}
joy.addEventListener("pointerup",end);joy.addEventListener("pointercancel",end);
$("rl").onclick=function(){if(LIVE)pulse(-50,50,700);$("drv").textContent="Xoay tại chỗ sang trái"+M;setTimeout(function(){$("drv").textContent="Đứng yên"},900)};
$("rr").onclick=function(){if(LIVE)pulse(50,-50,700);$("drv").textContent="Xoay tại chỗ sang phải"+M;setTimeout(function(){$("drv").textContent="Đứng yên"},900)};
$("snap").onclick=function(){toast("Đã chụp ảnh (mẫu)")};
$("camoff").onclick=function(){var s=$("screen").classList.toggle("off");this.textContent=s?"Bật camera":"Tắt camera"};
$("full").onclick=function(){var s=$("screen");if(s.requestFullscreen)s.requestFullscreen().catch(function(){toast("Không mở được toàn màn hình")});else toast("Không hỗ trợ toàn màn hình")};
})();
$("save").onclick=function(){toast("Đã lưu (mẫu)")};
var songs=["Con cò bé bé","Chú voi con ở Bản Đôn","Baby Shark","Twinkle Twinkle Little Star","Cá heo ơi","Bé học đếm số"];
$("songs").innerHTML=songs.map(function(s,i){return '<div class="item"><div class="t">'+s+'<small>'+(2+i%3)+':'+(10+i*7)+'</small></div><button class="btn" data-s="'+s+'">▶ Phát</button></div>'}).join("");
$("songs").onclick=function(e){var b=e.target.closest("button");if(b)toast("Phát: "+b.dataset.s+" (mẫu)")};
$("dkbtn").onclick=function(){var b=this,bar=$("dkbar"),i=bar.firstChild,p=0;b.disabled=true;bar.hidden=false;$("dk").textContent="Đang tải bản 1.0.2...";
 var iv=setInterval(function(){p+=8;i.style.width=Math.min(p,100)+"%";if(p>=100){clearInterval(iv);$("dk").textContent="1.0.2, đã mới nhất";bar.hidden=true;b.textContent="Mới nhất";b.className="btn ghost";document.querySelector(".alert").style.display="none";var dt=document.querySelector(".drawer .dot");if(dt)dt.style.display="none";toast("Cập nhật xong (mẫu)")}},300)};
})();
