/*
  TLS METER - Firmware real V1.3
  ESP32-WROOM + MAX31865/PT1000 + INA226 + MOSFET + buzzer
  Compativel com o aplicativo Windows TLS Meter V4.4

  ---------------- PINAGEM ----------------
  MAX31865 / SPI
    SCK  -> GPIO 18
    SDO  -> GPIO 19
    SDI  -> GPIO 23
    CS   -> GPIO 27

  INA226 / I2C
    SDA  -> GPIO 21
    SCL  -> GPIO 22
    Endereco confirmado: 0x44
    Shunt confirmado no modulo: R100 = 0,1 ohm

  MOSFET
    PWM/IN -> GPIO 25

  BUZZER ATIVO
    sinal -> GPIO 33

  MONITORAMENTO DA BATERIA
    tensao da bateria -> GPIO 34 (ADC1), via divisor 100k/100k
    presenca do carregador USB -> GPIO 32 (ADC1), via divisor resistivo

    GPIO32 nao le o pino CHRG do TP4056.
    Ele detecta diretamente os +5 V do micro-USB por meio do divisor:
      carregador conectado    -> aproximadamente 2,25 V no GPIO32
      carregador desconectado -> aproximadamente 0 V no GPIO32

  ---------------- PROTOCOLO BT SPP ----------------
  PC -> medidor:
    PING
    MONITOR_START
    HEATER_ON,<heat_ms>,<cool_ms>
    HEATER_OFF
    CYCLE_STOP

  medidor -> PC:
    READY,TLS-METER-SPP
    PONG,TLS-METER-SPP
    MONITORING
    HEATER_ON
    HEATER_OFF
    CYCLE_END
    CYCLE_STOPPED
    DATA,<PHASE>,<session_ms>,<phase_ms>,<temp_mC>,<power_mW>
    BAT,<battery_mV>,<percent>,<CHARGING|IDLE>
    STATUS,<codigo>

  OBSERVACOES DA BATERIA:
  - BAT e enviado ao conectar e depois periodicamente.
  - Faixa util adotada:
      0%   = 3,00 V
      100% = 4,15 V
  - A leitura usa media de varias amostras + filtro exponencial.
  - Durante aquecimento o filtro fica mais lento para reduzir o efeito
    da queda de tensao causada pela carga do aquecedor.
  - Sem carregador, a porcentagem nunca aumenta.
  - Com carregador conectado, a porcentagem nunca diminui.
  - Mudancas de porcentagem precisam persistir antes de serem aceitas.
  - Abaixo de 15%: bip longo de 1 s a cada 1 minuto.
  - Abaixo de 10%: bloqueia apenas o INICIO de um novo ensaio.
  - Se o ensaio ja estiver em andamento, nao e interrompido por bateria baixa.

  ---------------- AUTO TESTE ----------------
  1) Confere MAX31865/PT1000.
  2) Temperatura deve estar entre 0 e 100 C.
  3) Confere o INA226.
  4) Liga o aquecedor por 5 segundos.
  5) Verifica se existe corrente real na carga.
  6) Desliga o aquecedor SEMPRE ao terminar ou ao detectar falha.

  OBSERVACAO:
  "corrente diferente de zero" e implementado com um pequeno limiar de
  10 mA para nao confundir ruido/offset do INA226 com corrente real.
*/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_MAX31865.h>
#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth nao esta habilitado nesta configuracao do ESP32.
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Bluetooth Classic SPP requer ESP32 classico/WROOM.
#endif

// ============================================================
// PINOS
// ============================================================

static const uint8_t PIN_MAX_CS      = 27;
static const uint8_t PIN_HEATER      = 25;
static const uint8_t PIN_BUZZER      = 33;

// Bateria
static const uint8_t PIN_BATTERY_ADC   = 34;
static const uint8_t PIN_CHARGE_STATUS = 32;

static const uint8_t PIN_I2C_SDA     = 21;
static const uint8_t PIN_I2C_SCL     = 22;

static const uint8_t PIN_SPI_SCK     = 18;
static const uint8_t PIN_SPI_MISO    = 19;
static const uint8_t PIN_SPI_MOSI    = 23;

// ============================================================
// MAX31865 / PT1000
// ============================================================

Adafruit_MAX31865 max31865(PIN_MAX_CS);

static const float PT1000_RNOMINAL = 1000.0f;
static const float MAX31865_RREF   = 4300.0f;

// ============================================================
// INA226
// ============================================================

static const uint8_t INA226_ADDR = 0x44;

// Modulo confirmado com resistor R100.
static const float INA226_SHUNT_OHMS = 0.100f;

static const uint8_t INA_REG_CONFIG = 0x00;
static const uint8_t INA_REG_SHUNT  = 0x01;
static const uint8_t INA_REG_BUS    = 0x02;
static const uint8_t INA_REG_MANUF  = 0xFE;
static const uint8_t INA_REG_DIE    = 0xFF;

// Configuracao continua: shunt + bus.
static const uint16_t INA_CONFIG = 0x4127;

// ============================================================
// BLUETOOTH
// ============================================================

BluetoothSerial SerialBT;

static const char *DEVICE_NAME = "TLS-METER";
static const char *HANDSHAKE_ID = "TLS-METER-SPP";

// ============================================================
// PARAMETROS
// ============================================================

static const unsigned long SAMPLE_INTERVAL_MS = 1000;

// Auto teste: pedido de 5 segundos.
static const unsigned long SELFTEST_HEATER_MS = 5000;

// Corrente minima considerada realmente diferente de zero.
// Evita que ruido de poucos uA/mA passe no auto teste.
static const float SELFTEST_MIN_CURRENT_A = 0.010f;

// Limites solicitados para temperatura.
static const float TEMP_MIN_C = 0.0f;
static const float TEMP_MAX_C = 100.0f;

