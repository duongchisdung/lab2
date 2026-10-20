/*
 * TH02 - ESP32: doc DHT11 + web ve bieu do duong + xuat CSV + gia lap loi cam bien
 *
 * Chan (theo so do nha kinh): DHT11=GPIO14, SDA=33, SCL=32, ADC=39, nut=36, LED=26
 * Web: / (bieu do), /api/data?since=N, /api/sim?mode=0..3, /export.csv
 * Gia lap loi: 0=tat, 1=mat tin hieu (DHT_READ_FAIL), 2=ngoai dai (OUT_OF_RANGE), 3=ngau nhien
 *   -> dong gia lap duoc danh dau sim=1 trong CSV de khong lan voi loi that.
 * Serial CSV: time_ms,boot_id,seq,temp_c,hum_pct,valid,error,temp_avg5,hum_avg5,dt_ms,sim
 *   (time_ms = ms ke tu luc khoi dong, khong phai gio lich)
 * SENSOR_FROM_I2C=1: so do lay tu Arduino (DHT11 o D8 cua Arduino) qua I2C 0x08, khong doc GPIO14.
 * Lenh Serial: 'i' = quet I2C, 'a' = doc ADC, 'r' = doc tho 8 byte tu slave I2C 0x08
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include <DHT.h>

const char* WIFI_SSID = "VJU Office";
const char* WIFI_PASS = "VJuOffice@2023";
const char* AP_SSID   = "ESP32-dung";
const char* AP_PASS   = "12345678";

#define PIN_DHT 14
#define PIN_SDA 33
#define PIN_SCL 32
#define PIN_ADC 39
#define PIN_BTN 36   // GPIO36 khong co pull-up noi bo: can pull-up ngoai
#define PIN_LED 26
#define SENSOR_FROM_I2C 1   // 1 = lay so do tu Arduino (slave I2C 0x08); 0 = doc DHT11 truc tiep o GPIO14

// Chan dieu khien dong co/bom (MOTOR1: 5,17 | MOTOR2: 23,22 | ARRAY_LED: 12,13). Ep LOW de TAT.
static const int OFF_PINS[] = {5, 17, 23, 22, 12, 13};

const uint32_t PERIOD_MS = 2000;   // chu ky lay mau (DHT11 >= 1 s)
const int N = 5;                   // trung binh truot
const int CAP = 1000;              // so mau luu trong RAM
const int MAX_ROWS = 300;          // toi da so dong tra ve moi lan

struct Sample { uint32_t ms, dt; float t, h, ta, ha; uint8_t ok, err, sim; };
Sample buf[CAP];
uint32_t seq = 0, bootId = 0, lastMs = 0, prevMs = 0, lastBtnMs = 0;
const char* ERR[] = {"", "DHT_READ_FAIL", "OUT_OF_RANGE", "I2C_NO_RESPONSE"};
uint8_t rawFrame[4] = {0, 0, 0, 0};
uint8_t simMode = 0;

DHT dht(PIN_DHT, DHT11);
WebServer server(80);
float tb[N], hb[N];
int cnt = 0, idx = 0;
bool btnPrev = HIGH, ledOn = false;

// ===================== Trang web =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TH02 - Nhiệt độ & độ ẩm</title>
<style>
:root{color-scheme:light;--bg:#f4f5ef;--card:#fff;--tx:#172b24;--mut:#77867d;--bd:#e5e9e2;--ok:#26845e;--bad:#cf5b51;--t:#d47742;--h:#398b9a;--deep:#17372d;--mint:#d6eadb}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
.wrap{max-width:1120px;margin:0 auto;padding:34px 26px 48px}
.top{display:flex;justify-content:space-between;align-items:center;gap:20px;margin-bottom:24px}
.brand{display:flex;align-items:center;gap:12px}.mark{width:42px;height:42px;display:grid;place-items:center;border-radius:14px;background:var(--deep);color:#d9efde;font-size:22px}
.eyebrow{font-size:10px;letter-spacing:2px;font-weight:800;color:var(--ok)}h1{font-size:23px;letter-spacing:-.6px;margin:3px 0 0}
.connection{display:flex;align-items:center;gap:9px;padding:10px 14px;border:1px solid var(--bd);border-radius:30px;background:var(--card);font-size:12px;color:var(--mut)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 4px #e4f2e8}
.connection.offline .dot{background:var(--bad);box-shadow:0 0 0 4px #f9e8e6}.connection.offline{color:var(--bad)}
.intro{display:flex;justify-content:space-between;align-items:end;margin:0 2px 14px}.intro h2{font-size:15px;margin:0 0 4px}.intro p{font-size:12px;color:var(--mut);margin:0}
.stats{display:grid;grid-template-columns:1fr 1fr 1fr 1.25fr;gap:12px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:17px;padding:20px;box-shadow:0 5px 18px rgba(24,53,42,.035)}
.metric{position:relative;min-height:132px;overflow:hidden}.metric:after{content:"";position:absolute;width:76px;height:76px;right:-20px;bottom:-29px;border-radius:50%;background:var(--mint);opacity:.55}
.metric.temp:after{background:#f8e5d7}.metric.humidity:after{background:#d9edf0}
.lbl{font-size:11px;font-weight:650;letter-spacing:.25px;color:var(--mut)}.val{position:relative;z-index:1;margin-top:15px;font-size:34px;line-height:1;font-weight:720;letter-spacing:-1.5px}
.u{font-size:14px;color:var(--mut);font-weight:500;letter-spacing:0}.metric.temp .val{color:var(--t)}.metric.humidity .val{color:var(--h)}
.metric .hint{position:relative;z-index:1;margin-top:10px;font-size:10px;color:var(--mut)}
.quality{display:flex;flex-direction:column;justify-content:space-between}.quality .val{font-size:26px;margin-top:13px;letter-spacing:-.7px}.quality .small{font-size:11px;font-weight:500;color:var(--mut)}
#cer{color:var(--bad)}.state-card{background:var(--deep);border-color:var(--deep);color:#fff}.state-card .lbl{color:#b7cfc0}.state-card .val{font-size:16px;letter-spacing:0;color:#dff2e4}.state-card .hint{color:#a8c1b1}
.section-head{display:flex;justify-content:space-between;align-items:center;gap:16px;margin-bottom:14px}.section-title{font-size:14px;font-weight:700}.section-note{font-size:11px;color:var(--mut);margin-top:4px}
.bar{display:flex;flex-wrap:wrap;gap:9px;align-items:center}
select,button{padding:9px 12px;border-radius:9px;border:1px solid var(--bd);background:#fff;color:var(--tx);font:inherit;font-size:12px}
button:disabled{opacity:.45;cursor:not-allowed}
label{font-size:12px;color:var(--mut)}input[type=checkbox]{accent-color:var(--ok);vertical-align:middle}
button.p{background:var(--deep);color:#fff;border:0;font-weight:650;cursor:pointer}
button.p:hover{background:#285542}.charts{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-top:14px}
.chart-card{padding:18px 18px 10px}.chart-title{display:flex;align-items:center;gap:8px;font-size:13px;font-weight:700}
.chart-icon{display:grid;place-items:center;width:27px;height:27px;border-radius:9px;background:#fbede3;color:var(--t)}
.humidity-chart .chart-icon{background:#e2f1f1;color:var(--h)}canvas{width:100%;height:220px;display:block;margin-top:10px}
.legend{font-size:10px;color:var(--mut);margin-top:10px}.legend i{display:inline-block;width:13px;height:3px;margin:0 4px 3px 9px;vertical-align:middle}
.lg i{display:inline-block;width:13px;height:3px;margin:0 4px 3px 9px;vertical-align:middle}
.tools{display:grid;grid-template-columns:1fr auto;align-items:center;gap:20px;margin-top:14px}
.tools-copy strong{font-size:13px}.tools-copy .lbl{margin-top:5px;font-weight:400;letter-spacing:0}
.sim{border-color:var(--bad);box-shadow:0 0 0 2px #cf5b5114}.msg{min-height:18px;margin:12px 3px 0;color:var(--bad);font-size:12px}
@media(max-width:760px){.wrap{padding:22px 16px 34px}.stats{grid-template-columns:1fr 1fr}.charts{grid-template-columns:1fr}.top{margin-bottom:21px}.metric{min-height:120px}}
@media(max-width:430px){.wrap{padding:17px 12px 28px}.brand{gap:9px}.mark{width:37px;height:37px}h1{font-size:19px}.connection{padding:8px 10px;font-size:10px}.card{padding:16px}.section-head{align-items:flex-start;flex-direction:column}.tools{grid-template-columns:1fr}.tools .bar{justify-content:flex-start}.quality .val{font-size:23px}}
</style></head><body><main class="wrap">
<header class="top">
  <div class="brand"><div class="mark">⌁</div><div><div class="eyebrow">GREENHOUSE / TH02</div><h1>Trung tâm môi trường</h1></div></div>
  <div class="connection" id="connection"><span class="dot"></span><span id="conntext">Đang cập nhật · mỗi 2 giây</span></div>
</header>
<div class="intro"><div><h2>Tổng quan cảm biến</h2><p>Theo dõi điều kiện nhiệt độ và độ ẩm</p></div></div>
<section class="stats" aria-label="Thông số hiện tại">
  <div class="card metric temp"><div class="lbl">NHIỆT ĐỘ</div><div class="val"><span id="t">--</span><span class="u"> °C</span></div><div class="hint">Cảm biến DHT11</div></div>
  <div class="card metric humidity"><div class="lbl">ĐỘ ẨM</div><div class="val"><span id="h">--</span><span class="u"> %</span></div><div class="hint">Độ ẩm không khí</div></div>
  <div class="card quality"><div><div class="lbl">MẪU ĐO · HỢP LỆ / LỖI</div><div class="val"><span id="cok">0</span><span class="small"> / </span><span id="cer">0</span></div></div><div class="lbl">Tổng mẫu trong phiên</div></div>
  <div class="card state-card"><div class="lbl">TRẠNG THÁI CẢM BIẾN</div><div class="val" id="st">--</div><div class="hint">Giám sát theo thời gian thực</div></div>
</section>
<section class="card">
  <div class="section-head"><div><div class="section-title">Lịch sử đo</div><div class="section-note">Biểu đồ cập nhật tự động từ thiết bị</div></div>
    <div class="bar">
      <label>Cửa sổ <select id="win" onchange="win=+this.value;draw()">
        <option value="60">60 mẫu</option><option value="120" selected>120 mẫu</option>
        <option value="300">300 mẫu</option><option value="0">Tất cả</option></select></label>
      <label><input type="checkbox" id="avg" checked onchange="draw()"> Lọc trung bình 5 mẫu</label>
      <button class="p" onclick="location.href='/export.csv'">↓ &nbsp;Xuất CSV</button>
    </div>
  </div>
  <div class="lbl lg"><i style="background:var(--bad);height:9px;width:4px;opacity:.55;margin-left:0"></i>Mẫu lỗi &nbsp;·&nbsp; Nét liền: số đo gốc &nbsp;·&nbsp; Nét đứt: trung bình 5 mẫu</div>
</section>
<section class="charts">
  <div class="card chart-card"><div class="chart-title"><span class="chart-icon">♨</span>Nhiệt độ <span class="lbl">(°C)</span></div><canvas id="cvT"></canvas></div>
  <div class="card chart-card humidity-chart"><div class="chart-title"><span class="chart-icon">◌</span>Độ ẩm <span class="lbl">(%)</span></div><canvas id="cvH"></canvas></div>
</section>
<section class="card tools" id="simcard">
  <div class="tools-copy"><strong>Kiểm thử cảm biến</strong><div class="lbl">Mẫu giả lập được đánh dấu <b>sim=1</b> trong CSV.</div></div>
  <div class="bar"><label for="sim">Chế độ</label><select id="sim" onchange="setSim(this.value)">
    <option value="0">Tắt · đọc thật</option>
    <option value="1">Mất tín hiệu</option>
    <option value="2">Ngoài dải đo</option>
    <option value="3">Ngẫu nhiên ~25%</option></select></div>
</section>
<section class="card tools">
  <div class="tools-copy"><strong>Bộ hồ sơ nộp</strong><div class="lbl">Tải sketch, dữ liệu cảm biến và biểu đồ trước/sau lọc.</div></div>
  <div class="bar">
    <button type="button" onclick="location.href='/sensor_delay.ino'">sensor_delay.ino</button>
    <button type="button" onclick="location.href='/sensor_millis.ino'">sensor_millis.ino</button>
    <button type="button" id="csvBtn" disabled onclick="location.href='/submission.csv'">data.csv</button>
    <button type="button" id="chartBtn" class="p" disabled onclick="downloadChart()">chart.png</button>
  </div>
</section>
<div class="msg" id="msg" role="status"></div>
</main>
<script>
const $=id=>document.getElementById(id);
let D=[],last=0,win=120;
// moi dong: [seq, ms, t, h, ta, ha, ok, err, sim]
async function poll(){
  try{
    const r=await fetch('/api/data?since='+last),j=await r.json();
    if(j.seq<last){D=[];last=0;return poll();}
    j.rows.forEach(x=>D.push(x));
    if(j.rows.length)last=j.rows[j.rows.length-1][0];
    if(D.length>1000)D=D.slice(-1000);
    if(document.activeElement!==$('sim'))$('sim').value=j.sim;
    $('simcard').className='card tools'+(j.sim?' sim':'');
    $('csvBtn').disabled=!!j.sim;
    $('chartBtn').disabled=D.length<2;
    $('connection').className='connection';
    $('conntext').textContent='Đang cập nhật · mỗi 2 giây';
    $('msg').textContent='';
    stats();draw();
  }catch(e){$('connection').className='connection offline';$('conntext').textContent='Mất kết nối';$('msg').textContent='Không thể kết nối tới ESP32';}
}
function stats(){
  let ok=0,er=0;D.forEach(r=>r[6]?ok++:er++);
  $('cok').textContent=ok;$('cer').textContent=er;
  const l=D[D.length-1];if(!l)return;
  $('t').textContent=l[2]==null?'--':l[2].toFixed(1);
  $('h').textContent=l[3]==null?'--':l[3].toFixed(0);
  const names=['Bình thường','DHT_READ_FAIL','OUT_OF_RANGE','I2C_NO_RESPONSE'];
  $('st').textContent=names[l[7]]+(l[8]?' (giả lập)':'');
  $('st').style.color=l[6]?'#dff2e4':'#ffaaa0';
}
const setSim=v=>fetch('/api/sim?mode='+v);
const fmt=ms=>{let s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor(s%3600/60);s%=60;
  return (h?h+':':'')+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');};
function downloadChart(){
  const wasChecked=$('avg').checked;
  if(!wasChecked){$('avg').checked=true;draw();}
  const out=document.createElement('canvas');out.width=1600;out.height=620;
  const g=out.getContext('2d');g.fillStyle='#f4f5ef';g.fillRect(0,0,out.width,out.height);
  g.fillStyle='#172b24';g.font='bold 30px system-ui';g.fillText('TH02 · Biểu đồ cảm biến',42,48);
  g.fillStyle='#77867d';g.font='16px system-ui';g.fillText('Nét liền: số đo gốc  ·  Nét đứt: trung bình trượt 5 mẫu',42,78);
  [['cvT','Nhiệt độ (°C)',30],['cvH','Độ ẩm (%)',815]].forEach(([id,title,x])=>{
    g.fillStyle='#fff';g.fillRect(x,105,755,475);g.strokeStyle='#e5e9e2';g.strokeRect(x,105,755,475);
    g.fillStyle='#172b24';g.font='bold 19px system-ui';g.fillText(title,x+22,139);
    g.drawImage($(id),x+15,155,725,390);
  });
  if(!wasChecked){$('avg').checked=false;draw();}
  out.toBlob(blob=>{
    if(!blob){$('msg').textContent='Không thể tạo ảnh biểu đồ';return;}
    const url=URL.createObjectURL(blob),a=document.createElement('a');
    a.href=url;a.download='chart.png';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  },'image/png');
}
function draw(){
  const rows=win?D.slice(-win):D;
  chart('cvT',rows,2,4,'--t',1);
  chart('cvH',rows,3,5,'--h',0);
}
function chart(id,rows,kr,ka,colVar,dg){
  const c=$(id),dpr=window.devicePixelRatio||1,W=c.clientWidth,H=c.clientHeight;
  c.width=W*dpr;c.height=H*dpr;const g=c.getContext('2d');g.scale(dpr,dpr);
  const cs=getComputedStyle(document.body),mut=cs.getPropertyValue('--mut'),bd=cs.getPropertyValue('--bd'),
        col=cs.getPropertyValue(colVar),bad=cs.getPropertyValue('--bad');
  g.font='11px system-ui';g.fillStyle=mut;
  if(rows.length<2){g.fillText('Đang chờ dữ liệu...',20,30);return;}
  const L=44,R=12,T=8,B=24,pw=W-L-R,ph=H-T-B,x0=rows[0][1],x1=rows[rows.length-1][1];
  const X=ms=>L+(ms-x0)/((x1-x0)||1)*pw;
  let lo=1e9,hi=-1e9;
  rows.forEach(r=>[kr,ka].forEach(k=>{if(r[k]!=null){lo=Math.min(lo,r[k]);hi=Math.max(hi,r[k]);}}));
  if(lo>hi){lo=0;hi=1}if(hi-lo<2){lo-=1;hi+=1}const p=(hi-lo)*.1;lo-=p;hi+=p;
  g.strokeStyle=bd;g.lineWidth=1;g.textAlign='right';
  for(let i=0;i<=4;i++){
    const y=T+ph*i/4;g.beginPath();g.moveTo(L,y);g.lineTo(W-R,y);g.stroke();
    g.fillStyle=mut;g.fillText((hi-(hi-lo)*i/4).toFixed(dg),L-4,y+4);
  }
  g.textAlign='center';g.fillStyle=mut;
  for(let i=0;i<=4;i++)g.fillText(fmt(x0+(x1-x0)*i/4),L+pw*i/4,H-6);
  g.globalAlpha=.25;g.fillStyle=bad;
  rows.forEach(r=>{if(!r[6])g.fillRect(X(r[1])-2,T,4,ph);});
  g.globalAlpha=1;
  const line=(k,dash)=>{
    g.beginPath();g.strokeStyle=col;g.lineWidth=2;g.setLineDash(dash);let pen=false;
    rows.forEach(r=>{if(r[k]==null){pen=false;return;}
      const y=T+ph-(r[k]-lo)/(hi-lo)*ph;pen?g.lineTo(X(r[1]),y):g.moveTo(X(r[1]),y);pen=true;});
    g.stroke();g.setLineDash([]);};
  line(kr,[]);
  if($('avg').checked)line(ka,[6,4]);
}
window.addEventListener('resize',draw);
poll();setInterval(poll,2000);
</script></body></html>
)rawliteral";

const char SENSOR_DELAY_INO[] PROGMEM = R"ino(#include <DHT.h>

const uint8_t DHT_PIN = 14;
const uint8_t DHT_TYPE = DHT11;
const uint8_t FILTER_SIZE = 5;
DHT dht(DHT_PIN, DHT_TYPE);
float tempValues[FILTER_SIZE], humidityValues[FILTER_SIZE];
uint8_t sampleCount = 0, nextIndex = 0;

bool validReading(float t, float h) {
  return !isnan(t) && !isnan(h) && t >= 0 && t <= 50 && h >= 20 && h <= 90;
}

float average(const float *values) {
  float total = 0;
  for (uint8_t i = 0; i < sampleCount; i++) total += values[i];
  return total / sampleCount;
}

void printRow(uint32_t timestamp, float raw, float filtered, const char *unit, bool valid) {
  Serial.print(timestamp);
  Serial.print(',');
  if (valid) Serial.print(raw, 1);
  Serial.print(',');
  if (valid) Serial.print(filtered, 2);
  Serial.print(',');
  Serial.print(unit);
  Serial.print(',');
  Serial.println(valid ? 1 : 0);
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println("timestamp,raw,converted,unit,valid");
}

void loop() {
  delay(2000);
  const uint32_t timestamp = millis();
  const float t = dht.readTemperature();
  const float h = dht.readHumidity();
  const bool valid = validReading(t, h);

  if (valid) {
    tempValues[nextIndex] = t;
    humidityValues[nextIndex] = h;
    nextIndex = (nextIndex + 1) % FILTER_SIZE;
    if (sampleCount < FILTER_SIZE) sampleCount++;
  }

  printRow(timestamp, t, valid ? average(tempValues) : NAN, "C", valid);
  printRow(timestamp, h, valid ? average(humidityValues) : NAN, "%", valid);
}
)ino";

const char SENSOR_MILLIS_INO[] PROGMEM = R"ino(#include <DHT.h>

const uint8_t DHT_PIN = 14;
const uint8_t DHT_TYPE = DHT11;
const uint8_t FILTER_SIZE = 5;
const uint32_t SAMPLE_PERIOD_MS = 2000;
DHT dht(DHT_PIN, DHT_TYPE);
float tempValues[FILTER_SIZE], humidityValues[FILTER_SIZE];
uint8_t sampleCount = 0, nextIndex = 0;
uint32_t lastSampleMs = 0;

bool validReading(float t, float h) {
  return !isnan(t) && !isnan(h) && t >= 0 && t <= 50 && h >= 20 && h <= 90;
}

float average(const float *values) {
  float total = 0;
  for (uint8_t i = 0; i < sampleCount; i++) total += values[i];
  return total / sampleCount;
}

void printRow(uint32_t timestamp, float raw, float filtered, const char *unit, bool valid) {
  Serial.print(timestamp);
  Serial.print(',');
  if (valid) Serial.print(raw, 1);
  Serial.print(',');
  if (valid) Serial.print(filtered, 2);
  Serial.print(',');
  Serial.print(unit);
  Serial.print(',');
  Serial.println(valid ? 1 : 0);
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println("timestamp,raw,converted,unit,valid");
}

void loop() {
  const uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_PERIOD_MS) return;
  lastSampleMs = now;

  const float t = dht.readTemperature();
  const float h = dht.readHumidity();
  const bool valid = validReading(t, h);

  if (valid) {
    tempValues[nextIndex] = t;
    humidityValues[nextIndex] = h;
    nextIndex = (nextIndex + 1) % FILTER_SIZE;
    if (sampleCount < FILTER_SIZE) sampleCount++;
  }

  printRow(now, t, valid ? average(tempValues) : NAN, "C", valid);
  printRow(now, h, valid ? average(humidityValues) : NAN, "%", valid);
}
)ino";

// ===================== Cam bien =====================
void field(float v, int d) { if (!isnan(v)) Serial.print(v, d); }

void scanI2c() {
  int found = 0;
  Serial.printf("# I2C scan SDA=%d SCL=%d\n", PIN_SDA, PIN_SCL);
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("# I2C found 0x%02X\n", a); found++; }
  }
  Serial.printf("# I2C: %d thiet bi\n", found);
}

void readAdc() {
  int raw = analogRead(PIN_ADC);
  Serial.printf("# ADC GPIO%d raw=%d volt=%.2fV%s\n", PIN_ADC, raw, raw * 3.3f / 4095,
                (raw < 5 || raw > 4090) ? " (cham gioi han dai do)" : "");
}

#define I2C_SLAVE_ADDR 0x08   // nghi la ATmega8/Arduino giu DHT11 (chua kiem chung)
// Doc tho 8 byte tu slave de biet khung du lieu (lenh Serial 'r')
void probeSlave() {
  uint8_t n = Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)8);
  Serial.printf("# I2C 0x%02X tra ve %d byte:", I2C_SLAVE_ADDR, n);
  while (Wire.available()) Serial.printf(" %02X", Wire.read());
  Serial.println();
}

// Doc 4 byte tu Arduino. GIA DINH (suy ra tu khung 01 05 02 08): nhiet do*10 va do am*10,
// moi gia tri 16 bit big-endian  =>  0x0105 = 26.1 C, 0x0208 = 52.0 %. Can doi chieu voi sketch Arduino.
// Tra ve: 0 = ok, 1 = slave khong tra loi, 2 = slave bao so do khong hop le (0xFFFF)
uint8_t readSlave(float &t, float &h) {
  uint8_t n = Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)4);
  if (n < 4) { while (Wire.available()) Wire.read(); t = NAN; h = NAN; return 1; }
  for (int i = 0; i < 4; i++) rawFrame[i] = Wire.read();
  uint16_t rt = (rawFrame[0] << 8) | rawFrame[1];
  uint16_t rh = (rawFrame[2] << 8) | rawFrame[3];
  if (rt == 0xFFFF || rh == 0xFFFF) { t = NAN; h = NAN; return 2; }
  t = rt / 10.0f; h = rh / 10.0f;
  return 0;
}

void sample() {
  uint32_t now = millis();
  Sample &m = buf[seq % CAP];
  m.ms = now;
  m.dt = seq ? now - prevMs : 0;
  prevMs = now;

  float h, t;
#if SENSOR_FROM_I2C
  uint8_t st = readSlave(t, h);
#else
  h = dht.readHumidity(); t = dht.readTemperature();
  uint8_t st = (isnan(h) || isnan(t)) ? 2 : 0;
#endif

  // Gia lap loi (danh dau sim=1)
  m.sim = 0;
  bool inject = (simMode == 1 || simMode == 2) || (simMode == 3 && random(100) < 25);
  if (inject) {
    m.sim = 1;
    int kind = (simMode == 3) ? random(1, 3) : simMode;
    if (kind == 1) { t = NAN; h = NAN; }
    else { t = 85; h = 5; }
  }

  m.err = (st == 1 && !m.sim) ? 3
        : ((st == 2 || isnan(t) || isnan(h)) ? 1 : ((t < 0 || t > 50 || h < 20 || h > 90) ? 2 : 0));
  m.ok = (m.err == 0);
  m.ta = NAN; m.ha = NAN;
  if (m.ok) {
    tb[idx] = t; hb[idx] = h;
    idx = (idx + 1) % N;
    if (cnt < N) cnt++;
    float st = 0, sh = 0;
    for (int i = 0; i < cnt; i++) { st += tb[i]; sh += hb[i]; }
    m.ta = st / cnt; m.ha = sh / cnt;
  } else {
    t = NAN; h = NAN;          // loi -> null, khong ghi 0
  }
  m.t = t; m.h = h;
  seq++;

  Serial.printf("%lu,%lu,%lu,", (unsigned long)m.ms, (unsigned long)bootId, (unsigned long)seq);
  field(m.t, 1);  Serial.print(',');
  field(m.h, 1);  Serial.print(',');
  Serial.printf("%d,%s,", m.ok, ERR[m.err]);
  field(m.ta, 2); Serial.print(',');
  field(m.ha, 2); Serial.print(',');
  Serial.printf("%lu,%d\n", (unsigned long)m.dt, m.sim);
}

void handleInputs() {
  static uint32_t lowSince = 0;
  uint32_t now = millis();
  bool b = digitalRead(PIN_BTN);
  if (b == LOW) { if (!lowSince) lowSince = now; } else { lowSince = 0; btnPrev = HIGH; }
  if (btnPrev == HIGH && lowSince && now - lowSince >= 40 && now > 1500 && now - lastBtnMs > 200) {
    btnPrev = LOW;                       // chot: moi lan nhan chi tinh 1 lan
    lastBtnMs = now;
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn);
    Serial.printf("# BTN tai %lu ms\n", (unsigned long)now);
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'i') scanI2c();
    else if (c == 'a') readAdc();
    else if (c == 'r') probeSlave();
  }
}

// ===================== API =====================
String num(float v, int d, const char* nul) { return isnan(v) ? String(nul) : String(v, d); }

void handleData() {
  uint32_t since = server.arg("since").toInt();
  uint32_t first = (seq > CAP) ? seq - CAP + 1 : 1;
  uint32_t start = max(since + 1, first);
  if (seq >= start && seq - start + 1 > MAX_ROWS) start = seq - MAX_ROWS + 1;

  String j;
  j.reserve(MAX_ROWS * 48 + 80);
  j += "{\"seq\":" + String(seq) + ",\"sim\":" + String(simMode) + ",\"rows\":[";
  for (uint32_t q = start; q <= seq; q++) {
    const Sample &m = buf[(q - 1) % CAP];
    if (q != start) j += ',';
    j += "[" + String(q) + "," + String(m.ms) + "," + num(m.t, 1, "null") + "," + num(m.h, 1, "null") +
         "," + num(m.ta, 2, "null") + "," + num(m.ha, 2, "null") + "," + String(m.ok) + "," +
         String(m.err) + "," + String(m.sim) + "]";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleSim() {
  if (server.hasArg("mode")) simMode = constrain(server.arg("mode").toInt(), 0, 3);
  Serial.printf("# SIM mode=%d\n", simMode);
  server.send(200, "application/json", "{\"sim\":" + String(simMode) + "}");
}

void handleExport() {
  server.sendHeader("Content-Disposition", "attachment; filename=th02.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  String s = "time_ms,boot_id,seq,temp_c,hum_pct,valid,error,temp_avg5,hum_avg5,dt_ms,sim\n";
  uint32_t first = (seq > CAP) ? seq - CAP + 1 : 1;
  for (uint32_t q = first; q <= seq; q++) {
    const Sample &m = buf[(q - 1) % CAP];
    s += String(m.ms) + "," + String(bootId) + "," + String(q) + "," + num(m.t, 1, "") + "," +
         num(m.h, 1, "") + "," + String(m.ok) + "," + ERR[m.err] + "," + num(m.ta, 2, "") + "," +
         num(m.ha, 2, "") + "," + String(m.dt) + "," + String(m.sim) + "\n";
    if (s.length() > 1500) { server.sendContent(s); s = ""; }
  }
  server.sendContent(s);
  server.sendContent("");
}

void handleSubmissionCsv() {
  if (simMode != 0) {
    server.send(409, "text/plain; charset=utf-8", "Tat che do gia lap truoc khi xuat data.csv.");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=data.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  String s = "timestamp,raw,converted,unit,valid\n";
  uint32_t first = (seq > CAP) ? seq - CAP + 1 : 1;
  for (uint32_t q = first; q <= seq; q++) {
    const Sample &m = buf[(q - 1) % CAP];
    s += String(m.ms) + "," + num(m.t, 1, "") + "," + num(m.ta, 2, "") + ",C," + String(m.ok) + "\n";
    s += String(m.ms) + "," + num(m.h, 1, "") + "," + num(m.ha, 2, "") + ",%," + String(m.ok) + "\n";
    if (s.length() > 1200) { server.sendContent(s); s = ""; }
  }
  server.sendContent(s);
  server.sendContent("");
}

// ===================== setup / loop =====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("# Dang ket noi WiFi %s ", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(300); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("# IP: http://");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("# WiFi that bai -> AP '");
    Serial.print(AP_SSID);
    Serial.print("' http://");
    Serial.println(WiFi.softAPIP());
  }
}

void setup() {
  Serial.begin(115200);
  for (int p : OFF_PINS) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }   // tat may bom/dong co
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  analogReadResolution(12);

  Preferences p;
  p.begin("th02", false);
  bootId = p.getUInt("boot", 0) + 1;
  p.putUInt("boot", bootId);
  p.end();

  Wire.begin(PIN_SDA, PIN_SCL);
#if !SENSOR_FROM_I2C
  pinMode(PIN_DHT, INPUT);               // khong bat pull-up noi bo de thu dung duong keo len cua mach
  delay(50);
  Serial.printf("# GPIO%d nhan roi = %d (1 = DATA duoc keo len; 0 = bi keo xuong/chap/chua co nguon)\n",
                PIN_DHT, digitalRead(PIN_DHT));
  dht.begin();
#endif
  Serial.printf("# boot_id=%lu period=%lums\n", (unsigned long)bootId, (unsigned long)PERIOD_MS);
  Serial.println("time_ms,boot_id,seq,temp_c,hum_pct,valid,error,temp_avg5,hum_avg5,dt_ms,sim");
  scanI2c();
  probeSlave();
  readAdc();

  connectWiFi();
  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/sensor_delay.ino", []() {
    server.sendHeader("Content-Disposition", "attachment; filename=sensor_delay.ino");
    server.send_P(200, "text/plain; charset=utf-8", SENSOR_DELAY_INO);
  });
  server.on("/sensor_millis.ino", []() {
    server.sendHeader("Content-Disposition", "attachment; filename=sensor_millis.ino");
    server.send_P(200, "text/plain; charset=utf-8", SENSOR_MILLIS_INO);
  });
  server.on("/submission.csv", handleSubmissionCsv);
  server.on("/api/data", handleData);
  server.on("/api/sim", handleSim);
  server.on("/export.csv", handleExport);
  server.onNotFound([]() { server.send(404, "text/plain", ""); });
  server.begin();
}

void loop() {
  server.handleClient();
  handleInputs();
  uint32_t now = millis();
  if (now - lastMs >= PERIOD_MS) {
    lastMs = now;
    sample();
    if (seq % 5 == 0) {
      readAdc();
#if SENSOR_FROM_I2C
      Serial.printf("# RAW %02X %02X %02X %02X\n", rawFrame[0], rawFrame[1], rawFrame[2], rawFrame[3]);
#endif
    }
  }
}