#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

#define SS_PIN   5
#define RST_PIN  22
#define LED_PIN  2

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;
const String NOME = "hirrua";
const String BASE = "sis1a/" + NOME + "/rfid/";

const uint8_t MAX_TAGS = 10;
const unsigned long CADASTRO_TIMEOUT_MS = 15000;
const unsigned long DEBOUNCE_MS = 2000;
const unsigned long RECONEXAO_MS = 3000;

MFRC522 rfid(SS_PIN, RST_PIN);
WiFiClient net;
PubSubClient mqtt(net);

String tags[MAX_TAGS];
uint8_t totalTags = 0;
String mac;

enum Modo { NORMAL, CADASTRO };
Modo modo = NORMAL;
unsigned long cadastroInicio = 0;

String ultimoUid = "";
unsigned long ultimaLeitura = 0;

bool ledLigado = false;
unsigned long ledAte = 0;
uint8_t piscadasRestantes = 0;
unsigned long ultimoToggle = 0;

unsigned long ultimaTentativaWifi = 0;
unsigned long ultimaTentativaMqtt = 0;

void setLed(bool on) {
  ledLigado = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void atualizarLed() {
  unsigned long agora = millis();
  if (modo == CADASTRO) {
    if (agora - ultimoToggle >= 250) {
      ultimoToggle = agora;
      setLed(!ledLigado);
    }
    return;
  }
  if (piscadasRestantes > 0) {
    if (agora - ultimoToggle >= 150) {
      ultimoToggle = agora;
      setLed(!ledLigado);
      piscadasRestantes--;
    }
    return;
  }
  setLed(agora < ledAte);
}

void ledLiberado() {
  piscadasRestantes = 0;
  ledAte = millis() + 2000;
}

void ledNegado() {
  ledAte = 0;
  setLed(false);
  piscadasRestantes = 6;
  ultimoToggle = millis() - 150;
}

String hora() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return "00:00:00";
  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

String nomeModo() {
  return modo == CADASTRO ? "CADASTRO" : "NORMAL";
}

void publicar(const char* sub, const String& payload, bool retain = false) {
  String topico = BASE + sub;
  bool ok = mqtt.publish(topico.c_str(), payload.c_str(), retain);
  Serial.printf("[MQTT] %s %s -> %s\n", ok ? "OK  " : "FALHA", topico.c_str(), payload.c_str());
}

void publicarMac() {
  String json = "{\"mac\":\"" + mac + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"hora\":\"" + hora() + "\"}";
  publicar("mac", json, true);
}

void publicarCadastro(const String& uid, const String& status) {
  String json = "{\"mac\":\"" + mac + "\",\"uid\":\"" + uid + "\",\"status\":\"" + status + "\",\"hora\":\"" + hora() + "\"}";
  publicar("cadastro", json);
}

void publicarAcesso(const String& uid, bool liberado) {
  String json = "{\"mac\":\"" + mac + "\",\"uid\":\"" + uid + "\",\"resultado\":\"" + (liberado ? "liberado" : "negado") + "\",\"hora\":\"" + hora() + "\"}";
  publicar("acesso", json);
}

int buscarTag(const String& uid) {
  for (uint8_t i = 0; i < totalTags; i++) {
    if (tags[i] == uid) return i;
  }
  return -1;
}

String normalizarUid(String s) {
  s.trim();
  s.toUpperCase();
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  return s;
}

String lerUid() {
  String s;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i > 0) s += ' ';
    if (rfid.uid.uidByte[i] < 0x10) s += '0';
    s += String(rfid.uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void mudarModo(Modo novo) {
  modo = novo;
  ledAte = 0;
  piscadasRestantes = 0;
  setLed(false);
  Serial.printf("[MODO] %s\n", nomeModo().c_str());
}

void iniciarCadastro() {
  cadastroInicio = millis();
  ultimoToggle = millis();
  mudarModo(CADASTRO);
  Serial.println("[MODO] Aguardando cartao por 15 s...");
}

void verificarTimeoutCadastro() {
  if (modo == CADASTRO && millis() - cadastroInicio >= CADASTRO_TIMEOUT_MS) {
    publicarCadastro("", "tempo_esgotado");
    mudarModo(NORMAL);
  }
}

void removerTag(const String& uid) {
  int idx = buscarTag(uid);
  if (idx < 0) {
    publicarCadastro(uid, "nao_encontrada");
    return;
  }
  for (uint8_t i = idx; i < totalTags - 1; i++) tags[i] = tags[i + 1];
  totalTags--;
  tags[totalTags] = "";
  publicarCadastro(uid, "removida");
}

void listarTags() {
  String json = "{\"mac\":\"" + mac + "\",\"tags\":[";
  for (uint8_t i = 0; i < totalTags; i++) {
    if (i > 0) json += ",";
    json += "\"" + tags[i] + "\"";
  }
  json += "]}";
  publicar("lista", json);
}

void onMensagem(char* topico, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.printf("[CMD] Recebido em %s: %s\n", topico, cmd.c_str());

  String up = cmd;
  up.toUpperCase();

  if (up == "CADASTRO") {
    iniciarCadastro();
  } else if (up.startsWith("REMOVER")) {
    String uid = normalizarUid(cmd.substring(7));
    if (uid.length() == 0) {
      Serial.println("[CMD] REMOVER sem UID");
      return;
    }
    removerTag(uid);
  } else if (up == "LISTAR") {
    listarTags();
  } else {
    Serial.println("[CMD] Comando desconhecido");
  }
}

void processarCartao(const String& uid) {
  Serial.printf("[RFID] UID lido: %s (%d bytes) | modo %s\n", uid.c_str(), rfid.uid.size, nomeModo().c_str());

  if (modo == CADASTRO) {
    String status;
    if (buscarTag(uid) >= 0) {
      status = "ja_cadastrada";
    } else if (totalTags >= MAX_TAGS) {
      status = "lista_cheia";
    } else {
      tags[totalTags++] = uid;
      status = "cadastrada";
    }
    publicarCadastro(uid, status);
    mudarModo(NORMAL);
    return;
  }

  bool liberado = buscarTag(uid) >= 0;
  publicarAcesso(uid, liberado);
  if (liberado) ledLiberado();
  else ledNegado();
}

void lerCartao() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = lerUid();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  unsigned long agora = millis();
  if (uid == ultimoUid && agora - ultimaLeitura < DEBOUNCE_MS) {
    Serial.printf("[RFID] %s ignorado (lido ha menos de 2 s)\n", uid.c_str());
    return;
  }
  ultimoUid = uid;
  ultimaLeitura = agora;
  processarCartao(uid);
}

void conectarWifi() {
  Serial.printf("[WIFI] Conectando em %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] Conectado | IP %s\n", WiFi.localIP().toString().c_str());
}

void sincronizarHora() {
  configTime(-3 * 3600, 0, "pool.ntp.org");
  Serial.print("[NTP] Sincronizando");
  struct tm t;
  unsigned long inicio = millis();
  while (!getLocalTime(&t, 500) && millis() - inicio < 15000) Serial.print(".");
  Serial.printf("\n[NTP] Hora local: %s\n", hora().c_str());
}

void garantirWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - ultimaTentativaWifi < RECONEXAO_MS) return;
  ultimaTentativaWifi = millis();
  Serial.println("[WIFI] Desconectado, tentando reconectar...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
}

void garantirMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - ultimaTentativaMqtt < RECONEXAO_MS) return;
  ultimaTentativaMqtt = millis();

  String clientId = "esp32-" + NOME + "-" + String((uint32_t)esp_random(), HEX);
  Serial.printf("[MQTT] Conectando em %s:%d...\n", MQTT_HOST, MQTT_PORT);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("[MQTT] Conectado");
    String topicoCmd = BASE + "cmd";
    mqtt.subscribe(topicoCmd.c_str());
    Serial.printf("[MQTT] Inscrito em %s\n", topicoCmd.c_str());
    publicarMac();
  } else {
    Serial.printf("[MQTT] Falha, rc=%d. Nova tentativa em 3 s\n", mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  Serial.println("[RFID] MFRC522 iniciado");

  conectarWifi();
  mac = WiFi.macAddress();
  Serial.printf("[WIFI] MAC: %s\n", mac.c_str());

  sincronizarHora();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMensagem);
  mqtt.setBufferSize(1024);

  Serial.printf("[MODO] %s\n", nomeModo().c_str());
}

void loop() {
  garantirWifi();
  garantirMqtt();
  mqtt.loop();
  verificarTimeoutCadastro();
  lerCartao();
  atualizarLed();
}