// ============================================================
// BATERIA 1S2P - MONITORAMENTO
// ============================================================

// Divisor da bateria: 100 kOhm / 100 kOhm.
// Vbat = Vadc * 2.
// Ajuste BATTERY_CALIBRATION_FACTOR apos comparar com multimetro.
static const float BATTERY_DIVIDER_RATIO = 2.000f;
static const float BATTERY_CALIBRATION_FACTOR = 1.000f;

// Faixa eletricamente plausivel para validar o circuito de leitura.
// A porcentagem, entretanto, e saturada entre BATTERY_EMPTY_V e
// BATTERY_FULL_V.
static const float BATTERY_VALID_MIN_V = 2.50f;
static const float BATTERY_VALID_MAX_V = 4.35f;

// Limites operacionais adotados para a indicacao.
// A faixa foi definida de forma conservadora em relacao a bateria 1S,
// protecao e alimentacao por step-up.
static const float BATTERY_EMPTY_V = 3.00f;  // 0%
static const float BATTERY_FULL_V  = 4.15f;  // 100%

// Atualizacao enviada por Bluetooth.
static const unsigned long BATTERY_REPORT_INTERVAL_MS = 10000UL;

// GPIO32 detecta a PRESENCA DOS 5 V DO USB pelo ADC.
// No hardware atual sao medidos cerca de 2,25 V com o cabo conectado.
// Dois limiares criam histerese e evitam ficar alternando perto do ponto
// de decisao.
static const uint16_t CHARGER_PRESENT_ON_MV  = 1200;
static const uint16_t CHARGER_PRESENT_OFF_MV = 800;
static const uint8_t CHARGER_SENSE_SAMPLES = 8;

// Regras solicitadas.
static const int BATTERY_WARN_PERCENT = 15;
static const int BATTERY_BLOCK_START_PERCENT = 10;
static const unsigned long BATTERY_LOW_BEEP_INTERVAL_MS = 60000UL;

// Leitura e filtragem.
// A media curta reduz ruido do ADC; o filtro exponencial reduz variacoes
// lentas. Durante HEATING o filtro e propositalmente mais lento para que
// a queda de tensao sob carga nao derrube a porcentagem instantaneamente.
static const uint8_t BATTERY_ADC_SAMPLES = 32;
static const float BATTERY_FILTER_ALPHA_IDLE = 0.05f;
static const float BATTERY_FILTER_ALPHA_HEATING = 0.01f;

// Histerese temporal da porcentagem.
// A nova direcao precisa aparecer em duas atualizacoes consecutivas.
static const uint8_t BATTERY_PERCENT_CONFIRMATIONS = 2;

// ============================================================
// BUZZER - PADROES PARA BUZZER ATIVO
// Cada vetor alterna ON/OFF/ON/OFF... em milissegundos.
// ============================================================

// Ligou o ESP: bip curto.
static const uint16_t SOUND_POWER_ON[] = {
  90
};

// Auto teste aprovado: "fim de ciclo" curto, tipo eletrodomestico.
static const uint16_t SOUND_SELFTEST_OK[] = {
  90, 70, 90, 70, 280
};

// Auto teste falhou: tres bipes longos.
static const uint16_t SOUND_SELFTEST_FAIL[] = {
  300, 130, 300, 130, 300
};

// Bluetooth conectado: dois bipes curtos diferentes.
static const uint16_t SOUND_BT_CONNECTED[] = {
  60, 80, 140
};

// Inicio do ensaio: curto + longo.
static const uint16_t SOUND_TEST_START[] = {
  90, 100, 240
};

// Fim normal do ensaio: sequencia distinta.
static const uint16_t SOUND_TEST_END[] = {
  90, 70, 90, 70, 90, 100, 320
};

// Aborto/falha durante ensaio.
static const uint16_t SOUND_ABORT[] = {
  220, 120, 220
};

// Bateria abaixo de 15%: bip longo de 1 segundo.
static const uint16_t SOUND_LOW_BATTERY[] = {
  1000
};

struct BuzzerPlayer {
  const uint16_t *pattern = nullptr;
  uint8_t count = 0;
  uint8_t index = 0;
  bool active = false;
  bool outputOn = false;
  unsigned long nextChangeMs = 0;
};

BuzzerPlayer buzzer;

void buzzerOff() {
  digitalWrite(PIN_BUZZER, LOW);
}

void playPatternBlocking(const uint16_t *pattern, uint8_t count) {
  bool on = true;

  for (uint8_t i = 0; i < count; i++) {
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
    delay(pattern[i]);
    on = !on;
  }

  buzzerOff();
}

void startBuzzerPattern(const uint16_t *pattern, uint8_t count) {
  buzzer.pattern = pattern;
  buzzer.count = count;
  buzzer.index = 0;
  buzzer.active = true;
  buzzer.outputOn = true;

  digitalWrite(PIN_BUZZER, HIGH);
  buzzer.nextChangeMs = millis() + pattern[0];
}

void updateBuzzer() {
  if (!buzzer.active) return;

  unsigned long now = millis();

  if ((long)(now - buzzer.nextChangeMs) < 0) {
    return;
  }

  buzzer.index++;

  if (buzzer.index >= buzzer.count) {
    buzzer.active = false;
    buzzer.outputOn = false;
    buzzerOff();
    return;
  }

  buzzer.outputOn = !buzzer.outputOn;
  digitalWrite(PIN_BUZZER, buzzer.outputOn ? HIGH : LOW);
  buzzer.nextChangeMs = now + buzzer.pattern[buzzer.index];
}

// ============================================================
// ESTADO DO MEDIDOR
// ============================================================

