// ============================================================
// Monitoramento estrutural de ponte/trapiche - ESP32 + 2x MPU6050
// v3: média móvel, histerese, níveis de alerta, detecção de
//     falha de sensor (fail-safe) e rede não bloqueante.
// ============================================================
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_system.h>
#include "secrets.h"

// ===== PINOS =====
const int SDA1_PIN = 21;   // Wire  -> sensor 1 (pilar 1)
const int SCL1_PIN = 22;
const int SDA2_PIN = 19;   // Wire1 -> sensor 2 (pilar 3)
const int SCL2_PIN = 4;
const int SERVO_PIN = 18;

// ===== CANCELA =====
const int SERVO_FECHADA = 90;
const int SERVO_ABERTA  = 180;

// ===== LIMITES (graus) =====
const float LIM_ATENCAO = 2.5;  // amarelo
const float LIM_PERIGO  = 4.0;  // vermelho (fecha a cancela)
const float HISTERESE   = 1.0;  // só desce de nível quando cair este valor abaixo do limite

// ===== FILTRO / TEMPOS =====
const int JANELA = 8;                              // leituras na média móvel
const unsigned long INTERVALO_LEITURA = 100;       // ms
const unsigned long INTERVALO_ENVIO   = 1000;      // ms
const unsigned long REPETE_WIFI   = 15000;         // ms entre tentativas de WiFi
const unsigned long REPETE_MQTT   = 5000;          // ms entre tentativas de MQTT
const unsigned long REPETE_SENSOR = 5000;          // ms entre tentativas de reiniciar sensor

// ===== DETECÇÃO DE FALHA =====
const uint8_t FALHAS_MAX    = 3;    // leituras ruins seguidas até declarar falha
const uint8_t CONGELADO_MAX = 30;   // leituras idênticas seguidas (sensor travado)

// ===== NÍVEIS =====
enum Nivel { NIVEL_NORMAL = 0, NIVEL_ATENCAO, NIVEL_PERIGO, NIVEL_FALHA };
const char* NOMES_NIVEL[] = {"normal", "atencao", "perigo", "falha_sensor"};

// ===== MÉDIA MÓVEL =====
struct Filtro {
  float buf[JANELA];
  int idx = 0;
  int n = 0;

  float add(float v) {
    buf[idx] = v;
    idx = (idx + 1) % JANELA;
    if (n < JANELA) n++;
    float soma = 0;
    for (int i = 0; i < n; i++) soma += buf[i];
    return soma / n;
  }
  void reset() { idx = 0; n = 0; }
};

// ===== SENSOR =====
struct Sensor {
  Adafruit_MPU6050 mpu;
  TwoWire* bus = nullptr;
  const char* nome = "";
  uint8_t addr = 0x68;
  bool ok = false;
  bool calibrado = false;
  float offset = 0;
  float angulo = 0;             // ângulo filtrado (graus)
  Filtro filtro;
  uint8_t falhas = 0;
  uint8_t repetidas = 0;
  float ultX = 0, ultY = 0, ultZ = 0;
  unsigned long ultimaTentativa = 0;
};

Sensor s1, s2;
Servo meuServo;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

Nivel nivelAngulo = NIVEL_NORMAL;     // nível calculado só pelos ângulos (com histerese)
Nivel nivelAtual = NIVEL_NORMAL;      // nível final (inclui falha de sensor)
Nivel nivelReportado = NIVEL_NORMAL;
bool cancelaFechada = true;

unsigned long ultimaLeitura = 0;
unsigned long ultimoEnvio = 0;
unsigned long ultimaTentativaWiFi = 0;
unsigned long ultimaTentativaMQTT = 0;

// ------------------------------------------------------------
// Utilidades de sensor
// ------------------------------------------------------------
float anguloDe(const sensors_event_t &a) {
  return atan2(a.acceleration.x, a.acceleration.y) * 180.0 / PI;
}

bool responde(Sensor &s) {
  s.bus->beginTransmission(s.addr);
  return s.bus->endTransmission() == 0;
}

bool iniciarSensor(Sensor &s) {
  const uint8_t enderecos[2] = {0x68, 0x69};
  for (uint8_t i = 0; i < 2; i++) {
    if (s.mpu.begin(enderecos[i], s.bus)) {
      s.addr = enderecos[i];
      s.mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
      s.mpu.setGyroRange(MPU6050_RANGE_500_DEG);
      s.mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
      return true;
    }
  }
  return false;
}

