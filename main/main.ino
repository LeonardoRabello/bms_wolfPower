#include <INA219.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>

// ─── WiFi Access Point ───────────────────────────────────────────────────────
const char* AP_SSID     = "BMS-ESP32";
const char* AP_PASSWORD = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);
WebServer server(80);

// ─── INA219 ──────────────────────────────────────────────────────────────────
INA219 INA1(0x40);
INA219 INA2(0x41);
INA219 INA3(0x44);

INA219* sensores[] = {&INA1, &INA2, &INA3};
const char* nomes[] = {"Bateria 1", "Bateria 2", "Bateria 3"};

// ─── Pinos ───────────────────────────────────────────────────────────────────
const int PINO_RELE[3]  = {25, 26, 27};
const int PINO_MOSFET   = 33;
const int PINO_TERMISTOR = 34;

// ─── Limites BMS ─────────────────────────────────────────────────────────────
const float TENSAO_MAX   = 4.2;
const float TENSAO_MIN   = 3.0;
const float CORRENTE_MAX = 2000;
const float TEMP_MAX     = 60.0;
const float DELTA_BAL    = 0.05;

// ─── Estado global ───────────────────────────────────────────────────────────
struct Medicao {
  int   bateria;
  float tensao;
  float corrente;
};

Medicao leituras[3];
float tempPack = 0;
bool sistemaAtivo = true;

// ════════════════════════════════════════════════════════════════════════════
// RELÉS
// ════════════════════════════════════════════════════════════════════════════
void setRele(int i, bool ligar) {
  if (i < 0 || i > 2) return;
  digitalWrite(PINO_RELE[i], ligar ? HIGH : LOW);
}

void desligarTodosReles() {
  for (int i = 0; i < 3; i++) setRele(i, false);
}

// ════════════════════════════════════════════════════════════════════════════
// CORTE DO SISTEMA (MOSFET — só a carga)
// ════════════════════════════════════════════════════════════════════════════
void cortarSistema(bool cortar) {
  sistemaAtivo = !cortar;
  digitalWrite(PINO_MOSFET, cortar ? LOW : HIGH);
  if (cortar) {
    desligarTodosReles();
    Serial.println("[BMS] Sistema CORTADO!");
  } else {
    Serial.println("[BMS] Sistema ATIVADO!");
  }
}

// ════════════════════════════════════════════════════════════════════════════
// TEMPERATURA DO PACK
// ════════════════════════════════════════════════════════════════════════════
float lerTemperatura() {
  int raw = analogRead(PINO_TERMISTOR);
  float tensaoADC = raw * (3.3 / 4095.0);
  return tensaoADC * (100.0 / 3.3); // substituir por curva NTC real
}

// ════════════════════════════════════════════════════════════════════════════
// MEDIÇÃO
// ════════════════════════════════════════════════════════════════════════════
Medicao medir(int i) {
  Medicao m;
  m.bateria  = i + 1;
  m.tensao   = sensores[i]->getBusVoltage();
  m.corrente = sensores[i]->getCurrent_mA();
  return m;
}