// Estados do ciclo.
// Usamos uint8_t em vez de um tipo enum customizado porque o
// pre-processador do Arduino IDE pode gerar prototipos antes da
// declaracao de tipos definidos pelo usuario em arquivos .ino.
static const uint8_t PHASE_IDLE        = 0;
static const uint8_t PHASE_STABILIZING = 1;
static const uint8_t PHASE_HEATING     = 2;
static const uint8_t PHASE_COOLING     = 3;

uint8_t phase = PHASE_IDLE;

unsigned long sessionStartMs = 0;
unsigned long phaseStartMs = 0;
unsigned long lastSampleMs = 0;

unsigned long heatDurationMs = 600000UL;
unsigned long coolDurationMs = 300000UL;

bool heaterEnabled = false;
bool previousClient = false;
bool abortedByBluetooth = false;

bool selfTestPassed = false;
String selfTestStatus = "NOT_RUN";
String pendingStatus = "";

String btCommandBuffer;

// ============================================================
// ESTADO DA BATERIA
// ============================================================

bool batteryValid = false;
bool batteryCharging = false;

float batteryVoltageRaw = 0.0f;
float batteryVoltageFiltered = 0.0f;
int batteryPercent = -1;

// Estado auxiliar para a histerese temporal da porcentagem:
// -1 = tendencia de queda; +1 = tendencia de subida; 0 = nenhuma.
int8_t batteryPercentPendingDirection = 0;
uint8_t batteryPercentPendingCount = 0;

// Ultima leitura do divisor que detecta os 5 V do USB.
uint16_t chargerSenseMilliVolts = 0;

unsigned long lastBatteryReportMs = 0;
unsigned long lastBatteryLowBeepMs = 0;
unsigned long lastChargePollMs = 0;

// ============================================================
// FUNCOES BASICAS
// ============================================================

const char* phaseName(uint8_t p) {
  switch (p) {
    case PHASE_STABILIZING: return "STABILIZING";
    case PHASE_HEATING:     return "HEATING";
    case PHASE_COOLING:     return "COOLING";
    default:          return "IDLE";
  }
}

void sendLine(const String &line) {
  if (SerialBT.hasClient()) {
    SerialBT.println(line);
  }
}

void sendStatus(const String &status) {
  if (SerialBT.hasClient()) {
    SerialBT.print("STATUS,");
    SerialBT.println(status);
  } else {
    pendingStatus = status;
  }

  Serial.print("[STATUS] ");
  Serial.println(status);
}

void setHeater(bool on) {
  heaterEnabled = on;
  digitalWrite(PIN_HEATER, on ? HIGH : LOW);
}

void setPhase(uint8_t newPhase) {
  phase = newPhase;
  phaseStartMs = millis();
  lastSampleMs = 0;
}

// ============================================================
// BATERIA
// ============================================================

uint16_t readChargerSenseMilliVolts() {
  uint32_t sumMilliVolts = 0;

  for (uint8_t i = 0; i < CHARGER_SENSE_SAMPLES; i++) {
    sumMilliVolts += analogReadMilliVolts(PIN_CHARGE_STATUS);
    delayMicroseconds(500);
  }

  return (uint16_t)(
    sumMilliVolts / (uint32_t)CHARGER_SENSE_SAMPLES
  );
}

bool readChargingState() {
  chargerSenseMilliVolts = readChargerSenseMilliVolts();

  // Histerese:
  // - se ja consideramos o cabo conectado, so muda para OFF abaixo de 0,8 V;
  // - se estava desconectado, so muda para ON acima de 1,2 V.
  if (batteryCharging) {
    return chargerSenseMilliVolts > CHARGER_PRESENT_OFF_MV;
  }

  return chargerSenseMilliVolts >= CHARGER_PRESENT_ON_MV;
}

float readBatteryVoltageOnce() {
  uint32_t sumMilliVolts = 0;

  for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    sumMilliVolts += analogReadMilliVolts(PIN_BATTERY_ADC);
    delayMicroseconds(500);
  }

  float adcV =
    (sumMilliVolts / (float)BATTERY_ADC_SAMPLES) / 1000.0f;

  return adcV *
         BATTERY_DIVIDER_RATIO *
         BATTERY_CALIBRATION_FACTOR;
}

int batteryPercentFromVoltage(float voltage) {
  // Curva aproximada de uma celula Li-ion 1S ajustada para a faixa
  // operacional adotada no TLS Meter:
  //   <= 3,00 V -> 0%
  //   >= 4,15 V -> 100%
  //
  // Nao e uma regra de tres: a curva de descarga Li-ion nao e linear.
  static const float voltages[] = {
    3.00f,
    3.30f,
    3.50f,
    3.60f,
    3.70f,
    3.80f,
    3.90f,
    4.00f,
    4.10f,
    4.15f
  };

  static const int percents[] = {
     0,
     7,
    15,
    25,
    40,
    55,
    70,
    85,
    95,
   100
  };

  const uint8_t count =
    sizeof(voltages) / sizeof(voltages[0]);

  if (voltage <= BATTERY_EMPTY_V) return 0;
  if (voltage >= BATTERY_FULL_V) return 100;

  for (uint8_t i = 1; i < count; i++) {
    if (voltage <= voltages[i]) {
      float ratio =
        (voltage - voltages[i - 1]) /
        (voltages[i] - voltages[i - 1]);

      int result = lroundf(
        percents[i - 1] +
        ratio * (percents[i] - percents[i - 1])
      );

      if (result < 0) result = 0;
      if (result > 100) result = 100;
      return result;
    }
  }

  return 100;
}

void resetBatteryPercentHysteresis() {
  batteryPercentPendingDirection = 0;
  batteryPercentPendingCount = 0;
}

