#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_wifi.h>

// =========================================================
// VERSIONEN
// =========================================================
#define SW_VERSION  "1.0"
#define HW_VERSION  "0.1"

// =========================================================
// WLAN
// =========================================================
const char* ssid     = "N57Oilpress";
const char* password = "D3848367";

// =========================================================
// BOOT-KALIBRIERUNG
// =========================================================
#define ENABLE_BOOT_CAL   0          // 1 = Auto-Kalibrierung
#define CAL_R_KNOWN       9.7f      // Sensor ~0 bar

// =========================================================
// PINS
// =========================================================
#define LED_WIFI_OK   3
#define LED_CLIENT    2
#define OIL_ADC       4
#define ONE_WIRE_BUS  5

// =========================================================
// ADC / HARDWARE
// =========================================================

// Spannung am oberen Ende des Spannungsteilers (gemessen!)
const float V_SUPPLY = 4.983f;

// ADC-Kalibrierung (aus Messpunkten ermittelt)
// Vadc = ADC_GAIN * adc + ADC_OFFSET
const float ADC_GAIN   = 0.000860761f;
const float ADC_OFFSET = 0.0288261f;

// Pullup-Widerstand (auto-kalibriert oder Fallback)
float R_PULLUP = 223.0f;

// VDO Messbereich
const float MAX_BAR = 5.0f;



// =========================================================
// GLOBALS
// =========================================================
DNSServer dnsServer;
WebServer server(80);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// =========================================================
// ESP32 DIE TEMP (DEBUG)
// =========================================================
float espTempC() {
    return temperatureRead();
}

// =========================================================
// VDO LUT
// =========================================================
struct VdoPoint { float ohm; float bar; };

const VdoPoint VDO_TABLE[] = {
    { 10.0f, 0.0f },
    { 43.0f, 1.0f },
    { 79.0f, 2.0f },
    {111.0f, 3.0f },
    {136.0f, 4.0f },
    {184.0f, 5.0f }
};
const int VDO_SIZE = sizeof(VDO_TABLE) / sizeof(VdoPoint);

// =========================================================
// ADC → SPANNUNG → RSENSOR
// =========================================================
float adcToVoltage(uint16_t adc) {
    float v = ADC_GAIN * (float)adc + ADC_OFFSET;
    if (v < 0.0f) v = 0.0f;
    if (v > V_SUPPLY) v = V_SUPPLY;   // clamp
    return v;
}


float adcToRsensor(uint16_t adc) {
    float v = adcToVoltage(adc);
    if (v < 0.05f) return NAN;
    if (v > (V_SUPPLY - 0.05f)) return NAN;

    // 5V -> R_PULLUP -> Rsensor -> GND
    return R_PULLUP * v / (V_SUPPLY - v);
}


// =========================================================
// RSENSOR → BAR
// =========================================================
float rsensorToBar(float R) {
    if (isnan(R)) return NAN;
    if (R <= VDO_TABLE[0].ohm) return 0.0f;
    if (R >= VDO_TABLE[VDO_SIZE - 1].ohm) return MAX_BAR;

    for (int i = 0; i < VDO_SIZE - 1; i++) {
        if (R >= VDO_TABLE[i].ohm && R <= VDO_TABLE[i + 1].ohm) {
            return VDO_TABLE[i].bar +
                   (R - VDO_TABLE[i].ohm) *
                   (VDO_TABLE[i + 1].bar - VDO_TABLE[i].bar) /
                   (VDO_TABLE[i + 1].ohm - VDO_TABLE[i].ohm);
        }
    }
    return NAN;
}

// =========================================================
// IIR FILTER
// =========================================================
float oilIIR(float v) {
    static float last = NAN;
    const float alpha = 0.2f;
    if (isnan(last)) last = v;
    else last += alpha * (v - last);
    return last;
}

// =========================================================
// PULLUP-KALIBRIERUNG
// =========================================================
float calibratePullup(float Rs_known) {
    float sum = 0;
    int cnt = 0;
    delay(300);

    for (int i = 0; i < 16; i++) {
        float v = adcToVoltage(analogRead(OIL_ADC));
        if (v < 0.05f || v > 1.6f) continue;

        float rpu = Rs_known * (V_SUPPLY / v - 1.0f);
        if (rpu > 50 && rpu < 500) {
            sum += rpu;
            cnt++;
        }
        delay(5);
    }
    return cnt ? sum / cnt : R_PULLUP;
}