// Define a posição atual como "zero" (média de 30 leituras)
bool calibrar(Sensor &s) {
  float soma = 0;
  int validas = 0;
  for (int i = 0; i < 30; i++) {
    sensors_event_t a, g, t;
    if (responde(s) && s.mpu.getEvent(&a, &g, &t)) {
      soma += anguloDe(a);
      validas++;
    }
    delay(10);
  }
  if (validas < 20) return false;
  s.offset = soma / validas;
  s.calibrado = true;
  return true;
}

void tentarRecuperar(Sensor &s) {
  if (millis() - s.ultimaTentativa < REPETE_SENSOR) return;
  s.ultimaTentativa = millis();

  if (!iniciarSensor(s)) return;
  if (!s.calibrado && !calibrar(s)) return;

  s.ok = true;
  s.falhas = 0;
  s.repetidas = 0;
  s.filtro.reset();
  Serial.printf("[%s] sensor voltou a responder\n", s.nome);
}

void lerSensor(Sensor &s) {
  if (!s.ok) {
    tentarRecuperar(s);
    return;
  }

  bool leituraBoa = false;
  sensors_event_t a, g, t;

  if (responde(s) && s.mpu.getEvent(&a, &g, &t)) {
    float x = a.acceleration.x, y = a.acceleration.y, z = a.acceleration.z;
    float mag = sqrtf(x * x + y * y + z * z);

    // Estrutura parada: a aceleração total deve ficar perto de 9,8 m/s2
    if (mag > 2.0 && mag < 30.0) {
      leituraBoa = true;
      if (x == s.ultX && y == s.ultY && z == s.ultZ) s.repetidas++;
      else s.repetidas = 0;
      s.ultX = x; s.ultY = y; s.ultZ = z;
    }
  }

  if (leituraBoa) s.falhas = 0;
  else s.falhas++;

  if (s.falhas >= FALHAS_MAX || s.repetidas >= CONGELADO_MAX) {
    s.ok = false;
    s.ultimaTentativa = millis();
    Serial.printf("[%s] FALHA DE SENSOR detectada\n", s.nome);
    return;
  }

  if (leituraBoa) s.angulo = s.filtro.add(anguloDe(a) - s.offset);
}

// ------------------------------------------------------------
// Lógica de segurança (não depende de rede)
// ------------------------------------------------------------
Nivel calcularNivel(float m, Nivel atual) {
  switch (atual) {
    case NIVEL_NORMAL:
      if (m > LIM_PERIGO)  return NIVEL_PERIGO;
      if (m > LIM_ATENCAO) return NIVEL_ATENCAO;
      return NIVEL_NORMAL;
    case NIVEL_ATENCAO:
      if (m > LIM_PERIGO) return NIVEL_PERIGO;
      if (m < LIM_ATENCAO - HISTERESE) return NIVEL_NORMAL;
      return NIVEL_ATENCAO;
    default: // NIVEL_PERIGO
      if (m < LIM_PERIGO - HISTERESE) return NIVEL_ATENCAO;
      return NIVEL_PERIGO;
  }
}

void avaliarSeguranca() {
  Sensor* lista[2] = {&s1, &s2};
  float maior = 0;
  int qtdOk = 0;

  for (int i = 0; i < 2; i++) {
    if (lista[i]->ok) {
      qtdOk++;
      maior = fmaxf(maior, fabsf(lista[i]->angulo));
    }
  }

  if (qtdOk > 0) nivelAngulo = calcularNivel(maior, nivelAngulo);

  bool algumaFalha = (qtdOk < 2);
  if (nivelAngulo == NIVEL_PERIGO)      nivelAtual = NIVEL_PERIGO;
  else if (algumaFalha)                 nivelAtual = NIVEL_FALHA;
  else                                  nivelAtual = nivelAngulo;

  // Fail-safe: perigo OU sensor com falha => cancela fechada
  bool fechar = (nivelAtual == NIVEL_PERIGO || nivelAtual == NIVEL_FALHA);
  if (fechar != cancelaFechada) {
    cancelaFechada = fechar;
    meuServo.write(fechar ? SERVO_FECHADA : SERVO_ABERTA);
    Serial.println(fechar ? ">>> CANCELA FECHADA" : ">>> CANCELA ABERTA");
  }
}

// ------------------------------------------------------------
// Rede (não bloqueante)
// ------------------------------------------------------------
const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "energia";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "falha_programa";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "queda_tensao";
    default:               return "outro";
  }
}