void updateDisplayedBatteryPercent(
  int calculatedPercent,
  bool initializePercent
) {
  calculatedPercent = constrain(calculatedPercent, 0, 100);

  if (initializePercent || batteryPercent < 0) {
    batteryPercent = calculatedPercent;
    resetBatteryPercentHysteresis();
    return;
  }

  int8_t requestedDirection = 0;

  if (batteryCharging) {
    // Com carregador conectado, a porcentagem pode subir, mas pequenas
    // quedas de tensao/ruido nao fazem a indicacao diminuir.
    if (calculatedPercent > batteryPercent) {
      requestedDirection = +1;
    }
  } else {
    // Sem carregador, a porcentagem pode cair, mas nunca aumenta por
    // recuperacao de tensao ou ruido do ADC.
    if (calculatedPercent < batteryPercent) {
      requestedDirection = -1;
    }
  }

  if (requestedDirection == 0) {
    resetBatteryPercentHysteresis();
    return;
  }

  if (requestedDirection == batteryPercentPendingDirection) {
    if (batteryPercentPendingCount < 255) {
      batteryPercentPendingCount++;
    }
  } else {
    batteryPercentPendingDirection = requestedDirection;
    batteryPercentPendingCount = 1;
  }

  // So aceita a mudanca se a tendencia persistir.
  if (
    batteryPercentPendingCount >=
    BATTERY_PERCENT_CONFIRMATIONS
  ) {
    batteryPercent = calculatedPercent;
    resetBatteryPercentHysteresis();
  }
}

void refreshBatteryReading(bool resetFilter) {
  float measuredV = readBatteryVoltageOnce();
  batteryVoltageRaw = measuredV;

  if (
    !isfinite(measuredV) ||
    measuredV < BATTERY_VALID_MIN_V ||
    measuredV > BATTERY_VALID_MAX_V
  ) {
    batteryValid = false;
    batteryPercent = -1;
    resetBatteryPercentHysteresis();

    Serial.print("[BAT] Leitura invalida: ");
    Serial.print(measuredV, 3);
    Serial.println(" V");
    return;
  }

  batteryValid = true;

  bool initializePercent =
    batteryPercent < 0 ||
    !isfinite(batteryVoltageFiltered) ||
    batteryVoltageFiltered < BATTERY_VALID_MIN_V;

  if (
    resetFilter ||
    !isfinite(batteryVoltageFiltered) ||
    batteryVoltageFiltered < BATTERY_VALID_MIN_V
  ) {
    batteryVoltageFiltered = measuredV;
  } else {
    float alpha =
      heaterEnabled
        ? BATTERY_FILTER_ALPHA_HEATING
        : BATTERY_FILTER_ALPHA_IDLE;

    batteryVoltageFiltered =
      (1.0f - alpha) * batteryVoltageFiltered +
      alpha * measuredV;
  }

  int calculatedPercent =
    batteryPercentFromVoltage(batteryVoltageFiltered);

  updateDisplayedBatteryPercent(
    calculatedPercent,
    initializePercent
  );
}

void emitBatteryStatus() {
  if (!SerialBT.hasClient()) return;

  const char *chargeState =
    batteryCharging ? "CHARGING" : "IDLE";

  char packet[64];

  if (batteryValid) {
    long battery_mV =
      lroundf(batteryVoltageFiltered * 1000.0f);

    snprintf(
      packet,
      sizeof(packet),
      "BAT,%ld,%d,%s",
      battery_mV,
      batteryPercent,
      chargeState
    );
  } else {
    snprintf(
      packet,
      sizeof(packet),
      "BAT,0,-1,%s",
      chargeState
    );
  }

  Serial.print("[BAT TX] ");
  Serial.println(packet);
  sendLine(packet);
}

void updateBatteryMonitor(bool forceReport) {
  unsigned long now = millis();

  // Detecta a presenca do cabo USB pelo ADC do GPIO32.
  if (
    lastChargePollMs == 0 ||
    now - lastChargePollMs >= 250UL
  ) {
    lastChargePollMs = now;

    bool chargingNow = readChargingState();

    if (chargingNow != batteryCharging) {
      batteryCharging = chargingNow;
      resetBatteryPercentHysteresis();

      Serial.print("[BAT] USB: ");
      Serial.print(chargerSenseMilliVolts);
      Serial.print(" mV | ");
      Serial.println(
        batteryCharging ? "CARREGADOR CONECTADO" :
                          "CARREGADOR DESCONECTADO"
      );

      refreshBatteryReading(false);
      emitBatteryStatus();

      if (batteryCharging) {
        lastBatteryLowBeepMs = now;
      }
    }
  }

  // Atualizacao periodica de tensao/porcentagem.
  if (
    forceReport ||
    lastBatteryReportMs == 0 ||
    now - lastBatteryReportMs >= BATTERY_REPORT_INTERVAL_MS
  ) {
    lastBatteryReportMs = now;
    refreshBatteryReading(false);

    Serial.print("[BAT] ");
    if (batteryValid) {
      Serial.print(batteryVoltageFiltered, 3);
      Serial.print(" V | ");
      Serial.print(batteryPercent);
      Serial.print("% | ");
    } else {
      Serial.print("leitura invalida | ");
    }

    Serial.print(
      batteryCharging ? "USB CONECTADO" : "USB DESCONECTADO"
    );
    Serial.print(" | GPIO32=");
    Serial.print(chargerSenseMilliVolts);
    Serial.println(" mV");

    emitBatteryStatus();
  }

  // Abaixo de 15%: bip longo a cada 1 minuto.
  // Se o USB estiver conectado, o aviso sonoro fica suspenso.
  if (
    batteryValid &&
    !batteryCharging &&
    batteryPercent < BATTERY_WARN_PERCENT
  ) {
    if (
      lastBatteryLowBeepMs == 0 ||
      now - lastBatteryLowBeepMs >=
        BATTERY_LOW_BEEP_INTERVAL_MS
    ) {
      if (!buzzer.active) {
        startBuzzerPattern(
          SOUND_LOW_BATTERY,
          sizeof(SOUND_LOW_BATTERY) /
          sizeof(SOUND_LOW_BATTERY[0])
        );

        lastBatteryLowBeepMs = now;

        Serial.println(
          "[BAT] Aviso sonoro: bateria abaixo de 15%."
        );
      }
    }
  } else {
    lastBatteryLowBeepMs = now;
  }
}

