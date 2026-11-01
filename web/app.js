
(function(){
var lt=0;document.addEventListener("touchend",function(e){var n=Date.now();if(n-lt<350)e.preventDefault();lt=n},{passive:false});document.addEventListener("gesturestart",function(e){e.preventDefault()});
var views=["trang-chu","tinh-trang","cap-nhat","dieu-khien","choi-cung","bai-hat","truyen-tranh","hoc-tap","the-nho","cai-dat"];
var names={"trang-chu":"Trang chủ","tinh-trang":"Tình trạng","cap-nhat":"Cập nhật","dieu-khien":"Điều khiển","choi-cung":"Chơi cùng Rubi","bai-hat":"Bài hát","truyen-tranh":"Truyện tranh","hoc-tap":"Học tập","the-nho":"Quản lý thẻ nhớ","cai-dat":"Cài đặt"};
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
document.querySelectorAll("#game .act").forEach(function(b){b.onclick=function(){
 if(LIVE){
  if(b.dataset.g==="Đi ngủ"){lenh("sleep");return}
  if(b.dataset.g!=="Nhảy múa"){toast("Trò này chưa có trên Rubi");return}
  api("/api/dance").then(function(j){
   if(j&&j.ok){document.querySelectorAll("#game .act").forEach(function(x){x.classList.remove("on")});b.classList.add("on");$("gst").textContent="Rubi đang nhảy múa (xoay tại chỗ) cho đến khi bạn bấm Dừng lại, tối đa 10 phút. Nhớ để robot ở chỗ rộng.";toast("Rubi đang nhảy múa")}
   else if(j&&j.err==="pin")toast("Pin yếu, Rubi không nhảy được");
  }).catch(function(){toast("Không gọi được Rubi")});
  return}
 document.querySelectorAll("#game .act").forEach(function(x){x.classList.remove("on")});b.classList.add("on");$("gst").textContent="Đang chơi: "+b.dataset.g+" (mẫu)";$("state").textContent=b.dataset.g}});
if($("gstop"))$("gstop").onclick=function(){stopAll();if(LIVE)api("/api/lenh?c=huy").catch(function(){});document.querySelectorAll("#game .act").forEach(function(x){x.classList.remove("on")});$("gst").textContent="Đã dừng.";toast("Đã gửi lệnh dừng"+M)};
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
/* ---- Chế độ thật (mở từ rubi.local): số thật từ DevKit (/api/status) + bản mới nhất trên GitHub ---- */
if(LIVE){
var RAW="https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/";
var LAT={ds:"",dk:"",cam:""},latAt=0,ST=null;
function esc(s){return String(s==null?"":s).replace(/[&<>"]/g,function(c){return{"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]})}
function man(br){return fetch(RAW+br+"/manifest.json?t="+Date.now(),{cache:"no-store"}).then(function(r){return r.json()}).then(function(j){return j&&j.version?String(j.version):""}).catch(function(){return ""})}
function loadLat(){if(Date.now()-latAt<60000)return Promise.resolve();latAt=Date.now();
 return Promise.all([man("ota"),man("ota-devkit"),man("ota-cam")]).then(function(a){LAT.ds=a[0];LAT.dk=a[1];LAT.cam=a[2]})}
function stat(k,v){return '<div class="stat"><span>'+k+'</span><span>'+esc(v)+'</span></div>'}
function note(t){return '<p style="color:var(--mute);font-size:13px;margin:8px 0 0">'+t+'</p>'}
function setCard(id,h,body){var c=$(id);if(c)c.innerHTML='<h2>'+h+'</h2>'+body}
function pinBtn(){return '<button class="btn" id="pinbtn" style="margin-top:8px">Nhập PIN</button>'}
function bindPin(){var b=$("pinbtn");if(b)b.onclick=askPin}
function render(){
 var s=ST,ok=s&&s.auth!==false,ver=s&&s.ver?String(s.ver):"",bad=s?null:note("Chưa nối được Rubi. Kiểm tra Wi-Fi và nguồn.");
 if(!s){setCard("cpin","Pin",bad);setCard("cnet","Mạng",bad);setCard("csen","Bánh xe",bad)}
 else if(!ok){var n=note("Cần mã PIN để xem đầy đủ.")+pinBtn();setCard("cpin","Pin",n);setCard("cnet","Mạng",n);setCard("csen","Bánh xe",n)}
 else{
  var mv=+s.bat_mv||0;
  if(mv>2000){var p=Math.max(0,Math.min(100,Math.round((mv-6000)/24)));
   setCard("cpin","Pin",'<div class="big">'+p+'%</div><div class="bar"><i style="width:'+p+'%"></i></div>'+stat("Điện áp",(mv/1000).toFixed(1).replace(".",",")+" V")+note("Phần trăm là ước lượng theo điện áp của bộ pin 2S."))}
  else setCard("cpin","Pin",note("Chưa đo được điện áp pin."));
  setCard("cnet","Mạng",stat("Wi-Fi",s.ssid||"—")+stat("Tín hiệu",(s.rssi!=null?s.rssi+" dBm":"—"))+stat("Địa chỉ",s.ip||"—"));
  setCard("csen","Bánh xe",stat("Bánh trái",(s.l!=null?s.l+"%":"—"))+stat("Bánh phải",(s.r!=null?s.r+"%":"—"))+stat("Lỗi motor",s.fault?"Có lỗi":"Không")+note("Cảm biến khoảng cách chưa có số thật."));
  bindPin()}
 var dkNew=!!(ver&&LAT.dk&&ver!==LAT.dk);
 function pill(c,t){return '<span class="pill '+c+'"><b></b>'+t+'</span>'}
 function unk(n,l){return '<tr><td>'+n+'</td><td>—</td><td>'+esc(l||"—")+'</td><td>Chưa đọc bản đang chạy</td></tr>'}
 var tb=$("vtb");
 if(tb)tb.innerHTML=unk("Bo màn hình",LAT.ds)+
  '<tr><td>DevKit (bánh xe)</td><td>'+esc(ver||"—")+'</td><td>'+esc(LAT.dk||"—")+'</td><td>'+(!ver||!LAT.dk?"—":dkNew?pill("warn","Có bản mới"):pill("ok","Mới nhất"))+'</td></tr>'+unk("Camera",LAT.cam);
 var ups=$("ups");
 if(ups)ups.innerHTML=
  '<div class="item"><div class="t">Bo màn hình<small>Bản mới nhất: '+esc(LAT.ds||"—")+' (bo tự cập nhật khi bật nguồn)</small></div><button class="btn ghost" disabled>Tự cập nhật</button></div>'+
  '<div class="item"><div class="t">DevKit (bánh xe)<small>'+(ver?(dkNew?esc(ver)+" → "+esc(LAT.dk):esc(ver)+", đã mới nhất"):"Chưa đọc được bản đang chạy")+'</small></div>'+
  (dkNew?'<button class="btn" disabled>Tự cập nhật khi bật nguồn</button>':'<button class="btn ghost" disabled>Mới nhất</button>')+'</div>'+
  '<div class="item"><div class="t">Camera<small>Bản mới nhất: '+esc(LAT.cam||"—")+' (bo tự cập nhật khi bật nguồn)</small></div><button class="btn ghost" disabled>Tự cập nhật</button></div>';
 var al=document.querySelector(".alert"),dt=document.querySelector(".drawer .dot");
 if(al){al.style.display=dkNew?"":"none";if(dkNew){var b=al.querySelector("b");if(b)b.textContent="DevKit có bản mới "+LAT.dk+".";var sp=al.querySelector("span");if(sp&&sp.lastChild&&sp.lastChild.nodeType===3)sp.lastChild.textContent=" Robot sẽ tự cập nhật khi bật nguồn lại."}}
 if(dt)dt.style.display=dkNew?"":"none"}
function conn(){var c=$("conn"),n=$("demonote");
 if(c)c.innerHTML='<b></b>'+(ST?"Đã nối Rubi":"Chưa kết nối với Rubi");
 if(n)n.textContent="Đã nối Rubi thật: Điều khiển lái xe, Tình trạng và Cập nhật. Các mục còn lại (đèn, trò chơi, bài hát, truyện, thẻ nhớ, camera) vẫn là mẫu."}
function refresh(){return api("/api/status").then(function(j){ST=j}).catch(function(){ST=null}).then(loadLat).then(function(){render();conn()})}
function watching(){var h=(location.hash||"#trang-chu").slice(1);return h==="tinh-trang"||h==="cap-nhat"||h==="trang-chu"}
addEventListener("hashchange",function(){if(watching())refresh()});
setInterval(function(){if(!document.hidden&&watching())refresh()},5000);
/* ---- Truyện tranh thật: danh sách từ thẻ nhớ của Rubi (qua DevKit), bấm để mở trên màn hình Rubi ---- */
var tyKey="";
function loadTruyen(){var c=document.querySelector("#v-truyen-tranh .card");if(!c)return;
 api("/api/truyen").then(function(j){
  if(j&&j.err==="pin")return;
  var l=(j&&j.list)||[],k=JSON.stringify(l)+(j&&j.co);if(k===tyKey)return;tyKey=k;
  if(!l.length){c.innerHTML='<h2>Truyện tranh</h2>'+note(j&&j.co?"Thẻ nhớ của Rubi chưa có truyện nào.":"Chưa nhận được danh sách từ bo màn hình. Bo gửi mỗi 20 giây khi đang rảnh, đợi một chút rồi thử lại.")}
  else c.innerHTML='<h2>Truyện tranh ('+l.length+')</h2><div class="list">'+l.map(function(b){return '<div class="item"><div class="t">'+esc(b.t)+'</div><button class="btn" data-id="'+esc(b.id)+'">▶ Mở trên Rubi</button></div>'}).join("")+'</div>'+note("Truyện sẽ hiện và được đọc trên màn hình Rubi. Muốn dừng, chạm vào màn hình Rubi.");
 }).catch(function(){})}
document.addEventListener("click",function(e){var b=e.target.closest("#v-truyen-tranh button[data-id]");if(!b)return;
 b.disabled=true;api("/api/truyen?mo="+encodeURIComponent(b.dataset.id)).then(function(j){toast(j&&j.ok?"Đã gửi lệnh, xem màn hình Rubi":"Không mở được truyện")}).catch(function(){toast("Không gọi được Rubi")}).then(function(){setTimeout(function(){b.disabled=false},2500)})});
addEventListener("hashchange",function(){if((location.hash||"").slice(1)==="truyen-tranh")loadTruyen()});
setInterval(function(){if(!document.hidden&&(location.hash||"").slice(1)==="truyen-tranh")loadTruyen()},5000);
if((location.hash||"").slice(1)==="truyen-tranh")loadTruyen();
/* ---- Mọi mục trong menu của Rubi cũng có trên web: lệnh gửi qua DevKit xuống bo màn hình ---- */
function lenh(c,msg){return api("/api/lenh?c="+c).then(function(j){toast(j&&j.ok?(msg||"Đã gửi, xem màn hình Rubi"):"Không gửi được lệnh")}).catch(function(){toast("Không gọi được Rubi")})}
function btns(arr){return '<div class="acts">'+arr.map(function(b){return '<button class="btn" data-c="'+b[0]+'">'+b[1]+'</button>'}).join("")+'</div>'}
var HOLD='<p class="hint">Rubi sẽ mở mục này trên màn hình. Muốn dừng, bấm "Dừng" hoặc chạm màn hình Rubi.</p>';
(function(){
 var c=$("c-toan");if(c)c.innerHTML='<h2>Toán lớp 1</h2>'+btns([["m0","Cộng trừ trong 10"],["m1","Cộng trừ trong 20"],["m2","Cộng trừ trong 100"],["m3","Toán lời văn"]])+HOLD;
 c=$("c-hoc");if(c)c.innerHTML='<h2>Học song ngữ</h2>'+btns([["h0","Trái cây"],["h1","Động vật"],["h2","Đồ vật"],["h3","Hình khối"]])+HOLD;
 var cc=document.querySelector("#v-choi-cung");
 if(cc){var d=document.createElement("div");d.className="card";d.innerHTML='<h2>Đi ngủ, đồng hồ, hẹn giờ</h2>'+btns([["sleep","Đi ngủ (hát ru 30 phút)"],["clock","Đồng hồ"],["t2","Hẹn 2 phút"],["t5","Hẹn 5 phút"],["t10","Hẹn 10 phút"],["t20","Hẹn 20 phút"],["huy","■ Dừng và đóng menu"]])+HOLD;cc.appendChild(d)}
 var st=$("v-cai-dat");
 if(st){var e=document.createElement("div");e.className="card";e.innerHTML='<h2>Âm lượng Rubi</h2>'+btns([["vdown","− Nhỏ bớt"],["vup","+ To hơn"]]);st.insertBefore(e,st.firstChild)}
 document.addEventListener("click",function(ev){var b=ev.target.closest("button[data-c]");if(!b)return;b.disabled=true;lenh(b.dataset.c);setTimeout(function(){b.disabled=false},1500)});
})();
var nhKey="";
function loadNhac(){var c=document.querySelector("#v-bai-hat .card");if(!c)return;
 api("/api/nhac").then(function(j){
  if(j&&j.err==="pin")return;
  var l=(j&&j.list)||[],k=JSON.stringify(l)+(j&&j.co);if(k===nhKey)return;nhKey=k;
  if(!l.length){c.innerHTML='<h2>Bài hát</h2>'+note(j&&j.co?"Thẻ nhớ của Rubi chưa có bài hát nào.":"Chưa nhận được danh sách từ bo màn hình. Bo gửi mỗi 20 giây khi đang rảnh, đợi một chút rồi thử lại.")}
  else c.innerHTML='<h2>Bài hát ('+l.length+')</h2><div class="list">'+l.map(function(b){return '<div class="item"><div class="t">'+esc(b.t)+'</div><button class="btn" data-c="p'+b.i+'">▶ Phát</button></div>'}).join("")+'</div><button class="btn" data-c="huy" style="margin-top:10px;background:#d64545">■ Dừng nhạc</button>'+note("Nhạc phát trên loa Rubi. Muốn dừng, bấm Dừng nhạc hoặc chạm màn hình Rubi.");
 }).catch(function(){})}
addEventListener("hashchange",function(){if((location.hash||"").slice(1)==="bai-hat")loadNhac()});
setInterval(function(){if(!document.hidden&&(location.hash||"").slice(1)==="bai-hat")loadNhac()},5000);
if((location.hash||"").slice(1)==="bai-hat")loadNhac();
/* ---- Khóa khi đang cập nhật: bo màn hình báo "upd" qua DevKit; DevKit tự nạp/khởi động lại thì mất kết nối nên cũng khóa ---- */
var LK=null,okOnce=false,fails=0,verPrev="";
function lockUI(on,t,sub){
 if(!LK){LK=document.createElement("div");LK.setAttribute("style","position:fixed;inset:0;z-index:99999;background:rgba(15,20,30,.92);color:#fff;display:none;align-items:center;justify-content:center;text-align:center;padding:24px;font-family:inherit");
  LK.innerHTML='<div><div style="font-size:42px">⏳</div><div id="lkt" style="font-size:22px;font-weight:700;margin:10px 0"></div><div id="lks" style="font-size:15px;opacity:.85;line-height:1.5"></div></div>';document.body.appendChild(LK)}
 if(on){document.getElementById("lkt").textContent=t;document.getElementById("lks").textContent=sub;if(LK.style.display!=="flex"){stopAll();LK.style.display="flex"}}
 else LK.style.display="none"}
var failAt=0;
function pollUpd(){api("/api/status").then(function(j){
  fails=0;failAt=0;okOnce=true;
  if(!ST||ST.auth!==j.auth){ST=j;render()}else ST=j;
  conn();
  if(j.upd){lockUI(true,"Đang cập nhật bo màn hình","Đừng tắt nguồn Rubi. Mọi tính năng bị khóa cho đến khi cập nhật xong.")}
  else{var was=LK&&LK.style.display==="flex";lockUI(false);
   if(was){toast(j.ver&&verPrev&&j.ver!==verPrev?"Cập nhật xong: DevKit bản "+j.ver:"Đã nối lại, mở khóa");refresh()}}
  if(j.ver)verPrev=String(j.ver)
 }).catch(function(){
  if(!okOnce)return;
  if(++fails<2)return;
  if(!failAt)failAt=Date.now();
  if(Date.now()-failAt<45000)lockUI(true,"Đang cập nhật hoặc khởi động lại","Rubi đang nạp bản mới hoặc vừa mất kết nối. Trang sẽ tự mở khóa khi Rubi nối lại.");
  else{lockUI(false);if(ST){ST=null;render()}}
  conn()})}
setInterval(pollUpd,2500);
refresh();
}else{var c0=$("conn"),n0=$("demonote");
 if(c0){c0.className="pill warn";c0.innerHTML="<b></b>Chưa kết nối với Rubi"}
 if(n0)n0.innerHTML="<b>Chưa kết nối với Rubi.</b> Bạn đang xem bản mẫu: số liệu và nút bấm chưa điều khiển robot. Để điều khiển, nối điện thoại vào cùng Wi-Fi với Rubi rồi mở <b>http://rubi.local</b> hoặc quét mã QR trên màn hình của Rubi."}
})();
