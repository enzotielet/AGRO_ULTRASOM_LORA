/*
  ============================================================
  AJ-SR04M (modo 1 - R19 aberto) + ESP32 LoRa (LMIC, ABP)
  Monitor Serial: 115200 baud
  ============================================================

  LIGAÇÃO
    Sensor 5V   -> 5V da placa
    Sensor GND  -> GND da placa
    Sensor Trig -> GPIO 13   (+ resistor 10k do GPIO 13 para GND)
    Sensor Echo -> resistor 1k -> GPIO 25
                   GPIO 25 -> resistor 2k -> GND   (divisor 5V -> 3,3V)

  Payload Cayenne LPP (4 bytes):
    Canal 1 - Analog Input - distância em METROS (resolução 0,01 m)
*/

#include <Arduino.h>
#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <CayenneLPP.h>

// ===================== TEMPOS DE ENVIO =====================
// Os primeiros ENVIOS_INICIAIS envios usam INTERVALO_INICIAL_S.
// Depois disso, todos os envios usam INTERVALO_NORMAL_S.
const uint32_t ENVIOS_INICIAIS     = 1000;        // quantos envios no ritmo inicial
const uint32_t INTERVALO_INICIAL_S = 10;       // segundos entre os primeiros envios
const uint32_t INTERVALO_NORMAL_S  = 60 * 2;   // segundos entre os demais envios

// Se true: quando a leitura falha, reenvia a última distância válida
static bool SEND_OLD_WHEN_NO_NEW_READING = true;
const bool teste = true;
// ===================== SENSOR ULTRASSÔNICO =====================
const int PIN_TRIG = 13;
const int PIN_ECHO = 25;
float ultimaMedidaM = 0;

const unsigned int  TRIG_US      = 20;       // largura do pulso de disparo (µs)
const unsigned long TIMEOUT_US   = 60000UL;  // espera máxima pelo Echo (µs)
const unsigned long INTERVALO_MS = 30;       // pausa entre amostras (ms)
const unsigned long ECHO_MIN_US  = 1300;     // abaixo disso = zona cega (~22 cm)
const unsigned long ECHO_MAX_US  = 40000;    // acima disso = sem eco (~6,9 m)
const int           AMOSTRAS     = 20;        // leituras por resultado (mediana)
const float velocidadeSom = 346.45;          // m/s (~25 °C)

// ===================== ABP KEYS =====================
#if !teste
static const PROGMEM u1_t NWKSKEY[16] = { 0x9B, 0xBE, 0x25, 0xA7, 0x76, 0xA4, 0x01, 0x63, 0x2B, 0x16, 0x4D, 0xDB, 0xF1, 0xDE, 0x0A, 0x33 };
static const PROGMEM u1_t APPSKEY[16] = { 0x27, 0xF5, 0x4E, 0x19, 0xF8, 0x8A, 0xEF, 0x32, 0x08, 0xEA, 0xC8, 0x36, 0x0E, 0xF7, 0xB0, 0x51 };
static const u4_t DEVADDR = 0x260C367F;
#else
//key de teste
static const PROGMEM u1_t NWKSKEY[16] = { 0xD9, 0x55, 0x70, 0xA1, 0x31, 0xD0, 0x38, 0xEC, 0x4D, 0x36, 0x0D, 0x30, 0x89, 0x12, 0xC9, 0x34};
static const PROGMEM u1_t APPSKEY[16] = { 0xE6, 0x0B, 0x65, 0xF1, 0x34, 0x75, 0xD4, 0xFB, 0x97, 0x34, 0x06, 0x96, 0xFD, 0x40, 0xCF, 0x2C};
static const u4_t DEVADDR = 0x260C8564;
#endif

// Callbacks OTAA vazios (ABP)
void os_getArtEui(u1_t *buf) {}
void os_getDevEui(u1_t *buf) {}
void os_getDevKey(u1_t *buf) {}

// ===================== PAYLOAD =====================
CayenneLPP lpp(12);
uint8_t payloadRecebido[12];
uint8_t tamanhoPayload = 0;
bool novoPayloadPronto = false;