bool batteryAllowsNewCycle() {
  // Faz uma leitura nova com o aquecedor desligado.
  // O bloqueio vale SOMENTE antes de iniciar um novo ensaio.
  refreshBatteryReading(true);

  if (!batteryValid) {
    Serial.println(
      "[BAT] Leitura invalida; ensaio nao sera bloqueado."
    );
    return true;
  }

  // Para a decisao de seguranca do inicio do ensaio usamos a leitura
  // fresca, sem depender da histerese visual da porcentagem.
  int freshPercent =
    batteryPercentFromVoltage(batteryVoltageFiltered);

  // Sem carregador, se a leitura fresca for menor que a indicacao
  // estabilizada, atualiza imediatamente para baixo.
  if (
    !batteryCharging &&
    batteryPercent >= 0 &&
    freshPercent < batteryPercent
  ) {
    batteryPercent = freshPercent;
    resetBatteryPercentHysteresis();
  }

  Serial.print("[BAT] Antes do ensaio: ");
  Serial.print(batteryVoltageFiltered, 3);
  Serial.print(" V | ");
  Serial.print(freshPercent);
  Serial.println("%");

  return freshPercent >= BATTERY_BLOCK_START_PERCENT;
}

void initBatteryMonitor() {
  pinMode(PIN_BATTERY_ADC, INPUT);
  pinMode(PIN_CHARGE_STATUS, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  analogSetPinAttenuation(PIN_CHARGE_STATUS, ADC_11db);

  // Primeira deteccao do USB.
  batteryCharging = false;
  batteryCharging = readChargingState();

  refreshBatteryReading(true);

  Serial.print("[BAT] Inicial: ");

  if (batteryValid) {
    Serial.print(batteryVoltageFiltered, 3);
    Serial.print(" V | ");
    Serial.print(batteryPercent);
    Serial.print("% | ");
  } else {
    Serial.print("leitura invalida | ");
  }

  Serial.print(
    batteryCharging ? "USB CONECTADO" : "USB DESCONECTADO"
  );
  Serial.print(" | GPIO32=");
  Serial.print(chargerSenseMilliVolts);
  Serial.println(" mV");
}

// ============================================================
// INA226
// ============================================================

bool inaWrite16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));

  return Wire.endTransmission() == 0;
}

bool inaRead16(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom((uint8_t)INA226_ADDR, (uint8_t)2) != 2) {
    return false;
  }

  value = ((uint16_t)Wire.read() << 8) | Wire.read();
  return true;
}

bool initINA226() {
  uint16_t manufacturer = 0;
  uint16_t dieId = 0;

  if (!inaRead16(INA_REG_MANUF, manufacturer)) {
    return false;
  }

  if (!inaRead16(INA_REG_DIE, dieId)) {
    return false;
  }

  Serial.print("[INA226] Manufacturer ID: 0x");
  Serial.println(manufacturer, HEX);

  Serial.print("[INA226] Die ID: 0x");
  Serial.println(dieId, HEX);

  if (manufacturer != 0x5449 || dieId != 0x2260) {
    Serial.println("[INA226] Identificacao inesperada.");
    return false;
  }

  if (!inaWrite16(INA_REG_CONFIG, INA_CONFIG)) {
    return false;
  }

  return true;
}

bool readINA226(float &busV, float &currentA, float &powerW) {
  uint16_t rawBus = 0;
  uint16_t rawShuntUnsigned = 0;

  if (!inaRead16(INA_REG_BUS, rawBus)) {
    return false;
  }

  if (!inaRead16(INA_REG_SHUNT, rawShuntUnsigned)) {
    return false;
  }

  int16_t rawShunt = (int16_t)rawShuntUnsigned;

  // INA226:
  // VBUS LSB   = 1,25 mV/bit
  // VSHUNT LSB = 2,5 uV/bit
  busV = rawBus * 0.00125f;

  float shuntV = rawShunt * 2.5e-6f;
  currentA = shuntV / INA226_SHUNT_OHMS;

  // Para o protocolo do ensaio interessa a potencia dissipada positiva.
  powerW = busV * fabsf(currentA);

  // Offset muito pequeno com carga desligada e tratado como zero.
  if (fabsf(currentA) < 0.001f) {
    currentA = 0.0f;
    powerW = 0.0f;
  }

  return true;
}

// ============================================================
// MAX31865 / PT1000
// ============================================================

bool readTemperature(float &temperatureC, uint8_t &fault) {
  fault = 0;

  // Faz uma leitura para atualizar o conversor.
  uint16_t rtd = max31865.readRTD();
  fault = max31865.readFault();

  if (fault != 0) {
    max31865.clearFault();
    return false;
  }

  if (rtd == 0 || rtd >= 0x7FFF) {
    return false;
  }

  temperatureC = max31865.temperature(
    PT1000_RNOMINAL,
    MAX31865_RREF
  );

  if (!isfinite(temperatureC)) {
    return false;
  }

  return true;
}

// ============================================================
// AUTO TESTE
// ============================================================

