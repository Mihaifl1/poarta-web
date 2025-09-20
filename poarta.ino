/*******************************************************
 * ESP8266 Gate Controller – Calibrare cu feedback
 * - MQTT (OPEN/CLOSE/STOP/STATUS?)
 * - Releu impuls 1s
 * - 2 x PC817 capete (configurabil LOW/HIGH)
 * - Web UI: senzorii live, Start/Force Start, mesaje răspuns
 * - Calibrare: măsoară, oprește la capăt, Save
 *******************************************************/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

/* ------------ WiFi ------------ */
const char* ssid = "1";
const char* pass = "";

/* ------------ MQTT ------------ */
const char* mqtt_host = "broker.emqx.io";
const int   mqtt_port = 1883;
const char* client_id = "esp_gate_1";
const char* cmdTopic      = "kablem/gates/1/cmd";
const char* statusTopic   = "kablem/gates/1/status";
const char* progressTopic = "kablem/gates/1/progress";

WiFiClient espClient;
PubSubClient mqtt(espClient);
ESP8266WebServer server(80);

/* ------------ PINI & LOGICĂ ------------ */
#define RELAY_PIN          D5     // GPIO14
#define RELAY_ACTIVE_HIGH  0      // 0=activ LOW (obișnuit la module releu), 1=activ HIGH

#define OPEN_SENSE_PIN     D1     // GPIO5  – senzor „poartă DESCHISĂ”
#define CLOSED_SENSE_PIN   D2     // GPIO4  – senzor „poartă ÎNCHISĂ”
#define SENSE_ACTIVE_LOW   1      // <<<< dacă PC817 îți dă HIGH la capăt, pune 0

/* ------------ STARE ------------ */
String state = "STOP";           // STOP | OPENING | CLOSING | OPEN | CLOSED | FAULT
int progress = 0;                // 0..100
unsigned long moveStartMs = 0;

/* ------------ Releu impuls 1s ------------ */
unsigned long relayOnMs = 0;
bool relayActive = false;
const unsigned long RELAY_PULSE_MS = 1000;

/* ------------ Senzori / progres ------------ */
unsigned long lastSenseMs = 0;
const unsigned long senseEveryMs = 60;

unsigned long lastProgMs = 0;
const unsigned long progPublishMs = 220;

/* ------------ Config & Calibrare ------------ */
struct Config { unsigned long tOpenMs = 8000, tCloseMs = 8000; } cfg;
const char* CFG_PATH = "/gate_cfg.json";

enum Calib { CAL_IDLE, CAL_MEAS_OPEN, CAL_MEAS_CLOSE, CAL_DONE_OPEN, CAL_DONE_CLOSE };
Calib calibState = CAL_IDLE;

unsigned long calibStartMs = 0;
unsigned long measOpenMs   = 0;
unsigned long measCloseMs  = 0;

/* ------------ Helpers pini ------------ */
void relayWrite(bool on){
#if RELAY_ACTIVE_HIGH
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
#else
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
#endif
}
void pulseRelay(){ relayWrite(true); relayActive = true; relayOnMs = millis(); }
void updateRelay(){ if (relayActive && millis() - relayOnMs >= RELAY_PULSE_MS){ relayWrite(false); relayActive=false; } }

bool rawRead(uint8_t pin){ return digitalRead(pin); }
bool senseActive(uint8_t pin){
  bool r = rawRead(pin);
  return SENSE_ACTIVE_LOW ? (r==LOW) : (r==HIGH);
}
bool isOpenLimit()   { return senseActive(OPEN_SENSE_PIN); }
bool isClosedLimit() { return senseActive(CLOSED_SENSE_PIN); }

/* ------------ Persistență ------------ */
void loadConfig(){
  if (!LittleFS.begin()){ Serial.println("LittleFS mount fail"); return; }
  if (!LittleFS.exists(CFG_PATH)){ Serial.println("No cfg, defaults"); return; }
  File f = LittleFS.open(CFG_PATH, "r"); if (!f){ Serial.println("Open cfg fail"); return; }
  StaticJsonDocument<256> d; if (deserializeJson(d, f)){ Serial.println("JSON err"); f.close(); return; }
  f.close();
  cfg.tOpenMs  = d["tOpen"]  | cfg.tOpenMs;
  cfg.tCloseMs = d["tClose"] | cfg.tCloseMs;
  Serial.printf("Config: tOpen=%lums, tClose=%lums\n", cfg.tOpenMs, cfg.tCloseMs);
}
bool saveConfig(){
  StaticJsonDocument<256> d; d["tOpen"]=cfg.tOpenMs; d["tClose"]=cfg.tCloseMs;
  File f = LittleFS.open(CFG_PATH, "w"); if (!f){ Serial.println("Save cfg fail"); return false; }
  serializeJson(d, f); f.close(); Serial.println("Config saved"); return true;
}