// ===================== PINMAP LORA =====================
const lmic_pinmap lmic_pins = {
  .nss = 18,
  .rxtx = LMIC_UNUSED_PIN,
  .rst = 23,
  .dio = {26, 33, 32}, // DIO0, DIO1, DIO2
};

// ===================== CONTROLE DE ENVIOS =====================
static uint32_t g_enviosFeitos = 0;   // quantos pacotes já foram enfileirados
static uint32_t g_lastTickMs   = 0;

// Intervalo (s) que vale para o PRÓXIMO envio
uint32_t intervaloAtualS()
{
  return (g_enviosFeitos < ENVIOS_INICIAIS) ? INTERVALO_INICIAL_S : INTERVALO_NORMAL_S;
}

// ===================== UTILITÁRIOS =====================
// Espera sem travar a máquina de estados do LMIC
static void esperaComLmic(uint32_t ms)
{
  uint32_t t0 = millis();
  while (millis() - t0 < ms) os_runloop_once();
}

void ordenar(long *v, int n)
{
  for (int i = 1; i < n; i++) {
    long x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
}

long medirPulsoUs()
{
  // Garante que o ciclo anterior terminou
  unsigned long t0 = micros();
  while (digitalRead(PIN_ECHO) == HIGH) {
    if (micros() - t0 > TIMEOUT_US) return -1;
  }

  // Pulso de disparo
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(TRIG_US);
  digitalWrite(PIN_TRIG, LOW);

  return (long)pulseIn(PIN_ECHO, HIGH, TIMEOUT_US);
}

// ===================== LEITURA + PAYLOAD =====================
// Lê o sensor e monta o payload LPP.
// Retorna true se houve leitura válida nova.
bool distanciasend()
{
  
  long validas[AMOSTRAS];
  int n = 0, zonaCega = 0, semEco = 0, travado = 0;

  for (int i = 0; i < AMOSTRAS; i++) {
    long t = medirPulsoUs();
    if (t < 0)                                travado++;
    else if (t == 0 || t > (long)ECHO_MAX_US) semEco++;
    else if (t < (long)ECHO_MIN_US)           zonaCega++;
    else                                      validas[n++] = t;
    esperaComLmic(INTERVALO_MS);
  }

  if (n == 0) {
    Serial.printf("Sem leitura valida | zona cega: %d  sem eco: %d  echo travado: %d\n",
                  zonaCega, semEco, travado);
    return false;   // payload anterior fica intacto
  }

  ordenar(validas, n);
  long  t          = validas[n / 2];                   // mediana
  float distanciaM = t * velocidadeSom / 2000000.0f;   // µs -> metros

  lpp.reset();
  if(ultimaMedidaM != 0) {
    if(fabsl(distanciaM - ultimaMedidaM) > 0.30) {
      distanciaM = ultimaMedidaM;  // descarta leitura fora da faixa de tolerância
    }
  }

  lpp.addAnalogInput(1, distanciaM);
  ultimaMedidaM = distanciaM;
  if (teste) {
    lpp.addAnalogInput(2, 1);
  }

  tamanhoPayload = lpp.getSize();
  if (tamanhoPayload > sizeof(payloadRecebido)) {
    tamanhoPayload = sizeof(payloadRecebido);
  }
  memcpy(payloadRecebido, lpp.getBuffer(), tamanhoPayload);
  novoPayloadPronto = true;

  Serial.printf("Distancia: %.2f m | pulso: %ld us | validas: %d/%d | faixa: %ld-%ld us\n",
                distanciaM, t, n, AMOSTRAS, validas[0], validas[n - 1]);
  Serial.printf("Payload LPP preparado (%u bytes)\n", tamanhoPayload);
  Serial.println("---");
  return true;
}

// ===================== EVENTOS LMIC =====================
void onEvent(ev_t ev)
{
  Serial.print(os_getTime());
  Serial.print(": EV: ");
  Serial.println(ev);

  if (ev == EV_TXCOMPLETE) {
    Serial.println("EV_TXCOMPLETE");

    if (LMIC.dataLen) {
      Serial.println("==========================================");
      Serial.print("Data Received: ");
      Serial.write(LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
      Serial.println();
      Serial.println("==========================================");
    }
  }
}

// ===================== COMPASSO DE ENVIO =====================
void tickSendIfDue()
{
  uint32_t now      = millis();
  uint32_t periodMs = intervaloAtualS() * 1000UL;

  if (g_lastTickMs == 0) g_lastTickMs = now;
  if (now - g_lastTickMs < periodMs) return;
  g_lastTickMs = now;

  bool leituraNova = distanciasend();

  if (LMIC.opmode & OP_TXRXPEND) {
    Serial.println("Tick: OP_TXRXPEND, nao enviando agora.");
    return;
  }

  if (tamanhoPayload == 0) {
    Serial.println("Tick: ainda nao existe payload valido para enviar.");
    return;
  }

  if (!leituraNova) {
    if (!SEND_OLD_WHEN_NO_NEW_READING) {
      Serial.println("Tick: sem leitura nova, envio pulado.");
      return;
    }
    Serial.println("Tick: sem leitura nova, reenviando ultimo valor valido.");
  }

  Serial.println("Tick: enfileirando TX LoRa...");
  Serial.printf("Canal: %u\n", LMIC.txChnl);
  Serial.printf("Freq : %lu\n", LMIC.freq);
  Serial.printf("DR   : %u\n", LMIC.datarate);

  LMIC_setTxData2(1, payloadRecebido, tamanhoPayload, 0);
  novoPayloadPronto = false;
  g_enviosFeitos++;

  Serial.printf("Tick: Packet queued. Envio #%lu | proximo em %lu s",
                (unsigned long)g_enviosFeitos, (unsigned long)intervaloAtualS());
  if (g_enviosFeitos == ENVIOS_INICIAIS) {
    Serial.print("  (fim da fase inicial)");
  }
  Serial.println();
}

// ===================== SETUP =====================
void setup()
{
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nBOOT: iniciou setup()");

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);   // o pull-down vem do divisor externo
  delay(500);                 // estabilização do sensor após ligar
  Serial.println("=== AJ-SR04M | modo 1 (R19 aberto) ===");
  Serial.printf("Trig: GPIO %d | Echo: GPIO %d\n", PIN_TRIG, PIN_ECHO);
  Serial.printf("v_som: %.1f m/s\n", velocidadeSom);
  Serial.println("--------------------------------------");

  delay(100);
  SPI.begin(5, 19, 27, 18);
  os_init();
  LMIC_reset();

#ifdef PROGMEM
  uint8_t appskey[sizeof(APPSKEY)];
  uint8_t nwkskey[sizeof(NWKSKEY)];
  memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
  memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
  LMIC_setSession(0x1, DEVADDR, nwkskey, appskey);
#else
  LMIC_setSession(0x1, DEVADDR, NWKSKEY, APPSKEY);
#endif

  LMIC_setLinkCheckMode(0);

  // Ajustes RX2 / DR / TxPower
  LMIC.dn2Dr = DR_SF7;
  LMIC_setDrTxpow(DR_SF10, 14);

  // Desabilita canais (mantém só a sub-banda usada)
  for (int i = 0; i < 7; i++)   LMIC_disableChannel(i);
  for (int i = 16; i < 63; i++) LMIC_disableChannel(i);

  g_lastTickMs = millis();

  Serial.println("Sistema pronto.");
  Serial.printf("Primeiros %lu envios: a cada %lu s\n",
                (unsigned long)ENVIOS_INICIAIS, (unsigned long)INTERVALO_INICIAL_S);
  Serial.printf("Demais envios: a cada %lu s\n", (unsigned long)INTERVALO_NORMAL_S);
}

// ===================== LOOP =====================
void loop()
{
  os_runloop_once();
  tickSendIfDue();
}