bool runSelfTest() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" AUTO TESTE TLS METER");
  Serial.println("========================================");

  setHeater(false);

  // ---------- Temperatura ----------
  float temperatureC = 0.0f;
  uint8_t tempFault = 0;

  if (!readTemperature(temperatureC, tempFault)) {
    Serial.print("[ERRO] MAX31865/PT1000. Fault: 0x");
    Serial.println(tempFault, HEX);

    selfTestStatus = "SELFTEST_FAIL_TEMPERATURE_SENSOR";
    return false;
  }

  Serial.print("[OK] Temperatura inicial: ");
  Serial.print(temperatureC, 3);
  Serial.println(" C");

  if (temperatureC < TEMP_MIN_C || temperatureC > TEMP_MAX_C) {
    Serial.println("[ERRO] Temperatura fora da faixa 0...100 C.");

    selfTestStatus = "SELFTEST_FAIL_TEMP_RANGE";
    return false;
  }

  // ---------- INA226 ----------
  if (!initINA226()) {
    Serial.println("[ERRO] INA226 nao respondeu corretamente.");

    selfTestStatus = "SELFTEST_FAIL_INA226";
    return false;
  }

  float busV = 0.0f;
  float currentA = 0.0f;
  float powerW = 0.0f;

  if (!readINA226(busV, currentA, powerW)) {
    Serial.println("[ERRO] Nao foi possivel ler o INA226.");

    selfTestStatus = "SELFTEST_FAIL_INA226";
    return false;
  }

  Serial.print("[OK] Tensao antes do teste de carga: ");
  Serial.print(busV, 3);
  Serial.println(" V");

  // ---------- Teste do aquecedor ----------
  Serial.println("[TESTE] Ligando resistencia por 5 segundos...");

  setHeater(true);

  unsigned long startMs = millis();
  unsigned long lastPrint = 0;

  float maxCurrentA = 0.0f;
  float maxPowerW = 0.0f;

  bool measurementError = false;
  bool temperatureError = false;

  while (millis() - startMs < SELFTEST_HEATER_MS) {
    unsigned long elapsed = millis() - startMs;

    float v = 0.0f;
    float i = 0.0f;
    float p = 0.0f;

    if (!readINA226(v, i, p)) {
      measurementError = true;
      break;
    }

    if (fabsf(i) > maxCurrentA) {
      maxCurrentA = fabsf(i);
    }

    if (p > maxPowerW) {
      maxPowerW = p;
    }

    // Mantem seguranca termica tambem durante o pulso de auto teste.
    float t = 0.0f;
    uint8_t f = 0;

    if (!readTemperature(t, f) || t < TEMP_MIN_C || t > TEMP_MAX_C) {
      temperatureError = true;
      break;
    }

    if (elapsed - lastPrint >= 1000) {
      lastPrint = elapsed;

      Serial.print("[TESTE] t=");
      Serial.print(elapsed / 1000.0f, 1);
      Serial.print(" s | I=");
      Serial.print(i, 4);
      Serial.print(" A | P=");
      Serial.print(p, 4);
      Serial.println(" W");
    }

    delay(25);
  }

  // PRIMEIRA ACAO ao sair do teste: desliga a resistencia.
  setHeater(false);

  if (measurementError) {
    Serial.println("[ERRO] Falha de leitura do INA226 durante o teste.");

    selfTestStatus = "SELFTEST_FAIL_INA226";
    return false;
  }

  if (temperatureError) {
    Serial.println("[ERRO] Temperatura/sensor invalido durante o teste.");

    selfTestStatus = "SELFTEST_FAIL_TEMP_RANGE";
    return false;
  }

  Serial.print("[TESTE] Corrente maxima observada: ");
  Serial.print(maxCurrentA, 4);
  Serial.println(" A");

  Serial.print("[TESTE] Potencia maxima observada: ");
  Serial.print(maxPowerW, 4);
  Serial.println(" W");

  if (maxCurrentA < SELFTEST_MIN_CURRENT_A) {
    Serial.println("[ERRO] Nao foi detectada corrente real na resistencia.");

    selfTestStatus = "SELFTEST_FAIL_HEATER_CURRENT";
    return false;
  }

  selfTestStatus = "SELFTEST_OK";

  Serial.println("[OK] AUTO TESTE APROVADO.");
  Serial.println("========================================");
  Serial.println();

  return true;
}

// ============================================================
// PACOTES DE DADOS
// ============================================================

void emitSample() {
  if (phase == PHASE_IDLE) return;

  unsigned long now = millis();
  unsigned long sessionElapsed = now - sessionStartMs;
  unsigned long phaseElapsed = now - phaseStartMs;

  float temperatureC = 0.0f;
  uint8_t tempFault = 0;

  float busV = 0.0f;
  float currentA = 0.0f;
  float powerW = 0.0f;

  if (!readTemperature(temperatureC, tempFault)) {
    setHeater(false);
    phase = PHASE_IDLE;

    sendStatus("ABORT_TEMPERATURE_SENSOR");
    sendLine("CYCLE_STOPPED");
    startBuzzerPattern(
      SOUND_ABORT,
      sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
    );

    return;
  }

  // Seguranca local adicional durante um ensaio ativo.
  if (temperatureC < TEMP_MIN_C || temperatureC > TEMP_MAX_C) {
    setHeater(false);
    phase = PHASE_IDLE;

    sendStatus("ABORT_TEMP_RANGE");
    sendLine("CYCLE_STOPPED");
    startBuzzerPattern(
      SOUND_ABORT,
      sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
    );

    return;
  }

  if (!readINA226(busV, currentA, powerW)) {
    setHeater(false);
    phase = PHASE_IDLE;

    sendStatus("ABORT_INA226");
    sendLine("CYCLE_STOPPED");
    startBuzzerPattern(
      SOUND_ABORT,
      sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
    );

    return;
  }

  long temp_mC = lroundf(temperatureC * 1000.0f);
  long power_mW = lroundf(powerW * 1000.0f);

  char packet[110];

  snprintf(
    packet,
    sizeof(packet),
    "DATA,%s,%lu,%lu,%ld,%ld",
    phaseName(phase),
    sessionElapsed,
    phaseElapsed,
    temp_mC,
    power_mW
  );

  Serial.println(packet);
  sendLine(packet);
}