/* ------------ MQTT ------------ */
void publishStatus(const char* s){ mqtt.publish(statusTopic, s, true); }
void publishProgress(int p){ char b[8]; snprintf(b, sizeof(b), "%d", p); mqtt.publish(progressTopic, b, false); }
void setState(const String& s){ if (state != s){ state=s; publishStatus(state.c_str()); } }

void handleCmd(const String& raw){
  String cmd = raw; cmd.trim(); cmd.toUpperCase();
  if (cmd == "OPEN"){
    if (isOpenLimit()){ progress=100; setState("OPEN"); publishProgress(progress); return; }
    setState("OPENING"); moveStartMs = millis(); pulseRelay();
  } else if (cmd == "CLOSE"){
    if (isClosedLimit()){ progress=0; setState("CLOSED"); publishProgress(progress); return; }
    setState("CLOSING"); moveStartMs = millis(); pulseRelay();
  } else if (cmd == "STOP"){
    setState("STOP"); relayWrite(false); relayActive=false;
  } else if (cmd == "STATUS?"){
    publishStatus(state.c_str()); publishProgress(progress);
  }
}
void mqttCallback(char* topic, byte* payload, unsigned int len){
  String msg; msg.reserve(len); for (unsigned int i=0;i<len;i++) msg += (char)payload[i];
  if (String(topic)==cmdTopic) handleCmd(msg);
}
void reconnectMqtt(){
  while(!mqtt.connected()){
    if (mqtt.connect(client_id)){ mqtt.subscribe(cmdTopic,1); publishStatus("ONLINE"); }
    else delay(1200);
  }
}

/* ------------ Senzori + progres ------------ */
void updateFromSensors(){
  if (millis() - lastSenseMs < senseEveryMs) return;
  lastSenseMs = millis();

  bool oL = isOpenLimit();
  bool cL = isClosedLimit();

  if (oL && cL){ setState("FAULT"); relayWrite(false); relayActive=false; return; }

  // finalizează calibrarea
  if (calibState == CAL_MEAS_OPEN && oL){
    measOpenMs = millis() - calibStartMs; calibState = CAL_DONE_OPEN;
  }
  if (calibState == CAL_MEAS_CLOSE && cL){
    measCloseMs = millis() - calibStartMs; calibState = CAL_DONE_CLOSE;
  }

  if (oL){ progress=100; setState("OPEN");   publishProgress(progress); }
  else if (cL){ progress=0; setState("CLOSED"); publishProgress(progress); }
}

void updateProgressByTime(){
  if (millis() - lastProgMs < progPublishMs) return;
  lastProgMs = millis();

  if (state == "OPENING" && cfg.tOpenMs>0 && !isOpenLimit()){
    unsigned long e = millis() - moveStartMs;
    int p = (int)(e * 100.0 / cfg.tOpenMs); if (p>99) p=99; if (p<0) p=0;
    if (p != progress){ progress=p; publishProgress(progress); }
  } else if (state == "CLOSING" && cfg.tCloseMs>0 && !isClosedLimit()){
    unsigned long e = millis() - moveStartMs;
    int p = 100 - (int)(e * 100.0 / cfg.tCloseMs); if (p<1) p=1; if (p>100) p=100;
    if (p != progress){ progress=p; publishProgress(progress); }
  }
}