// ════════════════════════════════════════════════════════════════════════════
// BALANCEAMENTO
// ════════════════════════════════════════════════════════════════════════════
void balancear() {
  float tensoes[3];
  float media = 0;
  for (int i = 0; i < 3; i++) {
    tensoes[i] = leituras[i].tensao;
    media += tensoes[i];
  }
  media /= 3.0;
  for (int i = 0; i < 3; i++) {
    bool drenar = (tensoes[i] > media + DELTA_BAL);
    setRele(i, drenar);
    if (drenar)
      Serial.printf("[BAL] Rele %d LIGADO (%.3fV > media %.3fV)\n", i+1, tensoes[i], media);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PROTEÇÕES + RECUPERAÇÃO AUTOMÁTICA
// ════════════════════════════════════════════════════════════════════════════
bool condicoesNormais() {
  for (int i = 0; i < 3; i++) {
    if (leituras[i].tensao   > TENSAO_MAX)   return false;
    if (leituras[i].tensao   < TENSAO_MIN)   return false;
    if (abs(leituras[i].corrente) > CORRENTE_MAX) return false;
  }
  if (tempPack > TEMP_MAX) return false;
  return true;
}

void verificarProtecoes() {
  for (int i = 0; i < 3; i++) {
    if (leituras[i].tensao > TENSAO_MAX || leituras[i].tensao < TENSAO_MIN) {
      Serial.printf("[PROT] %s - tensao fora: %.2fV\n", nomes[i], leituras[i].tensao);
      cortarSistema(true); return;
    }
    if (abs(leituras[i].corrente) > CORRENTE_MAX) {
      Serial.printf("[PROT] %s - sobrecorrente: %.1fmA\n", nomes[i], leituras[i].corrente);
      cortarSistema(true); return;
    }
  }
  if (tempPack > TEMP_MAX) {
    Serial.printf("[PROT] Superaquecimento: %.1fC\n", tempPack);
    cortarSistema(true);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// SERVIDOR WEB
// ════════════════════════════════════════════════════════════════════════════
void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BMS Dashboard</title>
<style>
  *{box-sizing:border-box;}
  body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:0;padding:12px;}
  h1{text-align:center;color:#4fc3f7;margin:8px 0;}
  #status{text-align:center;font-size:1.1em;margin:6px 0;}
  .ok{color:#66bb6a;} .err{color:#ef5350;}
  #temp{text-align:center;font-size:1em;color:#ffb74d;margin-bottom:10px;}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;max-width:860px;margin:auto;}
  .card{background:#1e1e1e;border-radius:12px;padding:12px;}
  .card h3{margin:0 0 8px;font-size:.95em;color:#aaa;text-align:center;}
  canvas{display:block;width:100%!important;}
</style>
</head>
<body>
<h1>BMS Dashboard</h1>
<div id="status" class="ok">Sistema: ATIVO</div>
<div id="temp">Temperatura do Pack: --°C</div>
<div class="grid">
  <div class="card"><h3>Tensao (V)</h3>     <canvas id="cV" height="160"></canvas></div>
  <div class="card"><h3>Corrente (mA)</h3>  <canvas id="cI" height="160"></canvas></div>
  <div class="card"><h3>Pack Total (V)</h3> <canvas id="cP" height="160"></canvas></div>
</div>

<script>
const LABELS = ['Bat1','Bat2','Bat3'];
const CORES  = ['#4fc3f7','#aed581','#ffb74d'];

function barChart(id, data, minV, maxV) {
  var cv  = document.getElementById(id);
  var ctx = cv.getContext('2d');
  var W   = cv.parentElement.clientWidth - 24;
  cv.width = W; cv.height = 160;
  ctx.fillStyle = '#1e1e1e'; ctx.fillRect(0,0,W,160);
  var pad=32, top=14, bot=138;
  var slots = 3, slotW = (W - pad) / slots;
  var barW  = Math.floor(slotW * 0.6);
  var range = (maxV - minV) || 1;
  for (var i = 0; i < 3; i++) {
    var x = pad + i * slotW + (slotW - barW) / 2;
    var h = Math.max(2, ((data[i] - minV) / range) * (bot - top));
    ctx.fillStyle = CORES[i];
    ctx.beginPath();
    if (ctx.roundRect) ctx.roundRect(x, bot-h, barW, h, 4);
    else ctx.rect(x, bot-h, barW, h);
    ctx.fill();
    ctx.fillStyle = '#eee'; ctx.font = '11px Arial'; ctx.textAlign = 'center';
    ctx.fillText(data[i].toFixed(2), x + barW/2, bot - h - 4);
    ctx.fillStyle = '#aaa'; ctx.font = '11px Arial';
    ctx.fillText(LABELS[i], x + barW/2, 155);
  }
  ctx.strokeStyle = '#333'; ctx.lineWidth = 1;
  for (var g = 0; g <= 4; g++) {
    var y = bot - (g/4)*(bot-top);
    ctx.beginPath(); ctx.moveTo(pad, y); ctx.lineTo(W-2, y); ctx.stroke();
    ctx.fillStyle = '#666'; ctx.font = '9px Arial'; ctx.textAlign = 'right';
    ctx.fillText((minV + g/4 * range).toFixed(1), pad-2, y+3);
  }
}

function packChart(id, data) {
  var cv  = document.getElementById(id);
  var ctx = cv.getContext('2d');
  var W   = cv.parentElement.clientWidth - 24;
  cv.width = W; cv.height = 160;
  ctx.fillStyle = '#1e1e1e'; ctx.fillRect(0,0,W,160);
  var cx = W/2, cy = 72, r = 58;
  var total = data.reduce(function(a,b){return a+b;}, 0);
  var start = -Math.PI/2;
  for (var i = 0; i < 3; i++) {
    var slice = (data[i]/total)*2*Math.PI;
    ctx.beginPath(); ctx.moveTo(cx,cy);
    ctx.arc(cx, cy, r, start, start+slice);
    ctx.closePath(); ctx.fillStyle = CORES[i]; ctx.fill();
    start += slice;
  }
  ctx.beginPath(); ctx.arc(cx, cy, 28, 0, 2*Math.PI);
  ctx.fillStyle = '#1e1e1e'; ctx.fill();
  ctx.fillStyle = '#eee'; ctx.font = 'bold 12px Arial'; ctx.textAlign = 'center';
  ctx.fillText(total.toFixed(2)+'V', cx, cy+5);
  for (var i = 0; i < 3; i++) {
    var lx = 4 + (i%2)*(W/2), ly = 140 + Math.floor(i/2)*14;
    ctx.fillStyle = CORES[i]; ctx.fillRect(lx, ly, 10, 8);
    ctx.fillStyle = '#aaa'; ctx.font = '10px Arial'; ctx.textAlign = 'left';
    ctx.fillText(LABELS[i]+' '+data[i].toFixed(2)+'V', lx+13, ly+8);
  }
}

async function atualizar() {
  try {
    var r = await fetch('/dados');
    var d = await r.json();
    var tv = d.baterias.map(function(b){return b.tensao;});
    var tc = d.baterias.map(function(b){return b.corrente;});
    barChart('cV', tv, Math.min.apply(null,tv)-0.1, Math.max.apply(null,tv)+0.1);
    barChart('cI', tc, Math.min(0,Math.min.apply(null,tc)), Math.max.apply(null,tc)+10);
    packChart('cP', tv);
    document.getElementById('temp').textContent = 'Temperatura do Pack: ' + d.temperatura.toFixed(1) + '°C';
    var st = document.getElementById('status');
    st.textContent = 'Sistema: ' + (d.ativo ? 'ATIVO' : 'CORTADO');
    st.className   = d.ativo ? 'ok' : 'err';
  } catch(e) { console.log(e); }
}

atualizar();
setInterval(atualizar, 1000);
</script>
</body></html>
)rawhtml";
  server.send(200, "text/html", html);
}

void handleDados() {
  String json = "{\"ativo\":" + String(sistemaAtivo ? "true" : "false")
              + ",\"temperatura\":" + String(tempPack, 1)
              + ",\"baterias\":[";
  for (int i = 0; i < 3; i++) {
    json += "{\"id\":"       + String(i + 1)
          + ",\"tensao\":"   + String(leituras[i].tensao,   3)
          + ",\"corrente\":" + String(leituras[i].corrente, 2)
          + "}";
    if (i < 2) json += ",";
  }
  float totalV = 0;
  for (int i = 0; i < 3; i++) totalV += leituras[i].tensao;
  json += "],\"totalV\":" + String(totalV, 3) + "}";
  server.send(200, "application/json", json);
}

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Wire.begin();

  for (int i = 0; i < 3; i++) {
    pinMode(PINO_RELE[i], OUTPUT);
    digitalWrite(PINO_RELE[i], LOW);
  }
  pinMode(PINO_MOSFET, OUTPUT);
  digitalWrite(PINO_MOSFET, HIGH); // carga ligada ao iniciar

  for (int i = 0; i < 3; i++) {
    if (!sensores[i]->begin()) {
      Serial.printf("%s: INA219 nao encontrado!\n", nomes[i]);
    } else {
      sensores[i]->setMaxCurrentShunt(2, 0.1);
      Serial.printf("%s: iniciado!\n", nomes[i]);
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("AP: '%s' | Senha: '%s'\n", AP_SSID, AP_PASSWORD);
  Serial.printf("Acesse: http://%s\n", AP_IP.toString().c_str());

  server.on("/",      handleRoot);
  server.on("/dados", handleDados);
  server.begin();
}

// ════════════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  for (int i = 0; i < 3; i++) leituras[i] = medir(i);
  tempPack = lerTemperatura();

  if (sistemaAtivo) {
    verificarProtecoes();
    balancear();
  } else if (condicoesNormais()) {
    // recuperação automática quando tudo voltar ao normal
    cortarSistema(false);
  }

  float totalV = 0;
  for (int i = 0; i < 3; i++) {
    totalV += leituras[i].tensao;
    Serial.printf("[%s] %.3fV | %.1fmA\n", nomes[i], leituras[i].tensao, leituras[i].corrente);
  }
  Serial.printf("[PACK] %.3fV | %.1fC | %s\n\n", totalV, tempPack, sistemaAtivo ? "ATIVO" : "CORTADO");

  delay(500);
}