// ============================================================
// CICLO TLS
// ============================================================

void refuseCycleBecauseSelfTestFailed() {
  setHeater(false);
  phase = PHASE_IDLE;

  sendStatus(selfTestStatus);
  sendLine("CYCLE_STOPPED");

  startBuzzerPattern(
    SOUND_ABORT,
    sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
  );
}

void startMonitoring() {
  if (!selfTestPassed) {
    refuseCycleBecauseSelfTestFailed();
    return;
  }

  // Abaixo de 10% bloqueia SOMENTE o inicio de um novo ensaio.
  // Se o ensaio ja estiver ativo, nenhuma queda posterior de bateria
  // interrompe o ciclo por esse criterio.
  if (!batteryAllowsNewCycle()) {
    setHeater(false);
    phase = PHASE_IDLE;

    sendStatus("START_BLOCKED_LOW_BATTERY");
    sendLine("CYCLE_STOPPED");

    startBuzzerPattern(
      SOUND_ABORT,
      sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
    );

    Serial.println(
      "[BAT] Novo ensaio recusado: bateria abaixo de 10%."
    );

    emitBatteryStatus();
    return;
  }

  abortedByBluetooth = false;
  pendingStatus = "";

  sessionStartMs = millis();
  setPhase(PHASE_STABILIZING);

  Serial.println("[CYCLE] Monitoramento de estabilizacao iniciado.");
  sendLine("MONITORING");

  startBuzzerPattern(
    SOUND_TEST_START,
    sizeof(SOUND_TEST_START) / sizeof(SOUND_TEST_START[0])
  );
}

void startHeating(unsigned long heatMs, unsigned long coolMs) {
  if (!selfTestPassed) {
    refuseCycleBecauseSelfTestFailed();
    return;
  }

  // O aplicativo normalmente chama HEATER_ON durante STABILIZING.
  // Se vier fora de um ciclo, nao energiza a resistencia.
  if (phase == PHASE_IDLE) {
    setHeater(false);
    sendStatus("HEATER_ON_REJECTED_NO_ACTIVE_CYCLE");
    sendLine("CYCLE_STOPPED");
    return;
  }

  if (heatMs < 1000UL) {
    heatMs = 1000UL;
  }

  heatDurationMs = heatMs;
  coolDurationMs = coolMs;

  setHeater(true);
  setPhase(PHASE_HEATING);

  Serial.print("[CYCLE] Aquecedor ligado. Timeout local: ");
  Serial.print(heatDurationMs);
  Serial.println(" ms");

  sendLine("HEATER_ON");
}

void startCooling() {
  // Desliga PRIMEIRO.
  setHeater(false);

  if (phase == PHASE_IDLE) {
    return;
  }

  setPhase(PHASE_COOLING);

  Serial.println("[CYCLE] Aquecedor desligado. Resfriamento iniciado.");
  sendLine("HEATER_OFF");
}

void finishCycle() {
  setHeater(false);
  phase = PHASE_IDLE;

  Serial.println("[CYCLE] Ensaio concluido.");
  sendLine("CYCLE_END");

  startBuzzerPattern(
    SOUND_TEST_END,
    sizeof(SOUND_TEST_END) / sizeof(SOUND_TEST_END[0])
  );
}

void stopCycle() {
  setHeater(false);
  phase = PHASE_IDLE;

  Serial.println("[CYCLE] Ensaio interrompido.");
  sendLine("CYCLE_STOPPED");

  startBuzzerPattern(
    SOUND_ABORT,
    sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
  );
}

void abortBluetoothDisconnect() {
  // PRIMEIRA ACAO DE SEGURANCA: remove energia da resistencia.
  setHeater(false);

  phase = PHASE_IDLE;
  abortedByBluetooth = true;
  pendingStatus = "ABORT_BT_DISCONNECT";

  Serial.println("[SAFETY] Bluetooth perdido.");
  Serial.println("[SAFETY] Aquecedor desligado imediatamente.");
  Serial.println("[CYCLE] Ensaio abortado.");

  startBuzzerPattern(
    SOUND_ABORT,
    sizeof(SOUND_ABORT) / sizeof(SOUND_ABORT[0])
  );
}

// ============================================================
// COMANDOS BLUETOOTH
// ============================================================

void handleCommand(String command) {
  command.trim();

  if (!command.length()) return;

  Serial.print("[BT CMD] ");
  Serial.println(command);

  if (command.equalsIgnoreCase("PING")) {
    sendLine(String("PONG,") + HANDSHAKE_ID);
    return;
  }

  if (command.equalsIgnoreCase("MONITOR_START")) {
    startMonitoring();
    return;
  }

  if (command.startsWith("HEATER_ON")) {
    unsigned long heatMs = heatDurationMs;
    unsigned long coolMs = coolDurationMs;

    int comma1 = command.indexOf(',');
    int comma2 = command.indexOf(',', comma1 + 1);

    if (comma1 > 0) {
      String a;

      if (comma2 > comma1) {
        a = command.substring(comma1 + 1, comma2);
      } else {
        a = command.substring(comma1 + 1);
      }

      unsigned long parsed = strtoul(a.c_str(), nullptr, 10);

      if (parsed > 0) {
        heatMs = parsed;
      }
    }

    if (comma2 > comma1) {
      String b = command.substring(comma2 + 1);
      coolMs = strtoul(b.c_str(), nullptr, 10);
    }

    startHeating(heatMs, coolMs);
    return;
  }

  if (command.equalsIgnoreCase("HEATER_OFF")) {
    startCooling();
    return;
  }

  if (
    command.equalsIgnoreCase("CYCLE_STOP") ||
    command.equalsIgnoreCase("STOP")
  ) {
    stopCycle();
    return;
  }

  if (
    command.equalsIgnoreCase("BAT?") ||
    command.equalsIgnoreCase("BATTERY?")
  ) {
    refreshBatteryReading(false);
    batteryCharging = readChargingState();
    emitBatteryStatus();
    return;
  }

  sendStatus("UNKNOWN_COMMAND");
}