void conectarMQTT() {
  char clientId[40];
  snprintf(clientId, sizeof(clientId), "%s%04X", MQTT_CLIENT_PREFIX, (unsigned)random(0xffff));

  const char* user = strlen(MQTT_USER) > 0 ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) > 0 ? MQTT_PASS : nullptr;

  // Last Will: se o ESP32 sumir, o broker publica "offline" sozinho
  bool ok = mqttClient.connect(clientId, user, pass,
                               MQTT_TOPIC_STATUS, 1, true, "{\"status\":\"offline\"}");
  if (ok) {
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"status\":\"online\",\"reinicio\":\"%s\"}", motivoReinicio());
    mqttClient.publish(MQTT_TOPIC_STATUS, msg, true);
    Serial.println("MQTT conectado");
  } else {
    Serial.printf("MQTT falhou, rc=%d (nova tentativa em %lus)\n",
                  mqttClient.state(), REPETE_MQTT / 1000);
  }
}

void gerenciarRede() {
  unsigned long agora = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (agora - ultimaTentativaWiFi >= REPETE_WIFI) {
      ultimaTentativaWiFi = agora;
      Serial.println("WiFi desconectado, tentando reconectar...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  if (!mqttClient.connected()) {
    if (agora - ultimaTentativaMQTT >= REPETE_MQTT) {
      ultimaTentativaMQTT = agora;
      conectarMQTT();
    }
  } else {
    mqttClient.loop();
  }
}

// ------------------------------------------------------------
// Serial + MQTT (a cada 1 s, ou na hora se o nível mudar)
// ------------------------------------------------------------
void reportar() {
  unsigned long agora = millis();
  bool mudou = (nivelAtual != nivelReportado);
  if (!mudou && agora - ultimoEnvio < INTERVALO_ENVIO) return;

  ultimoEnvio = agora;
  nivelReportado = nivelAtual;

  Serial.printf("Pilar 1: %.2f graus (%s) | Pilar 3: %.2f graus (%s) | nivel: %s | cancela: %s\n",
                s1.angulo, s1.ok ? "ok" : "FALHA",
                s2.angulo, s2.ok ? "ok" : "FALHA",
                NOMES_NIVEL[nivelAtual], cancelaFechada ? "fechada" : "aberta");

  if (!mqttClient.connected()) {
    Serial.println("(sem MQTT: o sistema segue operando localmente)");
    return;
  }

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"angulo1\":%.2f,\"angulo2\":%.2f,\"nivel\":\"%s\",\"risco\":%s,"
           "\"cancela\":\"%s\",\"sensor1\":\"%s\",\"sensor2\":\"%s\"}",
           s1.angulo, s2.angulo, NOMES_NIVEL[nivelAtual],
           cancelaFechada ? "true" : "false",
           cancelaFechada ? "fechada" : "aberta",
           s1.ok ? "ok" : "falha", s2.ok ? "ok" : "falha");

  mqttClient.publish(MQTT_TOPIC, payload);
  Serial.println("Publicado: " + String(payload));
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Cancela começa FECHADA (estado seguro) até os sensores confirmarem
  meuServo.setPeriodHertz(50);
  meuServo.attach(SERVO_PIN, 500, 2400);
  meuServo.write(SERVO_FECHADA);

  Wire.begin(SDA1_PIN, SCL1_PIN);
  Wire1.begin(SDA2_PIN, SCL2_PIN);

  s1.bus = &Wire;  s1.nome = "Pilar 1";
  s2.bus = &Wire1; s2.nome = "Pilar 3";

  Sensor* lista[2] = {&s1, &s2};
  for (int i = 0; i < 2; i++) {
    lista[i]->ok = iniciarSensor(*lista[i]) && calibrar(*lista[i]);
    lista[i]->ultimaTentativa = millis();
    Serial.printf("[%s] %s\n", lista[i]->nome, lista[i]->ok ? "OK e calibrado" : "FALHOU (vai tentar de novo)");
  }

  // Rede: apenas inicia, NÃO espera conectar
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ultimaTentativaWiFi = millis();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(2);
}

void loop() {
  unsigned long agora = millis();

  // 1) Segurança primeiro, independente da rede
  if (agora - ultimaLeitura >= INTERVALO_LEITURA) {
    ultimaLeitura = agora;
    lerSensor(s1);
    lerSensor(s2);
    avaliarSeguranca();
  }

  // 2) Comunicação depois
  gerenciarRede();
  reportar();

  delay(1);
}