/* ------------ Web UI (cu feedback) ------------ */
const char HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Poartă – Calibrare</title>
<style>
body{font-family:system-ui;background:#0b1220;color:#e8f0ff;margin:0}
.wrap{max-width:560px;margin:0 auto;padding:16px}
.card{background:#0e1b33;border:1px solid #1d2d55;border-radius:12px;padding:14px;margin-top:12px}
.btn{padding:10px 14px;border-radius:10px;border:0;background:#173a7a;color:#fff;font-weight:700;margin:4px}
.btn.alt{background:#333}
.row{display:flex;gap:8px;flex-wrap:wrap}
.badge{display:inline-block;padding:4px 8px;border-radius:8px;background:#1b2b4f;margin-left:8px}
.ok{background:#1f6d3a}.err{background:#7a1f1f}.warn{background:#6a5b18}
small{color:#9fb0d8}
#msg{margin-top:8px}
.ind{display:inline-block;padding:4px 8px;border-radius:8px}
.up{background:#274a8a} .on{background:#1f6d3a} .off{background:#7a1f1f}
</style></head><body><div class="wrap">
<h2>Poartă – Calibrare</h2>

<div class="card">
  <div>Stare: <span id="st">—</span> <span class="badge">Progress: <span id="pr">0</span>%</span></div>
  <div style="margin-top:8px"><progress id="pg" max="100" value="0" style="width:100%"></progress></div>
  <div style="margin-top:6px">
    Senzor OPEN: <span id="so" class="ind off">OFF</span> •
    Senzor CLOSED: <span id="sc" class="ind off">OFF</span>
  </div>
  <div class="row">
    <button class="btn" onclick="cmd('OPEN')">OPEN</button>
    <button class="btn alt" onclick="cmd('CLOSE')">CLOSE</button>
    <button class="btn alt" onclick="cmd('STOP')">STOP</button>
  </div>
</div>

<div class="card">
  <b>Calibrare OPEN</b>
  <div>Pornește din <b>CLOSED</b>. Apasă <i>Start</i> – timpul curge; la capăt se oprește singur.</div>
  <div style="margin:6px 0">Timp măsurat: <b><span id="mo">—</span> ms</b> <span id="co" class="badge up">idle</span></div>
  <div class="row">
    <button class="btn" onclick="calibStart('open',0)">Start</button>
    <button class="btn alt" onclick="calibStart('open',1)">Force start</button>
    <button class="btn alt" onclick="save('open')">Save OPEN</button>
  </div>
</div>

<div class="card">
  <b>Calibrare CLOSE</b>
  <div>Pornește din <b>OPEN</b>. Apasă <i>Start</i> – timpul curge; la capăt se oprește.</div>
  <div style="margin:6px 0">Timp măsurat: <b><span id="mc">—</span> ms</b> <span id="cc" class="badge up">idle</span></div>
  <div class="row">
    <button class="btn" onclick="calibStart('close',0)">Start</button>
    <button class="btn alt" onclick="calibStart('close',1)">Force start</button>
    <button class="btn alt" onclick="save('close')">Save CLOSE</button>
  </div>
  <div id="msg" class="badge warn" style="display:none"></div>
</div>

<div class="card">
  <b>Timpi salvați</b>
  <div>OPEN: <b><span id="to">—</span> ms</b> • CLOSE: <b><span id="tc">—</span> ms</b></div>
</div>

<script>
function showMsg(t,cls){ const m=msg; m.textContent=t; m.className='badge '+cls; m.style.display='inline-block'; setTimeout(()=>m.style.display='none',3000); }
async function cmd(c){ const r=await fetch('/api/cmd?c='+c); showMsg(await r.text(),'up'); }
async function calibStart(dir,force){
  const r = await fetch('/api/calib_start?dir='+dir+'&force='+force);
  const t = await r.text();
  showMsg(t, r.ok?'ok':'err');
}
async function save(dir){ const r=await fetch('/api/calib_save?dir='+dir); showMsg(await r.text(), r.ok?'ok':'err'); await tick(); }
function badge(el, txt){ el.textContent = txt; el.className='badge '+(txt==='measuring'?'warn':(txt==='done'?'ok':'up')); }
function ind(el, on){ el.textContent=on?'ON':'OFF'; el.className='ind '+(on?'on':'off'); }
async function tick(){
  const r = await fetch('/api/state'); const j = await r.json();
  st.textContent=j.state; pr.textContent=j.progress; pg.value=j.progress;
  to.textContent=j.tOpenMs; tc.textContent=j.tCloseMs;
  ind(so, j.openLimit); ind(sc, j.closedLimit);
  mo.textContent = j.measOpenMs>=0 ? j.measOpenMs : '—';  badge(co, j.calibStateOpen);
  mc.textContent = j.measCloseMs>=0 ? j.measCloseMs : '—'; badge(cc, j.calibStateClose);
}
setInterval(tick, 400); tick();
</script>
</div></body></html>
)HTML";

void handleRoot(){ server.send_P(200, "text/html", HTML); }

void sendState(){
  StaticJsonDocument<448> d;
  d["state"]    = state;
  d["progress"] = progress;
  d["tOpenMs"]  = cfg.tOpenMs;
  d["tCloseMs"] = cfg.tCloseMs;

  const char* csOpen  = (calibState==CAL_MEAS_OPEN)  ? "measuring" :
                        (calibState==CAL_DONE_OPEN)   ? "done" : "idle";
  const char* csClose = (calibState==CAL_MEAS_CLOSE) ? "measuring" :
                        (calibState==CAL_DONE_CLOSE)  ? "done" : "idle";
  d["calibStateOpen"]  = csOpen;
  d["calibStateClose"] = csClose;
  d["measOpenMs"]  = (calibState==CAL_MEAS_OPEN)  ? (int)(millis()-calibStartMs) :
                     (calibState==CAL_DONE_OPEN)   ? (int)measOpenMs  : -1;
  d["measCloseMs"] = (calibState==CAL_MEAS_CLOSE) ? (int)(millis()-calibStartMs) :
                     (calibState==CAL_DONE_CLOSE)  ? (int)measCloseMs : -1;

  d["openLimit"]   = isOpenLimit();
  d["closedLimit"] = isClosedLimit();

  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

// Start calibrare: dir=open/close, optional force=1 ignoră verificarea poziției
void apiCalibStart(){
  String dir = server.hasArg("dir") ? server.arg("dir") : "";
  bool force = server.hasArg("force") ? (server.arg("force")=="1") : false;

  if (dir=="open"){
    if (!force && !isClosedLimit()){ server.send(400, "text/plain", "Trebuie CLOSED (senzor CLOSED=ON)"); return; }
    calibState = CAL_MEAS_OPEN; measOpenMs=0; calibStartMs=millis();
    setState("OPENING"); moveStartMs=millis(); pulseRelay();
    server.send(200, "text/plain", force?"Force start OPEN":"Start OPEN");
  } else if (dir=="close"){
    if (!force && !isOpenLimit()){ server.send(400, "text/plain", "Trebuie OPEN (senzor OPEN=ON)"); return; }
    calibState = CAL_MEAS_CLOSE; measCloseMs=0; calibStartMs=millis();
    setState("CLOSING"); moveStartMs=millis(); pulseRelay();
    server.send(200, "text/plain", force?"Force start CLOSE":"Start CLOSE");
  } else {
    server.send(400, "text/plain", "Param dir lipsa");
  }
}

// Save rezultat (după „done”)
void apiCalibSave(){
  String dir = server.hasArg("dir") ? server.arg("dir") : "";
  if (dir=="open"  && calibState==CAL_DONE_OPEN){
    cfg.tOpenMs = measOpenMs; saveConfig(); calibState=CAL_IDLE; server.send(200,"text/plain","Saved OPEN");
  } else if (dir=="close" && calibState==CAL_DONE_CLOSE){
    cfg.tCloseMs = measCloseMs; saveConfig(); calibState=CAL_IDLE; server.send(200,"text/plain","Saved CLOSE");
  } else {
    server.send(400,"text/plain","Nimic de salvat (rulează START și așteaptă DONE)");
  }
}

void apiCmd(){ String c = server.hasArg("c")?server.arg("c"):""; handleCmd(c); server.send(200,"text/plain","OK"); }

/* ------------ Setup / Loop ------------ */
void setup(){
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT); relayWrite(false);
  pinMode(OPEN_SENSE_PIN,   INPUT_PULLUP);
  pinMode(CLOSED_SENSE_PIN, INPUT_PULLUP);

  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("WiFi"); while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.printf(" OK %s\n", WiFi.localIP().toString().c_str());

  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setCallback(mqttCallback);
  reconnectMqtt();
  publishStatus("ONLINE");

  server.on("/", handleRoot);
  server.on("/api/state", sendState);
  server.on("/api/cmd", apiCmd);
  server.on("/api/calib_start", apiCalibStart);
  server.on("/api/calib_save", apiCalibSave);
  server.begin();
}

void loop(){
  server.handleClient();
  if (!mqtt.connected()) reconnectMqtt();
  mqtt.loop();

  updateRelay();
  updateFromSensors();
  updateProgressByTime();
}