void readBluetoothCommands() {
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();

    if (c == '\n' || c == '\r') {
      if (btCommandBuffer.length()) {
        handleCommand(btCommandBuffer);
        btCommandBuffer = "";
      }
    } else if (btCommandBuffer.length() < 120) {
      btCommandBuffer += c;
    } else {
      btCommandBuffer = "";
      sendStatus("COMMAND_TOO_LONG");
    }
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  // Saidas em estado seguro.
  pinMode(PIN_HEATER, OUTPUT);
  setHeater(false);

  pinMode(PIN_BUZZER, OUTPUT);
  buzzerOff();

  // Monitor de bateria:
  // GPIO34 = tensao da bateria via divisor 100k/100k
  // GPIO32 = presenca dos 5 V do USB via divisor resistivo/ADC
  initBatteryMonitor();

  // Bip de inicializacao.
  playPatternBlocking(
    SOUND_POWER_ON,
    sizeof(SOUND_POWER_ON) / sizeof(SOUND_POWER_ON[0])
  );

  Serial.println();
  Serial.println("========================================");
  Serial.println(" TLS METER - FIRMWARE REAL V1.3");
  Serial.println("========================================");

  // I2C
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  // SPI
  SPI.begin(
    PIN_SPI_SCK,
    PIN_SPI_MISO,
    PIN_SPI_MOSI,
    PIN_MAX_CS
  );

  max31865.begin(MAX31865_3WIRE);

  // Auto teste.
  selfTestPassed = runSelfTest();

  // Garante aquecedor desligado em qualquer caso.
  setHeater(false);

  if (selfTestPassed) {
    playPatternBlocking(
      SOUND_SELFTEST_OK,
      sizeof(SOUND_SELFTEST_OK) / sizeof(SOUND_SELFTEST_OK[0])
    );
  } else {
    Serial.print("[SELFTEST] FALHOU: ");
    Serial.println(selfTestStatus);

    playPatternBlocking(
      SOUND_SELFTEST_FAIL,
      sizeof(SOUND_SELFTEST_FAIL) / sizeof(SOUND_SELFTEST_FAIL[0])
    );
  }

  // Bluetooth so inicia depois do auto teste.
  Serial.println("[BT] Iniciando Bluetooth Classic SPP...");

  if (!SerialBT.begin(DEVICE_NAME, false, true)) {
    Serial.println("[BT] ERRO ao iniciar Bluetooth.");

    // Nunca deixa a carga energizada se BT nao iniciar.
    setHeater(false);

    while (true) {
      playPatternBlocking(
        SOUND_SELFTEST_FAIL,
        sizeof(SOUND_SELFTEST_FAIL) / sizeof(SOUND_SELFTEST_FAIL[0])
      );
      delay(1500);
    }
  }

  Serial.print("[BT] Nome: ");
  Serial.println(DEVICE_NAME);

  if (selfTestPassed) {
    Serial.println("[SELFTEST] Sistema pronto.");
  } else {
    Serial.println("[SELFTEST] Bluetooth disponivel somente para diagnostico.");
  }

  Serial.println("[BT] Aguardando conexao...");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  updateBuzzer();
  updateBatteryMonitor(false);
  readBluetoothCommands();

  // ----------------------------------------------------------
  // Detecta conexao/desconexao Bluetooth.
  // ----------------------------------------------------------

  bool clientNow = SerialBT.hasClient();

  if (clientNow && !previousClient) {
    Serial.println("[BT] Cliente conectado.");

    sendLine(String("READY,") + HANDSHAKE_ID);

    // Envia bateria imediatamente ao novo cliente.
    updateBatteryMonitor(true);

    // Som exclusivo de conexao Bluetooth.
    startBuzzerPattern(
      SOUND_BT_CONNECTED,
      sizeof(SOUND_BT_CONNECTED) / sizeof(SOUND_BT_CONNECTED[0])
    );

    // Se o auto teste falhou, informa a causa.
    if (!selfTestPassed) {
      sendStatus(selfTestStatus);
    }

    // Mantem compatibilidade com a regra da V4.4:
    // informa no reconectar se o ultimo ensaio caiu por Bluetooth.
    if (abortedByBluetooth) {
      sendLine("STATUS,ABORT_BT_DISCONNECT");
    }

    // Outros abortos pendentes.
    if (pendingStatus.length() &&
        pendingStatus != "ABORT_BT_DISCONNECT") {
      sendStatus(pendingStatus);
      pendingStatus = "";
    }
  }

  else if (!clientNow && previousClient) {
    Serial.println("[BT] Cliente desconectado.");

    // Regra do projeto:
    // queda de Bluetooth em qualquer fase ativa invalida o ciclo.
    if (phase != PHASE_IDLE) {
      abortBluetoothDisconnect();
    }
  }

  previousClient = clientNow;

  // ----------------------------------------------------------
  // Transicoes temporizadas autonomas.
  // ----------------------------------------------------------

  unsigned long now = millis();

  if (
    phase == PHASE_HEATING &&
    now - phaseStartMs >= heatDurationMs
  ) {
    startCooling();
  }

  if (
    phase == PHASE_COOLING &&
    now - phaseStartMs >= coolDurationMs
  ) {
    finishCycle();
  }

  // ----------------------------------------------------------
  // Envia amostras de 1 em 1 segundo.
  // ----------------------------------------------------------

  if (
    phase != PHASE_IDLE &&
    (lastSampleMs == 0 ||
     now - lastSampleMs >= SAMPLE_INTERVAL_MS)
  ) {
    lastSampleMs = now;
    emitSample();
  }

  delay(5);
}