// =========================================================
// HTML UI
// =========================================================
const char MAIN_PAGE[] PROGMEM = R"====(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>N57 Oilpressure</title>

<style>
/* =======================================================
   Farb-Variablen (Default = TAG = GRÜN)
   ======================================================= */
:root{
    --main-color:#00ff66;
    --dim-color:rgba(0,255,102,0.55);
    --soft-color:rgba(0,255,102,0.35);
    --warn-color:#ff2a2a;
}

/* ================= NACHT = BMW-AMBER ================= */
body[data-theme="amber"]{
    --main-color:#ff7a18;
    --dim-color:rgba(255,122,24,0.55);
    --soft-color:rgba(255,122,24,0.35);
}

/* ================= Grundlayout ================= */
body{
    background:#000;
    color:var(--main-color);
    font-family:Arial, sans-serif;
    text-align:center;
    margin:0;
}

h1{
    font-weight:normal;
    letter-spacing:1px;
    color:var(--dim-color);
}

/* ================= Boxen ================= */
.box{
    border:1px solid var(--soft-color);
    margin:14px;
    padding:18px;
    border-radius:14px;
}

/* ================= Hauptanzeige ================= */
.flex{
    display:flex;
    justify-content:center;
    align-items:center;
    gap:50px;
}

.value{
    font-size:64px;
    font-weight:bold;
    color:var(--main-color);
    text-shadow:0 0 10px var(--dim-color);
}

.unit{
    font-size:26px;
    color:var(--dim-color);
    margin-top:-6px;
}

/* ================= Warnung ================= */
span.warn{
    color:var(--warn-color);
    text-shadow:0 0 12px rgba(255,42,42,0.9);
}

span.warn.blink{
    animation:blink-red 1s infinite;
}

@keyframes blink-red{
    0%,50%,100%{opacity:1}
    25%,75%{opacity:0}
}

/* ================= Instrument ================= */
svg path{ stroke:var(--soft-color); }
svg line{
    stroke:var(--main-color);
    filter:drop-shadow(0 0 6px var(--dim-color));
}
svg circle{ fill:var(--main-color); }

.scale text{
    fill:var(--dim-color);
    font-size:14px;
}

/* ================= Debug ================= */
.box.debug{
    color:var(--dim-color);
    border-color:var(--soft-color);
    font-size:24px;
}

.debuggrid{
    display:grid;
    grid-template-columns:
        max-content max-content 100px max-content max-content;
    row-gap:12px;
    column-gap:8px;
    justify-content:start;
    text-align:left;
}
</style>

<script>
/* ================= TAG / NACHT ================= */
function updateThemeByTime(){
    const h = new Date().getHours();
    if(h >= 20 || h < 6){
        document.body.setAttribute("data-theme","amber");
    }else{
        document.body.setAttribute("data-theme","green");
    }
}

/* ================= Schwellen ================= */
const OIL_WARN_BAR = 0.8;
const TEMP_WARN_C = 120;

/* ================= Zeiger ================= */
function setNeedle(bar){
    if(bar===null) return;
    let a = -90 + (bar/5)*180;
    document.getElementById("needle")
        .setAttribute("transform","rotate("+a+" 150 140)");
}

/* ================= Update ================= */
async function update(){
    let j = await (await fetch("/data.json")).json();

    for(let k in j){
        let e=document.getElementById(k);
        if(e) e.innerText=j[k] ?? "--";
    }

    let oil=document.getElementById("oil_bar");
    oil.classList.remove("warn","blink");

    if(j.oil_bar!==null && j.oil_bar < OIL_WARN_BAR){
        oil.classList.add("warn","blink");
    }

    let temp=document.getElementById("oil_temp");
    temp.classList.remove("warn");
    if(j.oil_temp!==null && j.oil_temp > TEMP_WARN_C){
        temp.classList.add("warn");
    }

    setNeedle(j.oil_bar);
}

document.addEventListener("DOMContentLoaded", () => {
    updateThemeByTime();
    setInterval(updateThemeByTime, 300000);
    setInterval(update, 1000);
});

</script>
</head>

<body>

<h1>N57 Oilpressure</h1>

<!-- ================= ÖLDRUCK ================= -->
<div class="box flex">
<svg width="340" height="200" viewBox="0 0 300 170">
    <path d="M40 140 A110 110 0 0 1 260 140"
          fill="none" stroke-width="10"/>

    <g class="scale">
        <text x="40"  y="150">0</text>
        <text x="80"  y="80">1</text>
        <text x="130" y="50">2</text>
        <text x="170" y="50">3</text>
        <text x="220" y="80">4</text>
        <text x="255" y="150">5</text>
    </g>

    <line id="needle" x1="150" y1="140" x2="150" y2="45" stroke-width="5"/>
    <circle cx="150" cy="140" r="7"/>
</svg>

<div>
    <div class="value"><span id="oil_bar">--</span></div>
    <div class="unit">bar</div>
</div>
</div>

<!-- ================= ÖLTEMPERATUR ================= -->
<div class="box">
    <div class="value"><span id="oil_temp">--</span></div>
    <div class="unit">°C Oil</div>
</div>

<!-- ================= DEBUG ================= -->
<div class="box debug">
<b>Debug</b><br><br>
<div class="debuggrid">

<div>ADC:</div><div><span id="adc"></span></div>
<div></div>
<div>Vadc:</div><div><span id="vadc"></span> V</div>

<div>Rsensor:</div><div><span id="rs"></span> Ω</div>
<div></div>
<div>Bar raw:</div><div><span id="bar_raw"></span></div>

<div>ESP Temp:</div><div><span id="esp_temp"></span> °C</div>
<div></div>
<div>V_SUPPLY:</div><div><span id="v_supply"></span> V</div>

<div>R_PULLUP:</div><div><span id="rpu"></span> Ω</div>
<div></div><div></div><div></div>

<div>SW-Version:</div><div><span id="sw"></span></div>
<div></div>
<div>HW-Version:</div><div><span id="hw"></span></div>

</div>
</div>

</body>
</html>
)====";




// =========================================================
// JSON
// =========================================================
void handleDataJson() {
    uint16_t adc = analogRead(OIL_ADC);
    float vadc = adcToVoltage(adc);
    float Rs = adcToRsensor(adc);
    float bar_raw = rsensorToBar(Rs);
    float bar = oilIIR(bar_raw);

    sensors.requestTemperatures();
    float oilTemp = sensors.getTempCByIndex(0);
    if (oilTemp < -100) oilTemp = NAN;

    String j = "{";
    j += "\"adc\":"+String(adc)+",";
    j += "\"vadc\":"+String(vadc,3)+",";
    j += "\"rs\":"+(isnan(Rs)?"null":String(Rs,1))+",";
    j += "\"bar_raw\":"+(isnan(bar_raw)?"null":String(bar_raw,2))+",";
    j += "\"oil_bar\":"+(isnan(bar)?"null":String(bar,2))+",";
    j += "\"oil_temp\":"+(isnan(oilTemp)?"null":String(oilTemp,1))+",";
    j += "\"esp_temp\":"+String(espTempC(),1)+",";
    j += "\"v_supply\":"+String(V_SUPPLY,3)+",";
    j += "\"rpu\":"+String(R_PULLUP,1)+",";
    j += "\"sw\":\""+String(SW_VERSION)+"\",";
    j += "\"hw\":\""+String(HW_VERSION)+"\"}";
    server.send(200,"application/json",j);
}

// =========================================================
// SETUP / LOOP
// =========================================================
void setup() {
    Serial.begin(115200);
    pinMode(LED_WIFI_OK,OUTPUT);
    pinMode(LED_CLIENT,OUTPUT);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    sensors.begin();

    #if ENABLE_BOOT_CAL
        R_PULLUP = calibratePullup(CAL_R_KNOWN);
    #else
        R_PULLUP = 220.0f;
    #endif

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid,password);
    digitalWrite(LED_WIFI_OK,HIGH);

    dnsServer.start(53,"*",WiFi.softAPIP());
    server.on("/",[]{server.send(200,"text/html",MAIN_PAGE);});
    server.on("/data.json",handleDataJson);
    server.begin();
}

void loop() {
    dnsServer.processNextRequest();
    server.handleClient();
    wifi_sta_list_t l;
    esp_wifi_ap_get_sta_list(&l);
    digitalWrite(LED_CLIENT,l.num>0);
}
