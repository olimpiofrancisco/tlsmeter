/*
  ============================================================================
  TLS METER - FIRMWARE DISPLAY v2.0
  Board: ESP32-2432S028 / CYD 2.8"
  Display: ST7789 320x240
  Touch: XPT2046 (software SPI)
  SD: microSD onboard
  Communication: Bluetooth Classic SPP -> TLS-METER

  Projeto:
    Unidade de IHM portatil para o TLS Meter.
    O firmware de medicao da caixa permanece responsavel por aquisicao,
    acionamento do aquecedor, autoteste e segurancas.

  Bibliotecas:
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
    - ESP32 core: BluetoothSerial, Preferences, SD, SPI
    - Icones e logo: bitmaps raster embutidos em tls_assets.h

  IMPORTANTE:
    O touch usa os pinos 25/32/39/33 e e lido por SPI por software.
    Isso deixa o barramento VSPI de hardware livre para o microSD
    (18/19/23/5), enquanto o TFT usa HSPI (14/12/13/15).

  DISPLAY v2.0:
    - inclui menu de calibracao em Ajustes > Ensaio > Calibracao;
    - fator persistido na NVS da IHM (Preferences), independente do Windows;
    - ensaio da Amostra Padrao usa a resposta TLS bruta e requer confirmacao;
    - preserva o registro de dados brutos e associa fator ao ensaio;
    - NAO modifica firmware do medidor nem seus comandos de seguranca.

  DISPLAY v1.9:
    - mantem todas as funcoes da v1.8;
    - AJUSTES passa a ter submenu: TELA, ENSAIO, PC e SISTEMA;
    - TELA permite ajustar brilho em 10/25/50/75/100%, salvo em Preferences;
    - ENSAIO abre o antigo menu de ajustes de perfil/sonda;
    - PC desconecta voluntariamente a IHM do TLS-METER para liberar o
      Bluetooth do medidor ao software Windows;
    - VOLTAR A IHM tenta reconectar ao medidor e solicita PING + BAT?;
    - se o PC ainda estiver conectado, informa MEDIDOR OCUPADO e permite
      tentar novamente sem sair do modo PC;
    - SISTEMA mostra diagnostico resumido, permite retestar microSD e reiniciar;
    - preserva a invalidacao da bateria ao perder Bluetooth;
    - preserva o autoteste robusto do microSD da v1.7/v1.8.
  ============================================================================
*/

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/Org_01.h>
#include <math.h>
#include "tls_assets.h"

// ============================================================================
// PINAGEM DA PLACA ESP32-2432S028
// ============================================================================

// TFT ST7789 - HSPI
#define TFT_MISO   12
#define TFT_MOSI   13
#define TFT_CLK    14
#define TFT_CS     15
#define TFT_DC      2
#define TFT_RST    -1
#define TFT_BL     21

// Touch XPT2046 - pinos fisicos dedicados (software SPI)
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// microSD - barramento VSPI de hardware
#define SD_CS       5
#define SD_SCK     18
#define SD_MISO    19
#define SD_MOSI    23

#define SCREEN_W   320
#define SCREEN_H   240

// Calibracao inicial baseada no firmware de diagnostico fornecido
#define TOUCH_X_MIN 300
#define TOUCH_X_MAX 3750
#define TOUCH_Y_MIN 300
#define TOUCH_Y_MAX 3750

// ============================================================================
// BLUETOOTH - MODULO DE MEDICAO
// ============================================================================

static const char *BT_LOCAL_NAME  = "TLS-DISPLAY";
static const char *BT_TARGET_NAME = "TLS-METER";
static const char *DISPLAY_FW_VERSION = "2.0";

// MAC observado no modulo de medicao
uint8_t TLS_METER_MAC[6] = {0x3C, 0xE9, 0x0E, 0x0D, 0xB7, 0xFE};

// ============================================================================
// OBJETOS
// ============================================================================

SPIClass tftSpi(HSPI);
SPIClass sdSpi(VSPI);

Adafruit_ST7789 tft(&tftSpi, TFT_CS, TFT_DC, TFT_RST);
BluetoothSerial SerialBT;
Preferences prefs;
File logFile;

// ============================================================================
// CORES
// ============================================================================

#define C_BG        0x0861
#define C_PANEL     0x10E3
#define C_PANEL2    0x1945
#define C_BORDER    0x29A8
#define C_TEXT      0xFFFF
#define C_MUTED     0x8410
#define C_CYAN      0x4E7F
#define C_BLUE      0x349F
#define C_GREEN     0x36E8
#define C_YELLOW    0xFEA0
#define C_ORANGE    0xFC60
#define C_RED       0xF2A6
#define C_DARKRED   0x7000
#define C_WHITE     0xFFFF
#define C_BLACK     0x0000

// ============================================================================
// CONFIGURACAO DO ENSAIO
// ============================================================================

enum Profile {
  PROFILE_DEV   = 0,
  PROFILE_LAB   = 1,
  PROFILE_FIELD = 2
};
Profile currentProfile = PROFILE_DEV;

float activeLengthMm = 100.0f;
float probeDiameterMm = 1.5f;

// Calibracao local da IHM: k_corr = F*k_bruto; rho_corr = rho_bruto/F.
// Nao existe compartilhamento automatico com o software Windows.
float calibrationReferenceDraft = 0.0f;  // obrigatorio informar pela ficha do corpo de prova
uint8_t calibrationStepIndex = 1;      // passo inicial de 0,01 W/(m.K)
const float calibrationSteps[4] = {0.1f,0.01f,0.001f,0.0001f};
const char *calibrationStepNames[4] = {"0.1","0.01","0.001","0.0001"};
float calibrationFactor = 1.0f;
float calibrationReferenceUsed = 0.0f;
float calibrationLastRawK = 0.0f;
bool calibrationActive = false;
bool calibrationValid = false;
bool calibrationRun = false;
float calibrationRunReference = 0.0f;
float factorForCurrentTest = 1.0f;


// Opcoes especificas dos perfis Laboratorio e Campo.
// O perfil Rapido permanece fixo como na versao anterior.
uint8_t labStabMin = 10;
uint8_t labHeatMin = 10;
uint8_t fieldStabMin = 10;
uint8_t fieldHeatMin = 40;

// Parametros internos - nao editaveis na tela principal
const float STAB_DRIFT_LIMIT_C_PER_MIN = 0.05f;
const uint32_t STAB_DRIFT_WINDOW_MS = 120000UL;

const char *profileName() {
  switch (currentProfile) {
    case PROFILE_LAB:   return "Laboratório";
    case PROFILE_FIELD: return "Campo";
    default:            return "Rápido";
  }
}

const char *profileFileName() {
  switch (currentProfile) {
    case PROFILE_LAB:   return "LABORATORIO";
    case PROFILE_FIELD: return "CAMPO";
    default:            return "RAPIDO";
  }
}

bool profileUsesDriftCriterion() {
  return currentProfile == PROFILE_LAB || currentProfile == PROFILE_FIELD;
}

uint32_t profileStabMinMs() {
  if (currentProfile == PROFILE_LAB)   return (uint32_t)labStabMin * 60000UL;
  if (currentProfile == PROFILE_FIELD) return (uint32_t)fieldStabMin * 60000UL;
  return 10000UL;  // Rapido: 10 s, permanece fixo
}

uint32_t profileHeatMs() {
  if (currentProfile == PROFILE_LAB)   return (uint32_t)labHeatMin * 60000UL;
  if (currentProfile == PROFILE_FIELD) return (uint32_t)fieldHeatMin * 60000UL;
  return 120000UL; // Rapido: 120 s, permanece fixo
}

uint32_t profileCoolMs() {
  if (currentProfile == PROFILE_LAB)   return 120000UL;
  if (currentProfile == PROFILE_FIELD) return 120000UL;
  return 30000UL; // Rapido: 30 s, permanece fixo
}

// ============================================================================
// ESTADOS
// ============================================================================

enum ScreenState {
  SCR_SPLASH, SCR_HOME, SCR_LIVE, SCR_RESULTS, SCR_HISTORY,
  SCR_HISTORY_DETAIL,
  SCR_SETTINGS_MENU, SCR_DISPLAY_SETTINGS, SCR_SETTINGS, SCR_PROFILE_OPTIONS,
  SCR_PC_SETTINGS, SCR_SYSTEM_SETTINGS,
  SCR_CAL_SETTINGS, SCR_CAL_REVIEW, SCR_CAL_RESET,
  SCR_CREDITS, SCR_ERROR
};

enum TestStage {
  ST_IDLE, ST_STABILIZING, ST_HEATING, ST_COOLING, ST_PROCESSING, ST_DONE
};

ScreenState screenState = SCR_SPLASH;
TestStage testStage = ST_IDLE;

bool btOK = false;
bool sdOK = false;
// Fica true somente quando o cartao foi de fato detectado, mas falhou
// no teste de diretorio/escrita/leitura. Ausencia de cartao nao aciona popup.
bool sdDetectedButFailed = false;
bool sdWarningActive = false;
uint32_t sdWarningUntil = 0;

// Confirma o salvamento do ensaio atual, separado de sdOK.
bool testSavedOK = false;

bool meterReady = false;
bool userStopRequested = false;
bool testActive = false;

String lastErrorCode = "";
String lastErrorTitle = "";
String lastErrorLine1 = "";
String lastErrorLine2 = "";

uint32_t lastBTConnectAttempt = 0;
uint32_t lastUIRefresh = 0;
uint32_t stageStartMs = 0;
uint32_t testStartMs = 0;

// Aviso temporario e nao bloqueante na HOME quando o medidor para de responder.
bool homeWarningActive = false;
uint32_t homeWarningUntil = 0;

// Bateria recebida do ESP32 de medicao.
// Protocolo: BAT,<tensao_mV>,<percentual>,<CHARGING|IDLE>
bool batteryPacketSeen = false;
bool batteryValid = false;
bool batteryCharging = false;
int batteryPercent = -1;
uint16_t batteryMv = 0;

// Popup informativo de carregamento. Nao bloqueia o loop nem o ensaio.
bool batteryPopupActive = false;
uint32_t batteryPopupUntil = 0;
ScreenState batteryPopupScreen = SCR_HOME;

// Modo PC:
// true = a IHM se mantem desconectada do TLS-METER para que o computador
// possa assumir a unica conexao Bluetooth SPP disponivel no medidor.
bool pcMode = false;
bool pcReconnectFailed = false;

// Brilho do backlight do display. Persistido em Preferences.
uint8_t screenBrightnessPercent = 75;

float liveTempC = NAN;
float livePowerW = 0.0f;
float liveQ = 0.0f;
float t0C = NAN;
float liveDriftCPerMin = NAN;

String btLine = "";

// ============================================================================
// DADOS PARA ESTABILIZACAO / ANALISE / GRAFICO
// ============================================================================

const int STAB_BUF = 180;
float stabTemp[STAB_BUF];
uint32_t stabMs[STAB_BUF];
int stabCount = 0;
uint32_t lastStabStoredMs = 0;

const int MAX_HEAT_SAMPLES = 1600;
float heatT[MAX_HEAT_SAMPLES];
float heatTemp[MAX_HEAT_SAMPLES];
float heatPower[MAX_HEAT_SAMPLES];
int heatCount = 0;
uint32_t lastHeatStoredMs = 0;

struct TLSResult {
  bool valid;
  float rho, k, r2, slope, pMean, qMean, tStart, tEnd, logCycles;
  int i0, i1;
};

TLSResult resultTLS = {false,0,0,0,0,0,0,0,0,0,-1,-1};

const int GRAPH_N = 120;
float graphTemp[GRAPH_N];
uint32_t graphMs[GRAPH_N];
int graphCount = 0;
uint32_t lastGraphStoredMs = 0;

// ============================================================================
// ARQUIVOS / HISTORICO
// ============================================================================

uint16_t currentTestId = 0;
String currentCsvPath = "";
String currentSummaryPath = "";
int logFlushCounter = 0;

const int MAX_HISTORY = 30;
String historyFiles[MAX_HISTORY];
int historyCount = 0;
int historyOffset = 0;
String selectedHistoryFile = "";

// ============================================================================
// TOUCH
// ============================================================================

bool touchWasDown = false;
uint32_t lastTapMs = 0;

// ============================================================================
// PROTOTIPOS
// ============================================================================

void drawHome();
void drawLive(bool fullRedraw);
void drawResults();
void drawHistory();
void drawHistoryDetail(const String &path);
void drawSettingsMenu();
void drawDisplaySettings();
void drawSettings();
void drawCalibrationSettings();
void drawCalibrationReview();
void drawCalibrationReset();
void drawProfileOptions();
void drawPCSettings();
void drawSystemSettings();
void drawCredits();
void drawError();
void showHomeNoResponse();
void showSDFailurePopup();
void showChargingPopup();
void updateTransientUI();
void redrawCurrentScreen();
void drawHeaderIndicators();
void processBluetooth();
void processBTLine(const String &line);
void processBatteryPacket(const String &line);
void invalidateBatteryTelemetry();
bool connectMeter(bool showScreen);
void enterPCMode();
void attemptReturnToIHM();
void applyScreenBrightness();
void startNewTest();
void stopTestByUser();
void finalizeTest();
void analyzeTLS();
void scanHistory();

// ============================================================================
// DESENHO / ICONES
// ============================================================================


enum PtAccent {
  PT_NONE,
  PT_ACUTE,
  PT_GRAVE,
  PT_CIRC,
  PT_TILDE,
  PT_CEDILLA
};

int ptAccentUp = 7;
int ptCedillaDown = 2;

void useDefaultFont() {
  tft.setFont(NULL);
  tft.setTextSize(1);
  ptAccentUp = 7;
  ptCedillaDown = 2;
}

void useCompactFont() {
  tft.setFont(&Org_01);
  tft.setTextSize(1);
  ptAccentUp = 7;
  ptCedillaDown = 2;
}

void useSmallFont() {
  tft.setFont(&FreeSans9pt7b);
  tft.setTextSize(1);
  ptAccentUp = 15;
  ptCedillaDown = 3;
}

void useSmallBoldFont() {
  tft.setFont(&FreeSansBold9pt7b);
  tft.setTextSize(1);
  ptAccentUp = 15;
  ptCedillaDown = 3;
}

// Itens principais da HOME: todos usam exatamente a mesma fonte/tamanho.
void useMenuFont() {
  tft.setFont(&FreeSans9pt7b);
  tft.setTextSize(1);
  ptAccentUp = 15;
  ptCedillaDown = 3;
}

// Créditos: fonte regular para ficar discretamente menor que os itens principais.
void useCreditsHomeFont() {
  tft.setFont(&Org_01); 
  tft.setTextSize(1);
  ptAccentUp = 11;
  ptCedillaDown = 2;
}

// Texto dentro da tela de créditos.
void useCreditsFont() {
  tft.setFont(&FreeSans9pt7b);
  tft.setTextSize(1);
  ptAccentUp = 15;
  ptCedillaDown = 3;
}

void useTitleFont() {
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextSize(1);
  ptAccentUp = 20;
  ptCedillaDown = 4;
}

void setFontPT(const GFXfont *font) {
  tft.setFont(font);
  tft.setTextSize(1);

  if (font == &FreeSansBold12pt7b) {
    ptAccentUp = 20;
    ptCedillaDown = 4;
  } else if (font == &Org_01) {
    ptAccentUp = 7;
    ptCedillaDown = 2;
  } else {
    ptAccentUp = 15;
    ptCedillaDown = 3;
  }
}

bool decodePtChar(const char *&p, char &base, uint8_t &accent) {
  uint8_t c0 = (uint8_t)*p;

  if (c0 < 0x80) {
    base = (char)c0;
    accent = PT_NONE;
    p++;
    return true;
  }

  if (c0 == 0xC3 && p[1] != '\0') {
    uint8_t c1 = (uint8_t)p[1];
    p += 2;

    switch (c1) {
      // A / a
      case 0x81: base='A'; accent=PT_ACUTE;   return true; // Á
      case 0xA1: base='a'; accent=PT_ACUTE;   return true; // á
      case 0x80: base='A'; accent=PT_GRAVE;   return true; // À
      case 0xA0: base='a'; accent=PT_GRAVE;   return true; // à
      case 0x82: base='A'; accent=PT_CIRC;    return true; // Â
      case 0xA2: base='a'; accent=PT_CIRC;    return true; // â
      case 0x83: base='A'; accent=PT_TILDE;   return true; // Ã
      case 0xA3: base='a'; accent=PT_TILDE;   return true; // ã

      // E / e
      case 0x89: base='E'; accent=PT_ACUTE;   return true; // É
      case 0xA9: base='e'; accent=PT_ACUTE;   return true; // é
      case 0x8A: base='E'; accent=PT_CIRC;    return true; // Ê
      case 0xAA: base='e'; accent=PT_CIRC;    return true; // ê

      // I / i
      case 0x8D: base='I'; accent=PT_ACUTE;   return true; // Í
      case 0xAD: base='i'; accent=PT_ACUTE;   return true; // í

      // O / o
      case 0x93: base='O'; accent=PT_ACUTE;   return true; // Ó
      case 0xB3: base='o'; accent=PT_ACUTE;   return true; // ó
      case 0x94: base='O'; accent=PT_CIRC;    return true; // Ô
      case 0xB4: base='o'; accent=PT_CIRC;    return true; // ô
      case 0x95: base='O'; accent=PT_TILDE;   return true; // Õ
      case 0xB5: base='o'; accent=PT_TILDE;   return true; // õ

      // U / u
      case 0x9A: base='U'; accent=PT_ACUTE;   return true; // Ú
      case 0xBA: base='u'; accent=PT_ACUTE;   return true; // ú

      // C / c
      case 0x87: base='C'; accent=PT_CEDILLA; return true; // Ç
      case 0xA7: base='c'; accent=PT_CEDILLA; return true; // ç
    }
  }

  // Caractere UTF-8 não tratado: avança e usa '?'.
  if ((c0 & 0xE0) == 0xC0 && p[1] != '\0') p += 2;
  else if ((c0 & 0xF0) == 0xE0 && p[1] != '\0' && p[2] != '\0') p += 3;
  else p++;

  base = '?';
  accent = PT_NONE;
  return true;
}

String ptAscii(const char *utf8) {
  String out;
  const char *p = utf8;
  while (*p) {
    char base;
    uint8_t accent;
    decodePtChar(p, base, accent);
    out += base;
  }
  return out;
}

void drawPtAccent(int x0, int x1, int baselineY, uint8_t accent, uint16_t color) {
  if (accent == PT_NONE) return;

  int mid = (x0 + x1) / 2;
  int ay = baselineY - ptAccentUp;

  switch (accent) {
    case PT_ACUTE:
      tft.drawLine(mid - 2, ay + 2, mid + 2, ay - 2, color);
      break;

    case PT_GRAVE:
      tft.drawLine(mid - 2, ay - 2, mid + 2, ay + 2, color);
      break;

    case PT_CIRC:
      tft.drawLine(mid - 4, ay + 1, mid, ay - 3, color);
      tft.drawLine(mid, ay - 3, mid + 4, ay + 1, color);
      break;

    case PT_TILDE:
      tft.drawLine(mid - 5, ay, mid - 3, ay - 2, color);
      tft.drawLine(mid - 3, ay - 2, mid, ay - 2, color);
      tft.drawLine(mid, ay - 2, mid + 3, ay, color);
      tft.drawLine(mid + 3, ay, mid + 5, ay - 1, color);
      break;

    case PT_CEDILLA:
      tft.drawLine(mid, baselineY + 1, mid - 2, baselineY + ptCedillaDown + 1, color);
      tft.drawLine(mid - 2, baselineY + ptCedillaDown + 1, mid + 1, baselineY + ptCedillaDown + 2, color);
      break;

    default:
      break;
  }
}

void printPT(const char *utf8) {
  const char *p = utf8;
  uint16_t color = C_TEXT;

  // Adafruit_GFX não expõe a cor atual; nas chamadas desta IHM a cor é
  // sempre definida imediatamente antes de printPT. Para o acento,
  // usamos C_TEXT quando necessário e as funções abaixo passam a cor
  // explicitamente em textos centralizados. Em texto corrido, o desenho
  // do acento é atualizado pelo helper printPTColor.
  while (*p) {
    char base;
    uint8_t accent;
    decodePtChar(p, base, accent);

    int x0 = tft.getCursorX();
    int y0 = tft.getCursorY();
    tft.write((uint8_t)base);
    int x1 = tft.getCursorX();

    if (accent != PT_NONE) drawPtAccent(x0, x1, y0, accent, color);
  }
}

void printPTColor(const char *utf8, uint16_t color) {
  const char *p = utf8;
  while (*p) {
    char base;
    uint8_t accent;
    decodePtChar(p, base, accent);

    int x0 = tft.getCursorX();
    int y0 = tft.getCursorY();
    tft.write((uint8_t)base);
    int x1 = tft.getCursorX();

    if (accent != PT_NONE) drawPtAccent(x0, x1, y0, accent, color);
  }
}

void drawCenteredFontText(const char *s, int x, int y, int w, int h,
                          const GFXfont *font, uint16_t color) {
  setFontPT(font);
  tft.setTextColor(color);

  String ascii = ptAscii(s);

  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(ascii.c_str(), 0, 0, &x1, &y1, &tw, &th);

  int16_t cx = x + (w - (int)tw) / 2 - x1;
  int16_t cy = y + (h - (int)th) / 2 - y1;

  tft.setCursor(cx, cy);
  printPTColor(s, color);
}

void drawCenteredOrgText(const char *s, int x, int y, int w, int h, uint16_t color) {
  drawCenteredFontText(s, x, y, w, h, &Org_01, color);
  useDefaultFont();
}

void buttonCompact(int x,int y,int w,int h,const char *label,uint16_t accent,bool active=true) {
  uint16_t bg=active?C_PANEL2:C_PANEL;
  tft.fillRoundRect(x,y,w,h,8,bg);
  tft.drawRoundRect(x,y,w,h,8,active?accent:C_BORDER);
  drawCenteredOrgText(label,x,y,w,h,active?C_TEXT:C_MUTED);
}

// Mantido para elementos muito pequenos da interface.
// Para textos com acentos, use drawCenteredFontText().
void centerText(const char *s, int x, int y, int w, uint8_t size, uint16_t color) {
  useDefaultFont();
  int16_t x1, y1;
  uint16_t tw, th;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + (w - tw) / 2, y);
  tft.print(s);
}

void drawWrappedPT(const String &text, int x, int baselineY, int maxW,
                   int lineStep, int maxLines, const GFXfont *font,
                   uint16_t color) {
  setFontPT(font);
  tft.setTextColor(color);

  String remaining = text;
  remaining.trim();
  int lineNo = 0;

  while (remaining.length() && lineNo < maxLines) {
    int bestCut = remaining.length();
    String lineText = remaining;

    while (bestCut > 0) {
      lineText = remaining.substring(0, bestCut);
      lineText.trim();

      String ascii = ptAscii(lineText.c_str());
      int16_t bx, by;
      uint16_t bw, bh;
      tft.getTextBounds(ascii.c_str(), 0, 0, &bx, &by, &bw, &bh);

      if ((int)bw <= maxW) break;

      int prevSpace = remaining.lastIndexOf(' ', bestCut - 1);
      if (prevSpace < 0) {
        bestCut--;
      } else {
        bestCut = prevSpace;
      }
    }

    if (bestCut <= 0) break;

    String ascii = ptAscii(lineText.c_str());
    int16_t bx, by;
    uint16_t bw, bh;
    tft.getTextBounds(ascii.c_str(), 0, 0, &bx, &by, &bw, &bh);

    int cx = x + (maxW - (int)bw) / 2 - bx;
    tft.setCursor(cx, baselineY + lineNo * lineStep);
    printPTColor(lineText.c_str(), color);

    remaining = remaining.substring(bestCut);
    remaining.trim();
    lineNo++;
  }
}

void fillGradientV(int x, int y, int w, int h, uint16_t top, uint16_t bottom) {
  uint8_t r0=(top>>11)&0x1F, g0=(top>>5)&0x3F, b0=top&0x1F;
  uint8_t r1=(bottom>>11)&0x1F, g1=(bottom>>5)&0x3F, b1=bottom&0x1F;
  for (int row=0; row<h; row++) {
    uint8_t r=r0+(r1-r0)*row/max(1,h);
    uint8_t g=g0+(g1-g0)*row/max(1,h);
    uint8_t b=b0+(b1-b0)*row/max(1,h);
    uint16_t cc=((r&0x1F)<<11)|((g&0x3F)<<5)|(b&0x1F);
    tft.drawFastHLine(x,y+row,w,cc);
  }
}

void panel(int x,int y,int w,int h,uint16_t border=C_BORDER,uint16_t fill=C_PANEL) {
  tft.fillRoundRect(x,y,w,h,8,fill);
  tft.drawRoundRect(x,y,w,h,8,border);
}

void drawBitmapIcon(const uint8_t *bitmap, int x, int y, uint16_t color) {
  tft.drawBitmap(x, y, bitmap, TLS_ICON_W, TLS_ICON_H, color);
}

void button(int x,int y,int w,int h,const char *label,uint16_t accent,bool active=true) {
  uint16_t bg=active?C_PANEL2:C_PANEL;
  tft.fillRoundRect(x,y,w,h,8,bg);
  tft.drawRoundRect(x,y,w,h,8,active?accent:C_BORDER);
  drawCenteredFontText(label,x,y,w,h,&FreeSansBold9pt7b,active?C_TEXT:C_MUTED);
  useDefaultFont();
}

uint16_t batteryUiColor() {
  if(!batteryPacketSeen || !batteryValid) return C_MUTED;
  if(batteryCharging) return C_CYAN;
  if(batteryPercent < 15) return C_RED;
  if(batteryPercent < 30) return C_ORANGE;
  if(batteryPercent < 60) return C_YELLOW;
  return C_GREEN;
}

const uint8_t *batteryUiBitmap() {
  if(!batteryPacketSeen || !batteryValid) return TLS_BATTERY_0;

  if(batteryPercent <= 15) return TLS_BATTERY_1;
  if(batteryPercent <= 40) return TLS_BATTERY_2;
  if(batteryPercent <= 70) return TLS_BATTERY_3;
  return TLS_BATTERY_4;
}

void drawHeaderIndicators() {
  // Redesenha apenas a area direita do cabecalho para permitir
  // atualizacao da bateria a cada pacote BAT sem redesenhar a tela toda.
  fillGradientV(176,0,SCREEN_W-176,34,0x10C4,C_BG);
  tft.drawFastHLine(176,34,SCREEN_W-176,C_BORDER);

  useDefaultFont();

  // Bluetooth
  tft.fillCircle(185,17,5,btOK?C_GREEN:C_RED);
  tft.setTextColor(C_MUTED);
  tft.setCursor(193,13);
  tft.print("BT");

  // microSD
  tft.fillCircle(218,17,5,sdOK?C_GREEN:C_RED);
  tft.setCursor(226,13);
  tft.print("SD");

  // Bateria
  uint16_t batColor = batteryUiColor();
  const uint8_t *batBitmap = batteryUiBitmap();

  tft.drawBitmap(250,11,batBitmap,TLS_BATTERY_W,TLS_BATTERY_H,batColor);

  // Pequeno raio raster sobre a bateria enquanto CHRG estiver ativo.
  if(batteryCharging) {
    tft.drawBitmap(256,10,TLS_BATTERY_BOLT,
                   TLS_BATTERY_BOLT_W,TLS_BATTERY_BOLT_H,C_YELLOW);
  }

  tft.setTextColor(batColor);
  tft.setCursor(273,13);

  if(batteryPacketSeen && batteryValid && batteryPercent >= 0) {
    tft.print(batteryPercent);
    tft.print("%");
  } else {
    tft.print("--%");
  }
}

void drawHeader(const char *title) {
  fillGradientV(0,0,SCREEN_W,34,0x10C4,C_BG);
  tft.drawFastHLine(0,34,SCREEN_W,C_BORDER);

  tft.setTextColor(C_CYAN);
  useTitleFont();
  tft.setCursor(7,25);
  printPTColor(title,C_CYAN);

  drawHeaderIndicators();
}

void drawFooter(const char *leftText="") {
  tft.fillRect(0,220,SCREEN_W,20,C_PANEL);
  tft.drawFastHLine(0,220,SCREEN_W,C_BORDER);
  useDefaultFont();
  tft.setTextColor(C_MUTED);
  tft.setCursor(5,227);
  tft.print(leftText);
}

void tile(int x,int y,int w,int h,const char *label,uint16_t color,const uint8_t *iconBitmap){
  panel(x,y,w,h,color,C_PANEL);

  // Um pouco mais de espaço horizontal para o texto.
  int iconX=x+8;
  int iconY=y+(h-TLS_ICON_H)/2;
  drawBitmapIcon(iconBitmap,iconX,iconY,color);

  tft.setTextColor(C_TEXT);
  useMenuFont();

  const int labelX=x+43;
  const int labelW=w-47;

  if(strcmp(label,"INICIAR ENSAIO")==0){
    tft.setCursor(labelX,y+23);
    printPTColor("INICIAR",C_TEXT);
    tft.setCursor(labelX,y+43);
    printPTColor("ENSAIO",C_TEXT);
  } else {
    String ascii=ptAscii(label);
    int16_t x1,y1;
    uint16_t tw,th;
    tft.getTextBounds(ascii.c_str(),0,0,&x1,&y1,&tw,&th);

    // HISTÓRICO, CONECTADO/CONECTAR e AJUSTES usam a MESMA fonte.
    int ty=y+(h-(int)th)/2-y1;
    tft.setCursor(labelX,ty);
    printPTColor(label,C_TEXT);
  }

  useDefaultFont();
}

void tileCredits(int x,int y,int w,int h,const char *label,uint16_t color,const uint8_t *iconBitmap){
  panel(x,y,w,h,color,C_PANEL);
  int iconX=x+12;
  int iconY=y+(h-TLS_ICON_H)/2;
  drawBitmapIcon(iconBitmap,iconX,iconY,color);

  tft.setTextColor(C_TEXT);
  useCreditsHomeFont();

  String ascii=ptAscii(label);
  int16_t x1,y1;
  uint16_t tw,th;
  tft.getTextBounds(ascii.c_str(),0,0,&x1,&y1,&tw,&th);

  int labelX=x+55;
  int labelW=w-63;
  int tx=labelX+(labelW-(int)tw)/2-x1;
  int ty=y+(h-(int)th)/2-y1;

  tft.setCursor(tx,ty);
  printPTColor(label,C_TEXT);
  useDefaultFont();
}

void drawConnectionPopup(const char *status, const char *detail, uint16_t color) {
  tft.fillRoundRect(26,68,268,104,10,C_PANEL2);
  tft.drawRoundRect(26,68,268,104,10,color);
  drawBitmapIcon(TLS_ICON_BLUETOOTH,42,104,color);

  tft.setTextColor(color);
  useSmallBoldFont();
  tft.setCursor(88,103);
  printPTColor(status,color);

  // Texto aumentado novamente na v1.4.
  tft.setTextColor(C_MUTED);
  useSmallFont();
  tft.setCursor(88,137);
  printPTColor(detail,C_MUTED);

  useDefaultFont();
}

// ============================================================================
// TOUCH - SOFTWARE SPI
// ============================================================================

uint8_t touchTransfer(uint8_t data) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TOUCH_CLK, LOW);
    digitalWrite(TOUCH_MOSI, (data >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(TOUCH_CLK, HIGH);
    in = (in << 1) | (digitalRead(TOUCH_MISO) ? 1 : 0);
    delayMicroseconds(1);
  }
  digitalWrite(TOUCH_CLK, LOW);
  return in;
}

uint16_t touchReadRaw(uint8_t cmd) {
  touchTransfer(cmd);
  uint16_t val = ((uint16_t)touchTransfer(0) << 8) | touchTransfer(0);
  return val >> 3;
}

bool readTouch(int16_t &tx, int16_t &ty) {
  if (digitalRead(TOUCH_IRQ) == HIGH) return false;
  digitalWrite(TOUCH_CS, LOW);

  const int SAMPLES = 8;
  long rawA[SAMPLES], rawB[SAMPLES];
  for (int i=0;i<SAMPLES;i++) {
    rawA[i] = touchReadRaw(0x90);
    rawB[i] = touchReadRaw(0xD0);
  }
  digitalWrite(TOUCH_CS, HIGH);
  if (digitalRead(TOUCH_IRQ) == HIGH) return false;

  for (int i=1;i<SAMPLES;i++) {
    for (int j=i;j>0 && rawA[j]<rawA[j-1];j--) { long t=rawA[j]; rawA[j]=rawA[j-1]; rawA[j-1]=t; }
    for (int j=i;j>0 && rawB[j]<rawB[j-1];j--) { long t=rawB[j]; rawB[j]=rawB[j-1]; rawB[j-1]=t; }
  }

  long sumA=0,sumB=0;
  for (int i=2;i<SAMPLES-2;i++) { sumA+=rawA[i]; sumB+=rawB[i]; }
  uint16_t xRaw=sumA/(SAMPLES-4), yRaw=sumB/(SAMPLES-4);

  if (xRaw<200 || xRaw>3900 || yRaw<200 || yRaw>3900) return false;
  tx = map(xRaw,TOUCH_X_MIN,TOUCH_X_MAX,0,SCREEN_W);
  ty = map(yRaw,TOUCH_Y_MIN,TOUCH_Y_MAX,0,SCREEN_H);
  tx = constrain(tx,0,SCREEN_W-1); ty = constrain(ty,0,SCREEN_H-1);
  return true;
}

bool getTap(int16_t &x,int16_t &y) {
  bool down=readTouch(x,y); uint32_t now=millis();
  if(down && !touchWasDown && (now-lastTapMs>220)){ touchWasDown=true; lastTapMs=now; return true; }
  if(!down) touchWasDown=false;
  return false;
}

bool inRect(int16_t x,int16_t y,int rx,int ry,int rw,int rh){ return x>=rx && x<rx+rw && y>=ry && y<ry+rh; }

// ============================================================================
// SPLASH / HOME / CONEXAO
// ============================================================================

void drawSplash(){
  screenState=SCR_SPLASH;
  tft.fillScreen(C_BG);

  int x=(SCREEN_W-TLS_LOGO_W)/2;
  int y=6;
  tft.drawRGBBitmap(x,y,TLS_LOGO_MAIN,TLS_LOGO_W,TLS_LOGO_H);

  tft.setTextColor(C_MUTED);
  useSmallFont();
  drawCenteredFontText("Inicializando...",0,218,SCREEN_W,20,&FreeSans9pt7b,C_MUTED);
  useDefaultFont();
}

void drawHome(){
  screenState=SCR_HOME;
  tft.fillScreen(C_BG);
  drawHeader("TLS METER");

  tile(8,42,148,58,"INICIAR ENSAIO",btOK?C_GREEN:C_BORDER,TLS_ICON_PLAY);
  tile(164,42,148,58,"HISTÓRICO",C_CYAN,TLS_ICON_HISTORY);
  tile(8,106,148,58,pcMode?"MODO PC":(btOK?"ATIVO":"ATIVAR"),pcMode?C_ORANGE:(btOK?C_GREEN:C_ORANGE),TLS_ICON_BLUETOOTH);
  tile(164,106,148,58,"AJUSTES",C_YELLOW,TLS_ICON_SETTINGS);
  tileCredits(8,170,304,34,"CREDITOS",C_BLUE,TLS_ICON_CREDITS);

/*
  useCompactFont();
  tft.setTextColor(btOK?C_GREEN:C_RED);
  tft.setCursor(10,211);
  printPTColor(btOK?"TLS-METER disponivel":"TLS-METER desconectado",btOK?C_GREEN:C_RED);

  tft.setTextColor(sdOK?C_GREEN:C_RED);
  tft.setCursor(220,211);
  printPTColor(sdOK?"microSD OK":"sem microSD",sdOK?C_GREEN:C_RED);
*/
  useDefaultFont();

  String footer=String("Perfil de ensaio: ")+profileName();
  tft.fillRect(0,220,SCREEN_W,20,C_PANEL);
  tft.drawFastHLine(0,220,SCREEN_W,C_BORDER);
  useSmallFont();
  tft.setTextColor(C_MUTED);
  tft.setCursor(5,235);
  printPTColor(footer.c_str(),C_MUTED);
  useDefaultFont();
}

// Aviso nao modal: permanece por 3 s, mas a HOME continua aceitando toques.
// Historico, Creditos e Ajustes continuam acessiveis.
void showHomeNoResponse(){
  if(screenState!=SCR_HOME) return;

  homeWarningActive=true;
  homeWarningUntil=millis()+3000UL;

  tft.fillRoundRect(24,68,272,100,10,C_PANEL2);
  tft.drawRoundRect(24,68,272,100,10,C_ORANGE);
  drawBitmapIcon(TLS_ICON_WARNING,38,101,C_ORANGE);

  // Texto maior que na v1.3, mas dividido para caber com folga.
  tft.setTextColor(C_TEXT);
  useSmallFont();
  tft.setCursor(82,103);
  printPTColor("O medidor parou",C_TEXT);
  tft.setCursor(82,125);
  printPTColor("de responder.",C_TEXT);

  tft.setTextColor(C_ORANGE);
  useSmallBoldFont();
  tft.setCursor(82,151);
  printPTColor("Aguarde...",C_ORANGE);

  useDefaultFont();
}

void showSDFailurePopup(){
  if(screenState!=SCR_HOME || !sdDetectedButFailed) return;

  sdWarningActive=true;
  sdWarningUntil=millis()+5000UL;

  tft.fillRoundRect(22,64,276,112,10,C_PANEL2);
  tft.drawRoundRect(22,64,276,112,10,C_RED);
  drawBitmapIcon(TLS_ICON_STORAGE,36,103,C_RED);

  tft.setTextColor(C_RED);
  useSmallBoldFont();
  tft.setCursor(82,94);
  printPTColor("Cartão SD",C_RED);

  String msg="Cartão inserido, mas não passou no teste. Verifique o cartão.";
  drawWrappedPT(msg,78,122,204,20,3,&FreeSans9pt7b,C_TEXT);
  useDefaultFont();
}

void redrawCurrentScreen(){
  switch(screenState){
    case SCR_HOME:           drawHome(); break;
    case SCR_LIVE:           drawLive(true); break;
    case SCR_RESULTS:        drawResults(); break;
    case SCR_HISTORY:        drawHistory(); break;
    case SCR_HISTORY_DETAIL: drawHistoryDetail(selectedHistoryFile); break;
    case SCR_SETTINGS_MENU:  drawSettingsMenu(); break;
    case SCR_DISPLAY_SETTINGS:drawDisplaySettings(); break;
    case SCR_SETTINGS:       drawSettings(); break;
    case SCR_CAL_SETTINGS:   drawCalibrationSettings(); break;
    case SCR_CAL_REVIEW:     drawCalibrationReview(); break;
    case SCR_CAL_RESET:      drawCalibrationReset(); break;
    case SCR_PROFILE_OPTIONS:drawProfileOptions(); break;
    case SCR_PC_SETTINGS:    drawPCSettings(); break;
    case SCR_SYSTEM_SETTINGS:drawSystemSettings(); break;
    case SCR_CREDITS:        drawCredits(); break;
    case SCR_ERROR:          drawError(); break;
    default: break;
  }
}

void showChargingPopup(){
  if(screenState==SCR_SPLASH || screenState==SCR_ERROR) return;

  // Nao sobrepoe outros avisos temporarios da HOME.
  if(screenState==SCR_HOME && (homeWarningActive || sdWarningActive)) return;

  batteryPopupActive=true;
  batteryPopupUntil=millis()+3000UL;
  batteryPopupScreen=screenState;

  tft.fillRoundRect(26,70,268,100,10,C_PANEL2);
  tft.drawRoundRect(26,70,268,100,10,C_CYAN);

  // Icone raster ja existente no pacote de assets.
  drawBitmapIcon(TLS_ICON_BOLT,39,103,C_YELLOW);

  tft.setTextColor(C_CYAN);
  useSmallBoldFont();
  tft.setCursor(87,105);
  printPTColor("Bateria carregando",C_CYAN);

  tft.setTextColor(C_TEXT);
  useSmallFont();
  tft.setCursor(87,139);
  printPTColor("Carregamento detectado.",C_TEXT);

  useDefaultFont();
}

void updateTransientUI(){
  if(homeWarningActive && (int32_t)(millis()-homeWarningUntil)>=0){
    homeWarningActive=false;
    if(screenState==SCR_HOME) drawHome();
  }

  if(sdWarningActive && (int32_t)(millis()-sdWarningUntil)>=0){
    sdWarningActive=false;
    if(screenState==SCR_HOME) drawHome();
  }

  if(batteryPopupActive){
    // Se o usuario mudou de tela, a nova tela ja apagou o popup.
    if(screenState!=batteryPopupScreen){
      batteryPopupActive=false;
    }
    else if((int32_t)(millis()-batteryPopupUntil)>=0){
      batteryPopupActive=false;
      redrawCurrentScreen();
    }
  }
}

bool connectMeter(bool showScreen){
  if(SerialBT.connected()){
    btOK=true;
    if(showScreen){
      drawHome();
      drawConnectionPopup("Já conectado","TLS-METER",C_GREEN);
      delay(450);
    }
    return true;
  }

  if(showScreen){
    drawHome();
    drawConnectionPopup("Conectando...","Um momento...",C_CYAN);
    delay(100); // garante que o usuario veja o popup antes da chamada bloqueante
  }

  SerialBT.disconnect();
  delay(100);

  bool ok=SerialBT.connect(TLS_METER_MAC);
  if(!ok){
    delay(100);
    ok=SerialBT.connect(String(BT_TARGET_NAME));
  }

  btOK=ok && SerialBT.connected();

  if(btOK){
    SerialBT.println("PING");
    delay(40);
    SerialBT.println("BAT?");
    delay(80);
  }

  if(showScreen){
    if(btOK){
      drawConnectionPopup("Conectado","TLS-METER pronto",C_GREEN);
      delay(650);
    } else {
      drawConnectionPopup("Falha na conexão","Tente novamente",C_RED);
      delay(900);
    }
  }

  lastBTConnectAttempt=millis();
  return btOK;
}

// ============================================================================
// MICROSD
// ============================================================================

// Frequencias conservadoras para o slot microSD onboard.
// O TLS Meter grava poucos dados; estabilidade e mais importante que velocidade.
static const uint32_t SD_FREQ_PRIMARY  = 1000000UL;  // 1 MHz
static const uint32_t SD_FREQ_FALLBACK =  400000UL;  // 400 kHz
static const uint32_t SD_FREQ_SAFE     =  200000UL;  // 200 kHz
static const uint8_t  SD_TRIES_PER_FREQ = 2;

bool prepareSDBus(uint32_t frequency){
  // Libera qualquer montagem/tentativa anterior.
  SD.end();
  sdSpi.end();

  delay(80);

  // Garante os CS inativos antes de reinicializar o barramento.
  pinMode(SD_CS,OUTPUT);
  digitalWrite(SD_CS,HIGH);

  // Touch e TFT usam outros barramentos/pinos, mas mantemos seus CS
  // inativos durante a inicializacao do cartao.
  pinMode(TOUCH_CS,OUTPUT);
  digitalWrite(TOUCH_CS,HIGH);

  pinMode(TFT_CS,OUTPUT);
  digitalWrite(TFT_CS,HIGH);

  delay(20);

  sdSpi.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  delay(120);

  Serial.print("SD: tentando montar em ");
  Serial.print(frequency/1000UL);
  Serial.println(" kHz...");

  if(!SD.begin(SD_CS,sdSpi,frequency)){
    Serial.println("SD: SD.begin() falhou.");
    return false;
  }

  return true;
}

bool testSDCardAfterMount(){
  uint8_t cardType=SD.cardType();

  if(cardType==CARD_NONE){
    Serial.println("SD: CARD_NONE apos montagem.");
    return false;
  }

  Serial.print("SD: tipo = ");
  if(cardType==CARD_MMC) Serial.println("MMC");
  else if(cardType==CARD_SD) Serial.println("SDSC");
  else if(cardType==CARD_SDHC) Serial.println("SDHC/SDXC");
  else Serial.println("DESCONHECIDO");

  uint64_t cardSizeMB=SD.cardSize()/(1024ULL*1024ULL);
  Serial.print("SD: capacidade = ");
  Serial.print(cardSizeMB);
  Serial.println(" MB");

  if(cardSizeMB==0){
    Serial.println("SD: capacidade lida como 0 MB.");
    return false;
  }

  // ----------------------------------------------------------------
  // 1) Teste de escrita/leitura diretamente na raiz.
  // Isso separa problema de montagem de problema no diretorio /TLS.
  // ----------------------------------------------------------------
  const char *rootTest="/SDTEST.TMP";

  if(SD.exists(rootTest)){
    SD.remove(rootTest);
  }

  Serial.println("SD: testando escrita na raiz...");

  File rootFile=SD.open(rootTest,FILE_WRITE);
  if(!rootFile){
    Serial.println("SD: falha ao criar arquivo na raiz.");
    return false;
  }

  size_t rootWritten=rootFile.println("TLS-METER ROOT TEST");
  rootFile.flush();
  rootFile.close();

  if(rootWritten==0){
    Serial.println("SD: escrita na raiz retornou 0 bytes.");
    return false;
  }

  if(!SD.exists(rootTest)){
    Serial.println("SD: arquivo da raiz nao apareceu apos escrita.");
    return false;
  }

  File rootRead=SD.open(rootTest,FILE_READ);
  if(!rootRead){
    Serial.println("SD: nao conseguiu reabrir arquivo da raiz.");
    return false;
  }

  size_t rootSize=rootRead.size();
  String rootLine=rootRead.readStringUntil('\n');
  rootRead.close();

  if(rootSize==0 || rootLine.indexOf("TLS-METER")<0){
    Serial.println("SD: leitura de verificacao da raiz falhou.");
    return false;
  }

  if(!SD.remove(rootTest)){
    Serial.println("SD: aviso - nao conseguiu remover arquivo temporario da raiz.");
  }

  Serial.println("SD: raiz leitura/escrita OK.");

  // ----------------------------------------------------------------
  // 2) Diretorio /TLS.
  // ----------------------------------------------------------------
  if(SD.exists("/TLS")){
    File dir=SD.open("/TLS");

    if(!dir){
      Serial.println("SD: /TLS existe, mas nao pode ser aberto.");
      return false;
    }

    if(!dir.isDirectory()){
      dir.close();
      Serial.println("SD: /TLS existe, mas nao e diretorio.");
      return false;
    }

    dir.close();
  }else{
    Serial.println("SD: criando diretorio /TLS...");

    if(!SD.mkdir("/TLS")){
      Serial.println("SD: mkdir(/TLS) falhou.");
      return false;
    }
  }

  if(!SD.exists("/TLS")){
    Serial.println("SD: /TLS nao existe apos verificacao/criacao.");
    return false;
  }

  Serial.println("SD: diretorio /TLS OK.");

  // ----------------------------------------------------------------
  // 3) Teste de escrita/leitura dentro de /TLS.
  // ----------------------------------------------------------------
  const char *testPath="/TLS/SDTEST.TMP";

  if(SD.exists(testPath)){
    SD.remove(testPath);
  }

  Serial.println("SD: testando escrita em /TLS...");

  File testFile=SD.open(testPath,FILE_WRITE);
  if(!testFile){
    Serial.println("SD: nao abriu /TLS/SDTEST.TMP para escrita.");
    return false;
  }

  size_t written=testFile.println("TLS-METER STORAGE TEST");
  testFile.flush();
  testFile.close();

  if(written==0){
    Serial.println("SD: escrita em /TLS retornou 0 bytes.");
    return false;
  }

  if(!SD.exists(testPath)){
    Serial.println("SD: arquivo de teste de /TLS nao persistiu.");
    return false;
  }

  File verifyFile=SD.open(testPath,FILE_READ);
  if(!verifyFile){
    Serial.println("SD: nao conseguiu reabrir arquivo de /TLS.");
    return false;
  }

  size_t testSize=verifyFile.size();
  String testLine=verifyFile.readStringUntil('\n');
  verifyFile.close();

  if(testSize==0 || testLine.indexOf("TLS-METER")<0){
    Serial.println("SD: conteudo de verificacao em /TLS invalido.");
    return false;
  }

  if(!SD.remove(testPath)){
    Serial.println("SD: aviso - nao conseguiu remover arquivo temporario de /TLS.");
  }

  Serial.println("SD: /TLS leitura/escrita OK.");

  return true;
}

bool trySDAtFrequency(uint32_t frequency,bool &cardWasDetected){
  cardWasDetected=false;

  if(!prepareSDBus(frequency)){
    return false;
  }

  // Se chegamos aqui, SD.begin respondeu. Verificamos se ha cartao.
  uint8_t cardType=SD.cardType();

  if(cardType==CARD_NONE){
    Serial.println("SD: montagem respondeu, mas nenhum cartao foi identificado.");
    SD.end();
    return false;
  }

  cardWasDetected=true;

  if(!testSDCardAfterMount()){
    Serial.println("SD: cartao detectado, mas falhou no autoteste.");
    SD.end();
    return false;
  }

  return true;
}

bool initSD(){
  Serial.println();
  Serial.println("================================");
  Serial.println(" AUTOTESTE MICROSD - TLS METER");
  Serial.println("================================");

  sdOK=false;
  sdDetectedButFailed=false;

  // Pequena espera para estabilizacao da alimentacao/slot apos o boot.
  delay(350);

  const uint32_t frequencies[]={
    SD_FREQ_PRIMARY,
    SD_FREQ_FALLBACK,
    SD_FREQ_SAFE
  };

  bool anyCardDetected=false;

  for(uint8_t f=0;f<sizeof(frequencies)/sizeof(frequencies[0]);f++){
    for(uint8_t attempt=1;attempt<=SD_TRIES_PER_FREQ;attempt++){
      Serial.print("SD: tentativa ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(SD_TRIES_PER_FREQ);
      Serial.print(" em ");
      Serial.print(frequencies[f]/1000UL);
      Serial.println(" kHz");

      bool cardDetectedThisAttempt=false;

      if(trySDAtFrequency(frequencies[f],cardDetectedThisAttempt)){
        sdOK=true;
        sdDetectedButFailed=false;

        Serial.println("--------------------------------");
        Serial.print("microSD OK em ");
        Serial.print(frequencies[f]/1000UL);
        Serial.println(" kHz");
        Serial.println("Montagem: OK");
        Serial.println("Raiz: leitura/escrita OK");
        Serial.println("/TLS: leitura/escrita OK");
        Serial.println("--------------------------------");
        return true;
      }

      if(cardDetectedThisAttempt){
        anyCardDetected=true;
      }

      // Desmonta por completo antes de nova tentativa.
      SD.end();
      sdSpi.end();
      digitalWrite(SD_CS,HIGH);
      delay(180);
    }
  }

  sdOK=false;
  sdDetectedButFailed=anyCardDetected;

  Serial.println("--------------------------------");
  if(anyCardDetected){
    Serial.println("microSD FALHOU NO AUTOTESTE.");
    Serial.println("O cartao respondeu, mas leitura/escrita nao passou.");
  }else{
    Serial.println("microSD NAO FOI MONTADO.");
    Serial.println("Verifique contato, alimentacao, slot e formatacao FAT32.");
  }
  Serial.println("--------------------------------");

  return false;
}

uint16_t nextTestId(){ uint16_t id=prefs.getUShort("testid",0)+1; prefs.putUShort("testid",id); return id; }

void openTestLog(){
  testSavedOK=false;
  if(!sdOK) return;
  currentTestId=nextTestId();
  char csv[32],sum[32];
  snprintf(csv,sizeof(csv),"/TLS/T%04u.CSV",currentTestId);
  snprintf(sum,sizeof(sum),"/TLS/T%04u.TXT",currentTestId);
  currentCsvPath=csv; currentSummaryPath=sum;
  logFile=SD.open(currentCsvPath,FILE_WRITE);
  if(logFile){
    logFile.println("# TLS METER - DADOS DO ENSAIO");
    logFile.printf("# ID,T%04u\n",currentTestId);
    logFile.printf("# PERFIL,%s\n",profileFileName());
    logFile.printf("# L_MM,%.2f\n",activeLengthMm); logFile.printf("# D_MM,%.2f\n",probeDiameterMm);
    logFile.printf("# STAB_MIN_S,%lu\n",(unsigned long)(profileStabMinMs()/1000UL));
    logFile.printf("# HEAT_S,%lu\n",(unsigned long)(profileHeatMs()/1000UL));
    logFile.printf("# COOL_S,%lu\n",(unsigned long)(profileCoolMs()/1000UL));
    logFile.println("session_ms,phase,phase_ms,temp_C,power_W"); logFile.flush();
  }
  logFlushCounter=0;
}

void closeTestLog(){ if(logFile){ logFile.flush(); logFile.close(); } }

void logDataLine(uint32_t sessionMs,const String &phase,uint32_t phaseMs,float tempC,float powerW){
  if(!sdOK || !logFile) return;
  logFile.printf("%lu,%s,%lu,%.4f,%.5f\n",(unsigned long)sessionMs,phase.c_str(),(unsigned long)phaseMs,tempC,powerW);
  if(++logFlushCounter>=10){ logFile.flush(); logFlushCounter=0; }
}

void writeSummary(){
  if(!sdOK || currentSummaryPath.length()==0) return;
  File f=SD.open(currentSummaryPath,FILE_WRITE); if(!f) return;
  f.printf("ID=T%04u\n",currentTestId);
  f.printf("PROFILE=%s\n",profileFileName());
  f.printf("L_MM=%.2f\n",activeLengthMm); f.printf("D_MM=%.2f\n",probeDiameterMm); f.printf("T0_C=%.4f\n",t0C);
  f.printf("STAB_MIN_S=%lu\n",(unsigned long)(profileStabMinMs()/1000UL));
  f.printf("HEAT_S=%lu\n",(unsigned long)(profileHeatMs()/1000UL));
  f.printf("COOL_S=%lu\n",(unsigned long)(profileCoolMs()/1000UL));
  f.printf("CAL_RUN=%d\n",calibrationRun?1:0);
  f.printf("CAL_FACTOR_APPLIED=%.8f\n",factorForCurrentTest);
  if(calibrationRun){
    f.printf("CAL_REFERENCE_K=%.8f\n",calibrationRunReference);
    f.println("CAL_STATUS=PENDING_CONFIRMATION");
  }
  if(resultTLS.valid){
    f.printf("RHO_RAW=%.6f\n",resultTLS.rho);f.printf("K_RAW=%.6f\n",resultTLS.k);
    f.printf("RHO=%.6f\n",resultTLS.rho/factorForCurrentTest);
    f.printf("K=%.6f\n",resultTLS.k*factorForCurrentTest);
    f.printf("R2=%.6f\n",resultTLS.r2);
    f.printf("P_MEAN_W=%.6f\n",resultTLS.pMean); f.printf("Q_W_M=%.6f\n",resultTLS.qMean);
    f.printf("T_START_S=%.3f\n",resultTLS.tStart); f.printf("T_END_S=%.3f\n",resultTLS.tEnd); f.printf("LOG_CYCLES=%.3f\n",resultTLS.logCycles);
  } else f.println("RESULT=INVALID");
  f.close();
}

bool verifyCurrentTestSaved(){
  if(!sdOK || currentCsvPath.length()==0 || currentSummaryPath.length()==0) return false;

  File csv=SD.open(currentCsvPath,FILE_READ);
  if(!csv) return false;
  size_t csvSize=csv.size();
  csv.close();

  File txt=SD.open(currentSummaryPath,FILE_READ);
  if(!txt) return false;
  size_t txtSize=txt.size();
  txt.close();

  return csvSize>0 && txtSize>0;
}

// ============================================================================
// ESTABILIZACAO
// ============================================================================

void resetStab(){ stabCount=0; lastStabStoredMs=0; t0C=NAN; liveDriftCPerMin=NAN; }

void addStabSample(uint32_t nowMs,float tempC){
  if(lastStabStoredMs!=0 && nowMs-lastStabStoredMs<1000UL) return;
  lastStabStoredMs=nowMs;
  if(stabCount<STAB_BUF){ stabMs[stabCount]=nowMs; stabTemp[stabCount]=tempC; stabCount++; }
  else { for(int i=1;i<STAB_BUF;i++){ stabMs[i-1]=stabMs[i]; stabTemp[i-1]=stabTemp[i]; } stabMs[STAB_BUF-1]=nowMs; stabTemp[STAB_BUF-1]=tempC; }
}

float calcDriftCPerMin(){
  if(stabCount<6) return NAN;
  uint32_t newest=stabMs[stabCount-1]; int i0=0;
  while(i0<stabCount-2 && newest-stabMs[i0]>STAB_DRIFT_WINDOW_MS) i0++;
  float spanSec=(newest-stabMs[i0])/1000.0f; if(spanSec<20.0f) return NAN;
  int n=stabCount-i0; double sx=0,sy=0,sxx=0,sxy=0;
  for(int i=i0;i<stabCount;i++){ double x=(stabMs[i]-stabMs[i0])/1000.0; double y=stabTemp[i]; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; }
  double den=n*sxx-sx*sx; if(fabs(den)<1e-9) return NAN;
  return ((n*sxy-sx*sy)/den)*60.0;
}

float calcT0(){
  if(stabCount==0) return NAN;
  uint32_t newest=stabMs[stabCount-1]; double sum=0; int n=0;
  for(int i=stabCount-1;i>=0;i--){ if(newest-stabMs[i]>10000UL) break; sum+=stabTemp[i]; n++; }
  return n?sum/n:stabTemp[stabCount-1];
}

bool stabilizationReached(){
  if(millis()-stageStartMs<profileStabMinMs()) return false;
  if(!profileUsesDriftCriterion()) return true;
  liveDriftCPerMin=calcDriftCPerMin();
  return !isnan(liveDriftCPerMin) && fabs(liveDriftCPerMin)<=STAB_DRIFT_LIMIT_C_PER_MIN;
}

// ============================================================================
// GRAFICO
// ============================================================================

void resetGraph(){ graphCount=0; lastGraphStoredMs=0; }

void addGraphSample(uint32_t ms,float t){
  if(lastGraphStoredMs!=0 && ms-lastGraphStoredMs<1000UL) return;
  lastGraphStoredMs=ms;
  if(graphCount<GRAPH_N){ graphMs[graphCount]=ms; graphTemp[graphCount]=t; graphCount++; }
  else { for(int i=1;i<GRAPH_N;i++){ graphMs[i-1]=graphMs[i]; graphTemp[i-1]=graphTemp[i]; } graphMs[GRAPH_N-1]=ms; graphTemp[GRAPH_N-1]=t; }
}

void drawMiniGraph(int x,int y,int w,int h){
  tft.fillRect(x,y,w,h,C_PANEL); tft.drawRect(x,y,w,h,C_BORDER); if(graphCount<2) return;
  float tMin=graphTemp[0],tMax=graphTemp[0];
  for(int i=1;i<graphCount;i++){ tMin=min(tMin,graphTemp[i]); tMax=max(tMax,graphTemp[i]); }
  if(fabs(tMax-tMin)<0.2f){ tMin-=0.1f; tMax+=0.1f; }
  uint32_t xMin=graphMs[0],xMax=graphMs[graphCount-1]; if(xMax<=xMin)xMax=xMin+1;
  for(int i=1;i<graphCount;i++){
    int px0=x+3+(int)((graphMs[i-1]-xMin)*(w-6)/(float)(xMax-xMin));
    int px1=x+3+(int)((graphMs[i]-xMin)*(w-6)/(float)(xMax-xMin));
    int py0=y+h-3-(int)((graphTemp[i-1]-tMin)*(h-6)/(tMax-tMin));
    int py1=y+h-3-(int)((graphTemp[i]-tMin)*(h-6)/(tMax-tMin));
    tft.drawLine(px0,py0,px1,py1,C_CYAN);
  }
  if(!isnan(t0C) && t0C>=tMin && t0C<=tMax){ int py=y+h-3-(int)((t0C-tMin)*(h-6)/(tMax-tMin)); for(int xx=x+2;xx<x+w-2;xx+=5)tft.drawFastHLine(xx,py,3,C_MUTED); }
}

// ============================================================================
// TELA AO VIVO / RESULTADOS / AJUSTES
// ============================================================================

const char *stageName(){
  switch(testStage){
    case ST_STABILIZING:return "ESTABILIZANDO";
    case ST_HEATING:return "AQUECIMENTO";
    case ST_COOLING:return "RESFRIAMENTO";
    case ST_PROCESSING:return "PROCESSANDO";
    case ST_DONE:return "CONCLUIDO";
    default:return "AGUARDANDO";
  }
}

void drawStageBar(){
  const char *labels[4]={"1 STAB","2 AQUEC","3 RESFR","4 RESULT"};
  int active=0; if(testStage==ST_HEATING)active=1; else if(testStage==ST_COOLING)active=2; else if(testStage==ST_PROCESSING||testStage==ST_DONE)active=3;
  for(int i=0;i<4;i++){
    int x=4+i*79; uint16_t col=(i<active)?C_GREEN:(i==active?C_BLUE:C_BORDER);
    tft.fillRoundRect(x,38,75,18,5,(i<=active)?C_PANEL2:C_PANEL); tft.drawRoundRect(x,38,75,18,5,col);
    centerText(labels[i],x,44,75,1,(i<=active)?C_TEXT:C_MUTED);
  }
}

void drawLive(bool fullRedraw){
  if(fullRedraw){
    screenState=SCR_LIVE; tft.fillScreen(C_BG); drawHeader("ENSAIO"); drawStageBar();
    panel(7,63,148,52,C_RED,C_PANEL); drawBitmapIcon(TLS_ICON_THERMOMETER,12,73,C_RED); tft.setTextColor(C_MUTED); tft.setTextSize(1); tft.setCursor(45,70); tft.print("TEMPERATURA");
    panel(165,63,148,52,C_YELLOW,C_PANEL); drawBitmapIcon(TLS_ICON_BOLT,170,73,C_YELLOW); tft.setTextColor(C_MUTED); tft.setCursor(202,70); printPTColor("POTÊNCIA",C_MUTED);
    panel(7,121,306,70,C_CYAN,C_PANEL); drawBitmapIcon(TLS_ICON_CHART,10,122,C_CYAN);
    button(7,198,306,34,"PARAR ENSAIO",C_RED,true);
  }
  tft.fillRect(45,84,100,25,C_PANEL); tft.setTextColor(C_TEXT); tft.setTextSize(2); tft.setCursor(45,87);
  if(isnan(liveTempC))tft.print("--.- C"); else { tft.print(liveTempC,2); tft.print(" C"); }
  tft.fillRect(202,84,102,25,C_PANEL); tft.setCursor(202,87); tft.print(livePowerW,2); tft.print(" W");
  tft.fillRect(45,124,255,14,C_PANEL); tft.setTextSize(1); tft.setTextColor(C_MUTED); tft.setCursor(45,127); tft.print(stageName());
  if(testStage==ST_STABILIZING && profileUsesDriftCriterion()){ tft.setCursor(170,127); tft.print("dT/dt:"); if(!isnan(liveDriftCPerMin)){tft.print(liveDriftCPerMin,3);tft.print(" C/min");} else tft.print("--"); }
  else if(testStage==ST_HEATING){ tft.setCursor(190,127); tft.print("q="); tft.print(liveQ,1); tft.print(" W/m"); }
  drawMiniGraph(15,142,290,42);
}

void resultCard(int x,int y,int w,int h,const char *label,float value,const char *unit,uint16_t col,int decimals){
  panel(x,y,w,h,col,C_PANEL); useCompactFont(); tft.setTextColor(C_MUTED); tft.setCursor(x+8,y+12); printPTColor(label,C_MUTED);
  useDefaultFont(); tft.setTextColor(C_TEXT); tft.setTextSize(2); tft.setCursor(x+8,y+24); tft.print(value,decimals);
  tft.setTextSize(1); tft.setTextColor(C_MUTED); tft.setCursor(x+8,y+h-13); tft.print(unit);
}

void drawResults(){
  screenState=SCR_RESULTS; tft.fillScreen(C_BG); drawHeader("RESULTADO");
  drawBitmapIcon(TLS_ICON_CHECK,9,38,resultTLS.valid?C_GREEN:C_RED); tft.setTextColor(resultTLS.valid?C_GREEN:C_RED); useSmallBoldFont(); tft.setCursor(47,59); printPTColor(resultTLS.valid?"Ensaio concluído":"Resultado não válido",resultTLS.valid?C_GREEN:C_RED); useDefaultFont();
  if(resultTLS.valid){
    resultCard(7,72,148,62,"RESISTIVIDADE",resultTLS.rho/factorForCurrentTest,"K.m/W",C_ORANGE,4);
    resultCard(165,72,148,62,"CONDUTIVIDADE",resultTLS.k*factorForCurrentTest,"W/(m.K)",C_GREEN,4);
    tft.setTextSize(1); tft.setTextColor(C_CYAN); tft.setCursor(224,60);
    tft.print("F="); tft.print(factorForCurrentTest,3);
    resultCard(7,141,148,62,"AJUSTE LINEAR",resultTLS.r2,"R2",C_CYAN,5);
    resultCard(165,141,148,62,"POTÊNCIA LINEAR",resultTLS.qMean,"W/m",C_YELLOW,1);
  } else {
    panel(20,80,280,95,C_RED,C_PANEL); tft.setTextColor(C_RED); useSmallBoldFont(); tft.setCursor(55,118); tft.print("Sem janela TLS"); useDefaultFont();
    tft.setTextSize(1); tft.setTextColor(C_MUTED); tft.setCursor(45,140); printPTColor("A região linear não foi identificada.",C_MUTED);
  }
  button(7,207,148,28,"HOME",C_BLUE);
  const char *saveLabel = testSavedOK ? "SALVO NO SD" : (sdOK ? "ERRO AO SALVAR" : "SEM SD");
  button(165,207,148,28,saveLabel,testSavedOK?C_GREEN:C_RED,testSavedOK);
}


void applyScreenBrightness(){
  screenBrightnessPercent=constrain(screenBrightnessPercent,10,100);
  uint8_t duty=(uint8_t)map(screenBrightnessPercent,0,100,0,255);

  // analogWrite no core ESP32 usa PWM/LEDC e evita dependermos da
  // assinatura diferente de ledcAttach entre as versoes 2.x e 3.x.
  analogWrite(TFT_BL,duty);
}

void saveDisplaySettings(){
  prefs.putUChar("bright",screenBrightnessPercent);
}

void drawSettingsMenu(){
  screenState=SCR_SETTINGS_MENU;
  sdWarningActive=false;
  tft.fillScreen(C_BG);
  drawHeader("AJUSTES");

  tile(8,42,148,58,"TELA",C_CYAN,TLS_ICON_SCREEN);
  tile(164,42,148,58,"ENSAIO",C_GREEN,TLS_ICON_THERMOMETER);
  tile(8,106,148,58,"PC",pcMode?C_ORANGE:C_BLUE,TLS_ICON_PC);
  tile(164,106,148,58,"SISTEMA",C_YELLOW,TLS_ICON_SETTINGS);

  useCompactFont();
  tft.setTextColor(C_MUTED);
  tft.setCursor(10,188);
  if(pcMode) printPTColor("Modo PC ativo: medidor liberado.",C_ORANGE);
  else printPTColor("Configurações da IHM e do ensaio.",C_MUTED);
  useDefaultFont();

  button(8,207,304,28,"VOLTAR",C_BLUE);
}

void drawDisplaySettings(){
  screenState=SCR_DISPLAY_SETTINGS;
  tft.fillScreen(C_BG);
  drawHeader("TELA");

  panel(8,45,304,70,C_CYAN,C_PANEL);
  drawBitmapIcon(TLS_ICON_SCREEN,20,64,C_CYAN);

  tft.setTextColor(C_MUTED);
  useCompactFont();
  tft.setCursor(66,61);
  tft.print("BRILHO ATUAL");
  useDefaultFont();

  tft.setTextColor(C_TEXT);
  tft.setTextSize(3);
  tft.setCursor(112,77);
  tft.print(screenBrightnessPercent);
  tft.print("%");

  const uint8_t values[5]={10,25,50,75,100};
  for(int i=0;i<5;i++){
    int bx=10+i*61;
    char label[8];
    snprintf(label,sizeof(label),"%u%%",values[i]);
    buttonCompact(bx,125,56,30,label,
                  screenBrightnessPercent==values[i]?C_GREEN:C_CYAN);
  }

  panel(10,164,300,26,C_BORDER,C_PANEL);
  int fillW=(int)((296L*screenBrightnessPercent)/100L);
  if(fillW<2) fillW=2;
  tft.fillRoundRect(12,171,fillW,12,5,C_YELLOW);

  useCompactFont();
  tft.setTextColor(C_MUTED);
  drawCenteredFontText("O valor é salvo automaticamente.",8,190,304,15,&Org_01,C_MUTED);
  useDefaultFont();

  button(8,207,304,28,"VOLTAR",C_BLUE);
}

void invalidateBatteryTelemetry(){
  batteryPacketSeen=false;
  batteryValid=false;
  batteryCharging=false;
  batteryPercent=-1;
  batteryMv=0;
  batteryPopupActive=false;
}

void enterPCMode(){
  // Este menu so e acessivel fora de um ensaio ativo. Ainda assim,
  // mantemos a protecao para evitar liberar o link em uma condicao invalida.
  if(testActive){
    setError("PC_MODE_BLOCKED",
             "MODO PC INDISPONÍVEL",
             "Finalize o ensaio atual",
             "antes de liberar o Bluetooth.");
    drawError();
    return;
  }

  pcMode=true;
  pcReconnectFailed=false;
  homeWarningActive=false;

  // Libera a unica conexao SPP do ESP32 de medicao.
  // O Bluetooth do DISPLAY continua iniciado, porem sem cliente remoto.
  if(SerialBT.connected()){
    SerialBT.disconnect();
    delay(180);
  }

  btOK=false;
  meterReady=false;
  btLine="";
  invalidateBatteryTelemetry();

  drawPCSettings();
}

void attemptReturnToIHM(){
  pcReconnectFailed=false;

  // A partir daqui o display volta a poder assumir o TLS-METER.
  pcMode=false;

  tft.fillRoundRect(22,72,276,96,10,C_PANEL2);
  tft.drawRoundRect(22,72,276,96,10,C_CYAN);
  drawBitmapIcon(TLS_ICON_BLUETOOTH,38,103,C_CYAN);
  drawCenteredFontText("Reconectando à IHM...",76,83,205,32,&FreeSansBold9pt7b,C_CYAN);
  drawCenteredFontText("Aguarde um momento.",76,120,205,28,&FreeSans9pt7b,C_TEXT);
  useDefaultFont();

  bool ok=connectMeter(false);

  if(ok){
    pcMode=false;
    pcReconnectFailed=false;

    // Mantem bateria desconhecida ate chegar o NOVO BAT solicitado em connectMeter().
    invalidateBatteryTelemetry();

    drawHome();
    drawConnectionPopup("IHM conectada","TLS-METER pronto",C_GREEN);
    delay(650);
    drawHome();
  }else{
    // Se o Windows ainda estiver usando a unica conexao SPP, a IHM nao
    // conseguira entrar. Continuamos em modo PC e deixamos o usuario tentar.
    pcMode=true;
    pcReconnectFailed=true;
    btOK=false;
    meterReady=false;
    invalidateBatteryTelemetry();
    drawPCSettings();
  }
}

void drawPCSettings(){
  screenState=SCR_PC_SETTINGS;
  tft.fillScreen(C_BG);
  drawHeader("PC");

  if(pcReconnectFailed){
    panel(14,50,292,116,C_RED,C_PANEL);
    drawBitmapIcon(TLS_ICON_PC,27,86,C_RED);

    drawCenteredFontText("MEDIDOR OCUPADO",70,57,220,27,&FreeSansBold9pt7b,C_RED);
    drawWrappedPT(
      "Não foi possível reconectar. Encerre a conexão do TLS Meter no computador e tente novamente.",
      72,91,216,19,4,&FreeSans9pt7b,C_TEXT
    );

    button(8,174,304,28,"TENTAR NOVAMENTE",C_CYAN);
  }else{
    panel(14,50,292,116,C_GREEN,C_PANEL);
    drawBitmapIcon(TLS_ICON_PC,27,84,C_GREEN);

    drawCenteredFontText("MODO PC",70,57,220,27,&FreeSansBold9pt7b,C_GREEN);
    drawWrappedPT(
      "Medidor liberado para conexao com o computador. Abra o TLS Meter no Windows.",
      72,91,216,19,4,&FreeSans9pt7b,C_TEXT
    );

    useCompactFont();
    tft.setTextColor(C_MUTED);
    tft.setCursor(73,158);
    printPTColor("IHM desconectada - PC disponível",C_MUTED);
    useDefaultFont();

    button(8,174,304,28,"VOLTAR À IHM",C_CYAN);
  }

  button(8,207,304,28,"VOLTAR A AJUSTES",C_BLUE);
}

void drawSystemSettings(){
  screenState=SCR_SYSTEM_SETTINGS;
  tft.fillScreen(C_BG);
  drawHeader("SISTEMA");

  panel(8,43,304,125,C_YELLOW,C_PANEL);

  useCompactFont();
  int y=58;

  auto statusRow=[&](const char *label,const String &value,uint16_t color){
    tft.setTextColor(C_MUTED);
    tft.setCursor(18,y);
    printPTColor(label,C_MUTED);

    tft.setTextColor(color);
    tft.setCursor(145,y);
    printPTColor(value.c_str(),color);
    y+=21;
  };

  statusRow("Firmware",String("Display V")+DISPLAY_FW_VERSION,C_CYAN);

  String mode=pcMode?"MODO PC":"IHM";
  statusRow("Modo Bluetooth",mode,pcMode?C_ORANGE:C_GREEN);

  String btState=btOK?"CONECTADO":"DESCONECTADO";
  statusRow("Link TLS-METER",btState,btOK?C_GREEN:C_RED);

  String sdState=sdOK?"OK":(sdDetectedButFailed?"FALHA":"NÃO DETECTADO");
  statusRow("microSD",sdState,sdOK?C_GREEN:C_RED);

  String batState="--%";
  if(batteryPacketSeen && batteryValid && batteryPercent>=0){
    batState=String(batteryPercent)+"%";
  }
  statusRow("Bateria medidor",batState,
            (batteryPacketSeen && batteryValid)?C_GREEN:C_MUTED);

  useDefaultFont();

  buttonCompact(8,176,148,27,"TESTAR SD",C_CYAN);
  buttonCompact(164,176,148,27,"REINICIAR",C_ORANGE);
  button(8,207,304,28,"VOLTAR",C_BLUE);
}

void saveSettings(){
  prefs.putUChar("profile",(uint8_t)currentProfile);
  prefs.putFloat("Lmm",activeLengthMm);
  prefs.putFloat("Dmm",probeDiameterMm);
  prefs.putUChar("labStab",labStabMin);
  prefs.putUChar("labHeat",labHeatMin);
  prefs.putUChar("fldStab",fieldStabMin);
  prefs.putUChar("fldHeat",fieldHeatMin);
}

void drawSettings(){
  screenState=SCR_SETTINGS;
  sdWarningActive=false;
  tft.fillScreen(C_BG);
  drawHeader("ENSAIO");

  // Perfil
  panel(8,45,304,50,C_BLUE,C_PANEL);
  tft.setTextColor(C_MUTED);
  tft.setTextSize(1);
  tft.setCursor(18,53);
  tft.print("PERFIL");

  tft.setTextColor(C_TEXT);
  tft.setTextSize(2);
  tft.setCursor(18,70);
  printPTColor(profileName(),C_TEXT);

  // Rapido permanece simples. Laboratorio/Campo ganham acesso ao submenu.
  if(currentProfile==PROFILE_DEV){
    buttonCompact(232,56,70,28,"TROCAR",C_CYAN);
  } else {
    buttonCompact(164,56,66,28,"TROCAR",C_CYAN);
    buttonCompact(236,56,66,28,"OPÇÕES",C_GREEN);
  }

  // Comprimento ativo
  panel(8,102,304,48,C_GREEN,C_PANEL);
  drawCenteredFontText("COMPRIMENTO ATIVO L",8,105,304,18,&Org_01,C_MUTED);
  tft.setTextColor(C_TEXT);
  tft.setTextSize(2);
  tft.setCursor(126,130);
  tft.print(activeLengthMm,0);
  tft.print(" mm");
  button(18,116,40,26,"-",C_CYAN);
  button(254,116,40,26,"+",C_CYAN);

  // Diametro da sonda
  panel(8,157,304,48,C_YELLOW,C_PANEL);
  drawCenteredFontText("DIAMETRO DA SONDA",8,160,304,18,&Org_01,C_MUTED);
  tft.setTextColor(C_TEXT);
  tft.setTextSize(2);
  tft.setCursor(126,185);
  tft.print(probeDiameterMm,1);
  tft.print(" mm");
  button(18,171,40,26,"-",C_CYAN);
  button(254,171,40,26,"+",C_CYAN);

  button(8,211,148,25,"CALIBRAÇÃO",C_GREEN);
  button(164,211,148,25,"VOLTAR",C_BLUE);
}

void saveCalibration(){
  prefs.putFloat("cal_ref_d",calibrationReferenceDraft);
  prefs.putFloat("cal_factor",calibrationFactor);
  prefs.putFloat("cal_ref_u",calibrationReferenceUsed);
  prefs.putFloat("cal_k_raw",calibrationLastRawK);
  prefs.putBool("cal_valid",calibrationValid);
  prefs.putBool("cal_active",calibrationActive);
}

void drawCalibrationSettings(){
  screenState=SCR_CAL_SETTINGS;
  tft.fillScreen(C_BG); drawHeader("CALIBRAÇÃO");
  panel(8,43,304,52,C_GREEN,C_PANEL);
  useCompactFont(); tft.setTextColor(C_MUTED); tft.setCursor(18,56);
  printPTColor("AMOSTRA PADRÃO - k ref. W/(m.K)",C_MUTED); useDefaultFont();
  tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.setCursor(104,67);
  if(calibrationReferenceDraft>0) tft.print(calibrationReferenceDraft,4);
  else tft.print("--.----");
  button(8,99,72,29,"-",C_CYAN);
  button(85,99,72,29,calibrationStepNames[calibrationStepIndex],C_YELLOW);
  button(162,99,72,29,"+",C_CYAN);
  button(239,99,73,29,"ZERAR",C_RED);
  panel(8,133,304,38,calibrationValid?C_GREEN:C_BORDER,C_PANEL);
  tft.setTextColor(calibrationActive?C_GREEN:C_MUTED);tft.setTextSize(1);tft.setCursor(17,146);
  tft.print(calibrationActive?"FATOR ATIVO":"FATOR INATIVO");
  tft.setTextSize(2);tft.setCursor(162,143);
  if(calibrationValid) tft.print(calibrationFactor,4); else tft.print("1.0000");
  button(8,175,148,28,"ENS. PADRÃO",C_GREEN,!testActive && !pcMode && calibrationReferenceDraft>=0.01f && btOK && currentProfile==PROFILE_LAB);
  button(164,175,148,28,calibrationActive?"DESATIVAR":"ATIVAR",C_YELLOW,calibrationValid);
  button(8,207,148,28,"RESTAURAR",C_ORANGE,calibrationValid);
  button(164,207,148,28,"VOLTAR",C_BLUE);
}

void drawCalibrationReview(){
  screenState=SCR_CAL_REVIEW;
  tft.fillScreen(C_BG);drawHeader("CALIBRAÇÃO");
  panel(8,44,304,148,C_GREEN,C_PANEL);
  useCompactFont();tft.setTextColor(C_TEXT);tft.setCursor(18,59);
  printPTColor("Conferir resultado da Amostra Padrão",C_TEXT);useDefaultFont();
  tft.setTextColor(C_MUTED);tft.setTextSize(1);
  tft.setCursor(18,82);tft.print("k ref. (W/(m.K)):");
  tft.setCursor(18,105);tft.print("k bruto (W/(m.K)):");
  tft.setCursor(18,128);tft.print("F = k ref. / k bruto:");
  tft.setCursor(18,151);tft.print("R2:");
  const float proposed = (resultTLS.valid && resultTLS.k>0) ? calibrationRunReference/resultTLS.k : NAN;
  tft.setTextColor(C_CYAN);tft.setTextSize(1);
  tft.setCursor(242,82);tft.print(calibrationRunReference,4);
  tft.setCursor(242,105);tft.print(resultTLS.k,4);
  tft.setCursor(242,128); if(isfinite(proposed)) tft.print(proposed,4); else tft.print("inválido");
  tft.setCursor(242,151); tft.print(resultTLS.r2,4);
  tft.setTextColor(C_MUTED); tft.setCursor(18,177); tft.print("Verifique a qualidade antes de salvar.");
  const bool valid = isfinite(proposed) && proposed>=0.1f && proposed<=10.0f && resultTLS.r2>=0 && resultTLS.r2<=1;
  button(8,207,148,28,"CONFIRMAR",C_GREEN,valid);
  button(164,207,148,28,"DESCARTAR",C_RED);
}

void drawCalibrationReset(){
  screenState=SCR_CAL_RESET;tft.fillScreen(C_BG);drawHeader("RESTAURAR");
  panel(8,55,304,122,C_RED,C_PANEL);
  drawCenteredFontText("Apagar fator salvo?",18,72,284,28,&FreeSansBold9pt7b,C_TEXT);
  drawCenteredFontText("Resultados antigos não mudam.",18,116,284,22,&FreeSans9pt7b,C_MUTED);
  button(8,207,148,28,"APAGAR",C_RED);
  button(164,207,148,28,"CANCELAR",C_BLUE);
}

void handleCalibrationSettingsTap(int16_t x,int16_t y){
  if(inRect(x,y,85,99,72,29)){
    // Tocar o valor do passo alterna 0,1 / 0,01 / 0,001 / 0,0001.
    calibrationStepIndex=(calibrationStepIndex+1)%4;
    drawCalibrationSettings();return;
  }
  if(inRect(x,y,239,99,73,29)){
    calibrationReferenceDraft=0; prefs.putFloat("cal_ref_d",0.0f);
    drawCalibrationSettings();return;
  }
  if(inRect(x,y,8,99,72,29)||inRect(x,y,162,99,72,29)){
    const float sign=inRect(x,y,8,99,72,29)?-1.0f:1.0f;
    const float next=calibrationReferenceDraft+sign*calibrationSteps[calibrationStepIndex];
    calibrationReferenceDraft=constrain(roundf(next*10000.0f)/10000.0f,0.0f,10.0f);
    prefs.putFloat("cal_ref_d",calibrationReferenceDraft);
    drawCalibrationSettings();return;
  }
  if(inRect(x,y,8,175,148,28)){
    // Calibracao sempre com perfil Laboratorio, nunca pelo ciclo Rapido.
    if(testActive||pcMode||!btOK||calibrationReferenceDraft<0.01f||currentProfile!=PROFILE_LAB) return;
    calibrationRun=true;
    calibrationRunReference=calibrationReferenceDraft;
    startNewTest();return;
  }
  if(inRect(x,y,164,175,148,28)){
    if(!calibrationValid) return;
    calibrationActive=!calibrationActive;saveCalibration();drawCalibrationSettings();return;
  }
  if(inRect(x,y,8,207,148,28)){if(calibrationValid)drawCalibrationReset();return;}
  if(inRect(x,y,164,207,148,28)){drawSettings();return;}
}

void handleCalibrationReviewTap(int16_t x,int16_t y){
  if(inRect(x,y,8,207,148,28)){
    const float proposed=calibrationRunReference/resultTLS.k;
    if(!resultTLS.valid||!isfinite(proposed)||proposed<0.1f||proposed>10.0f)return;
    calibrationFactor=proposed;
    calibrationReferenceUsed=calibrationRunReference;
    calibrationLastRawK=resultTLS.k;
    calibrationValid=true;calibrationActive=true;saveCalibration();
    if(sdOK && currentSummaryPath.length()){
      File f=SD.open(currentSummaryPath,FILE_APPEND);
      if(f){f.printf("CAL_STATUS=ACCEPTED\nCAL_FACTOR_NEW=%.8f\n",calibrationFactor);f.close();}
    }
    calibrationRun=false;drawCalibrationSettings();return;
  }
  if(inRect(x,y,164,207,148,28)){
    if(sdOK && currentSummaryPath.length()){
      File f=SD.open(currentSummaryPath,FILE_APPEND);
      if(f){f.println("CAL_STATUS=DISCARDED");f.close();}
    }
    calibrationRun=false;drawCalibrationSettings();return;
  }
}

void handleCalibrationResetTap(int16_t x,int16_t y){
  if(inRect(x,y,8,207,148,28)){
    calibrationFactor=1;calibrationReferenceUsed=0;calibrationLastRawK=0;
    calibrationValid=false;calibrationActive=false;saveCalibration();drawCalibrationSettings();return;
  }
  if(inRect(x,y,164,207,148,28)){drawCalibrationSettings();return;}
}

void drawProfileOptions(){
  if(currentProfile==PROFILE_DEV){
    drawSettings();
    return;
  }

  screenState=SCR_PROFILE_OPTIONS;
  tft.fillScreen(C_BG);
  drawHeader("OPÇÕES");

  tft.setTextColor(C_CYAN);
  useSmallBoldFont();
  drawCenteredFontText(profileName(),0,35,SCREEN_W,22,&FreeSansBold9pt7b,C_CYAN);
  useDefaultFont();

  uint8_t stabMin=(currentProfile==PROFILE_LAB)?labStabMin:fieldStabMin;
  uint8_t heatMin=(currentProfile==PROFILE_LAB)?labHeatMin:fieldHeatMin;

  // Estabilizacao minima
  panel(8,58,304,43,C_BLUE,C_PANEL);
  drawCenteredFontText("ESTABILIZAÇÃO MÍNIMA",8,59,304,15,&Org_01,C_MUTED);
  buttonCompact(18,72,42,23,"-",C_CYAN);
  buttonCompact(260,72,42,23,"+",C_CYAN);
  char value[20];
  snprintf(value,sizeof(value),"%u min",stabMin);
  drawCenteredFontText(value,70,69,180,28,&FreeSans9pt7b,C_TEXT);

  // Aquecimento
  panel(8,106,304,43,C_ORANGE,C_PANEL);
  drawCenteredFontText("AQUECIMENTO",8,107,304,15,&Org_01,C_MUTED);
  buttonCompact(18,120,42,23,"-",C_CYAN);
  buttonCompact(260,120,42,23,"+",C_CYAN);
  snprintf(value,sizeof(value),"%u min",heatMin);
  drawCenteredFontText(value,70,117,180,28,&FreeSans9pt7b,C_TEXT);

  // Parametros fixos / internos
  panel(8,154,148,47,C_GREEN,C_PANEL);
  drawCenteredFontText("RESFRIAMENTO",8,157,148,14,&Org_01,C_MUTED);
  drawCenteredFontText("2 min",8,173,148,23,&FreeSans9pt7b,C_TEXT);

  panel(164,154,148,47,C_GREEN,C_PANEL);
  drawCenteredFontText("EQUILÍBRIO TÉRMICO",164,157,148,14,&Org_01,C_MUTED);
  drawCenteredFontText("AUTOMÁTICO",164,176,148,17,&Org_01,C_TEXT);

  buttonCompact(8,207,148,28,"RESTAURAR PADRÃO",C_ORANGE);
  buttonCompact(164,207,148,28,"VOLTAR",C_BLUE);
}


// ============================================================================
// CREDITOS
// ============================================================================

void drawCredits(){
  screenState=SCR_CREDITS;
  homeWarningActive=false;
  tft.fillScreen(C_BG);
  drawHeader("CRÉDITOS");

  panel(8,44,304,158,C_BLUE,C_PANEL);
  drawBitmapIcon(TLS_ICON_CREDITS,18,57,C_CYAN);

  tft.setTextColor(C_TEXT);
  useSmallBoldFont();
  tft.setCursor(60,69);
  printPTColor("TLS Meter",C_TEXT);

  // Apenas os quatro créditos solicitados.
  // Fonte um pouco maior que a v1.2, mantendo leitura confortável.
  tft.setTextColor(C_MUTED);
  useCreditsFont();

  tft.setCursor(22,106);
  printPTColor("UFCG",C_MUTED);

  tft.setCursor(22,131);
  printPTColor("Laboratório de Alta Tensão - LAT",C_MUTED);

  tft.setCursor(22,156);
  printPTColor("Francisco Olimpio Ferreira da Silva",C_MUTED);

  tft.setCursor(22,181);
  printPTColor("Prof. Ronimack Trajano de Souza",C_MUTED);

  useDefaultFont();
  button(8,207,304,28,"VOLTAR",C_BLUE);
}

// ============================================================================
// HISTORICO
// ============================================================================

bool endsWithIgnoreCase(String s,String ending){ s.toUpperCase(); ending.toUpperCase(); return s.endsWith(ending); }

void scanHistory(){
  historyCount=0; if(!sdOK)return; File dir=SD.open("/TLS"); if(!dir||!dir.isDirectory())return;
  File f=dir.openNextFile();
  while(f && historyCount<MAX_HISTORY){ String name=f.name(); if(!name.startsWith("/")) name=String("/TLS/")+name; if(!f.isDirectory()&&endsWithIgnoreCase(name,".TXT")) historyFiles[historyCount++]=name; f.close(); f=dir.openNextFile(); }
  dir.close();
  for(int i=0;i<historyCount;i++)for(int j=i+1;j<historyCount;j++)if(historyFiles[j]>historyFiles[i]){String t=historyFiles[i];historyFiles[i]=historyFiles[j];historyFiles[j]=t;}
}

String shortHistoryName(const String &path){ int s=path.lastIndexOf('/'); String n=(s>=0)?path.substring(s+1):path; n.replace(".TXT",""); n.replace(".txt",""); return n; }

void drawHistory(){
  screenState=SCR_HISTORY; scanHistory(); tft.fillScreen(C_BG); drawHeader("HISTÓRICO");
  if(!sdOK){ panel(25,80,270,80,C_RED,C_PANEL); drawBitmapIcon(TLS_ICON_STORAGE,42,104,C_RED); tft.setTextColor(C_RED); tft.setTextSize(2); tft.setCursor(90,100); tft.print("microSD"); tft.setTextSize(1); tft.setTextColor(C_MUTED); tft.setCursor(90,127); printPTColor("Cartao nao detectado.",C_MUTED); }
  else if(historyCount==0){ panel(25,80,270,80,C_CYAN,C_PANEL); drawBitmapIcon(TLS_ICON_HISTORY,42,104,C_CYAN); tft.setTextColor(C_MUTED); tft.setTextSize(1); tft.setCursor(90,115); tft.print("Nenhum ensaio salvo."); }
  else {
    int shown=min(5,historyCount-historyOffset);
    for(int i=0;i<shown;i++){ int idx=historyOffset+i; int y=42+i*33; panel(8,y,304,29,C_BORDER,C_PANEL); drawBitmapIcon(TLS_ICON_HISTORY,10,y-1,C_CYAN); tft.setTextColor(C_TEXT); tft.setTextSize(1); tft.setCursor(45,y+7); tft.print(shortHistoryName(historyFiles[idx])); tft.setTextColor(C_MUTED); tft.setCursor(235,y+7); tft.print("abrir >"); }
  }
  button(8,211,95,25,"VOLTAR",C_BLUE); button(112,211,95,25,"<",C_CYAN,historyOffset>0); button(217,211,95,25,">",C_CYAN,historyOffset+5<historyCount);
}

String readValue(const String &path,const String &key){
  File f=SD.open(path); if(!f)return ""; String result="";
  while(f.available()){ String l=f.readStringUntil('\n'); l.trim(); if(l.startsWith(key+"=")){result=l.substring(key.length()+1);break;} }
  f.close(); return result;
}


String profileDisplayName(const String &stored){
  String p=stored;
  p.toUpperCase();
  if(p=="LABORATORIO") return "Laboratório";
  if(p=="CAMPO") return "Campo";
  return "Rápido";
}

void drawHistoryDetail(const String &path){
  selectedHistoryFile=path; screenState=SCR_HISTORY_DETAIL; tft.fillScreen(C_BG); drawHeader("ENSAIO SALVO"); panel(8,43,304,155,C_CYAN,C_PANEL);
  String id=readValue(path,"ID"), profile=profileDisplayName(readValue(path,"PROFILE")), rho=readValue(path,"RHO"), k=readValue(path,"K"), r2=readValue(path,"R2"), q=readValue(path,"Q_W_M"), t0=readValue(path,"T0_C");
  tft.setTextSize(2); tft.setTextColor(C_CYAN); tft.setCursor(18,52); tft.print(id.length()?id:shortHistoryName(path));
  useCompactFont(); tft.setTextColor(C_MUTED); tft.setCursor(18,78); printPTColor("Perfil de Ensaio: ",C_MUTED); tft.setTextColor(C_TEXT); printPTColor(profile.c_str(),C_TEXT); useDefaultFont();
  tft.setTextColor(C_MUTED); tft.setCursor(18,98); tft.print("rho: "); tft.setTextColor(C_TEXT); tft.print(rho); tft.print(" K.m/W");
  tft.setTextColor(C_MUTED); tft.setCursor(18,118); tft.print("k: "); tft.setTextColor(C_TEXT); tft.print(k); tft.print(" W/(m.K)");
  tft.setTextColor(C_MUTED); tft.setCursor(18,138); tft.print("R2: "); tft.setTextColor(C_TEXT); tft.print(r2);
  tft.setTextColor(C_MUTED); tft.setCursor(18,158); tft.print("q: "); tft.setTextColor(C_TEXT); tft.print(q); tft.print(" W/m");
  tft.setTextColor(C_MUTED); tft.setCursor(18,178); tft.print("T0: "); tft.setTextColor(C_TEXT); tft.print(t0); tft.print(" C");
  button(8,207,304,28,"VOLTAR AO HISTÓRICO",C_BLUE);
}

// ============================================================================
// ERROS
// ============================================================================

void setError(const String &code,const String &title,const String &l1,const String &l2){ lastErrorCode=code; lastErrorTitle=title; lastErrorLine1=l1; lastErrorLine2=l2; }

void decodeStatusError(String code){
  code.trim();

  if(code=="SELFTEST_FAIL_TEMPERATURE_SENSOR")
    setError(code,"FALHA NO AUTOTESTE",
             "Sensor de temperatura",
             "não detectado. Verifique a sonda.");

  else if(code=="SELFTEST_FAIL_TEMP_RANGE")
    setError(code,"FALHA NO AUTOTESTE",
             "Temperatura fora",
             "da faixa permitida (0 a 100 C).");

  else if(code=="SELFTEST_FAIL_INA226")
    setError(code,"FALHA NO AUTOTESTE",
             "Monitor de potência",
             "INA226 não respondeu.");

  else if(code=="SELFTEST_FAIL_HEATER_CURRENT")
    setError(code,"FALHA NO AUTOTESTE",
             "Sem corrente no aquecedor.",
             "Verifique NiCr, cabo e conector.");

  else if(code=="START_BLOCKED_LOW_BATTERY")
    setError(code,"BATERIA MUITO BAIXA",
             "Bateria abaixo de 10%.",
             "Recarregue antes de iniciar novo ensaio.");

  else if(code=="ABORT_BT_DISCONNECT")
    setError(code,"ENSAIO INTERROMPIDO",
             "Conexão Bluetooth perdida.",
             "Ensaio interrompido. Tente novamente.");

  else
    setError(code,"STATUS DO MEDIDOR",code,"");
}

void drawError(){
  screenState=SCR_ERROR;
  tft.fillScreen(C_BG);
  drawHeader("ALERTA");
  panel(18,60,284,120,C_RED,C_PANEL);

  // Título centralizado e com acentuação correta.
  drawCenteredFontText(lastErrorTitle.c_str(),28,67,264,28,&FreeSansBold9pt7b,C_RED);

  drawBitmapIcon(TLS_ICON_WARNING,34,109,C_RED);

  // Mensagem maior que na v1.3, com quebra automática de linha.
  String msg=lastErrorLine1;
  if(lastErrorLine2.length()){
    msg += " ";
    msg += lastErrorLine2;
  }

  drawWrappedPT(msg,76,117,205,21,3,&FreeSans9pt7b,C_TEXT);

  button(18,195,284,35,"VOLTAR",C_BLUE);
}

// ============================================================================
// ENSAIO / ANALISE TLS
// ============================================================================

void resetAnalysis(){ heatCount=0; lastHeatStoredMs=0; resultTLS={false,0,0,0,0,0,0,0,0,0,-1,-1}; }

void startHeatingCommand(){
  t0C=calcT0();
  char cmd[64]; snprintf(cmd,sizeof(cmd),"HEATER_ON,%lu,%lu",(unsigned long)profileHeatMs(),(unsigned long)profileCoolMs());
  SerialBT.println(cmd); testStage=ST_HEATING; stageStartMs=millis(); drawLive(true);
}

void startNewTest(){
  if(!btOK || !SerialBT.connected()){
    if(screenState==SCR_HOME) showHomeNoResponse();
    else { setError("BT_OFF","SEM CONEXÃO","O medidor parou de responder.","Aguarde e tente novamente."); drawError(); }
    return;
  }
  // O fator e congelado no inicio para manter a rastreabilidade do ensaio.
  factorForCurrentTest = (!calibrationRun && calibrationActive && calibrationValid) ? calibrationFactor : 1.0f;
  userStopRequested=false; testActive=true; meterReady=true; resetStab(); resetAnalysis(); resetGraph();
  liveTempC=NAN; livePowerW=0.0f; liveQ=0.0f; testStartMs=millis(); stageStartMs=millis(); testStage=ST_STABILIZING;
  openTestLog(); SerialBT.println("MONITOR_START"); drawLive(true);
}

void stopTestByUser(){
  if(btOK && SerialBT.connected())SerialBT.println("CYCLE_STOP");
  userStopRequested=true; testActive=false; calibrationRun=false; closeTestLog();
  setError("USER_STOP","ENSAIO INTERROMPIDO","O ensaio foi parado","pelo usuário."); drawError();
}

void storeHeatingSample(uint32_t phaseMs,float tempC,float powerW){
  if(phaseMs<500 || phaseMs-lastHeatStoredMs<500 || heatCount>=MAX_HEAT_SAMPLES)return;
  lastHeatStoredMs=phaseMs; heatT[heatCount]=phaseMs/1000.0f; heatTemp[heatCount]=tempC; heatPower[heatCount]=powerW; heatCount++;
}

void analyzeTLS(){
  resultTLS={false,0,0,0,0,0,0,0,0,0,-1,-1}; if(heatCount<10)return;
  static float px[MAX_HEAT_SAMPLES+1],py[MAX_HEAT_SAMPLES+1],pxx[MAX_HEAT_SAMPLES+1],pxy[MAX_HEAT_SAMPLES+1],pyy[MAX_HEAT_SAMPLES+1];
  px[0]=py[0]=pxx[0]=pxy[0]=pyy[0]=0;
  for(int i=0;i<heatCount;i++){
    float tt=max(heatT[i],0.05f), x=log(tt), y=heatTemp[i];
    px[i+1]=px[i]+x; py[i+1]=py[i]+y; pxx[i+1]=pxx[i]+x*x; pxy[i+1]=pxy[i]+x*y; pyy[i+1]=pyy[i]+y*y;
  }
  float bestScore=-1e9,bestSlope=0,bestR2=0; int bestI=-1,bestJ=-1;
  for(int i=0;i<heatCount-5;i++){
    if(heatT[i]<0.5f)continue;
    for(int j=i+5;j<heatCount;j++){
      if(heatT[j]/heatT[i]<10.0f)continue;
      int n=j-i+1; double sx=px[j+1]-px[i],sy=py[j+1]-py[i],sxx=pxx[j+1]-pxx[i],sxy=pxy[j+1]-pxy[i],syy=pyy[j+1]-pyy[i];
      double denX=n*sxx-sx*sx,denY=n*syy-sy*sy; if(denX<=1e-12||denY<=1e-12)continue;
      double slope=(n*sxy-sx*sy)/denX; if(slope<=0)continue;
      double r=(n*sxy-sx*sy)/sqrt(denX*denY),r2=r*r; float cycles=log10(heatT[j]/heatT[i]); float score=r2+0.001f*min(cycles,3.0f);
      if(score>bestScore){bestScore=score;bestI=i;bestJ=j;bestSlope=slope;bestR2=r2;}
    }
  }
  if(bestI<0||bestJ<0)return;
  double pSum=0; int pN=0; for(int i=bestI;i<=bestJ;i++){ if(heatPower[i]>0){pSum+=heatPower[i];pN++;} }
  if(!pN)return;
  float pMean=pSum/pN,lengthM=activeLengthMm/1000.0f; if(lengthM<=0)return;
  float q=pMean/lengthM; if(q<=0||bestSlope<=0)return;
  float rho=4.0f*PI*bestSlope/q; if(rho<=0)return;
  resultTLS.valid=true; resultTLS.rho=rho; resultTLS.k=1.0f/rho; resultTLS.r2=bestR2; resultTLS.slope=bestSlope;
  resultTLS.pMean=pMean; resultTLS.qMean=q; resultTLS.tStart=heatT[bestI]; resultTLS.tEnd=heatT[bestJ];
  resultTLS.logCycles=log10(heatT[bestJ]/heatT[bestI]); resultTLS.i0=bestI; resultTLS.i1=bestJ;
}

void finalizeTest(){
  testActive=false;
  testStage=ST_PROCESSING;
  drawLive(true);
  analyzeTLS();
  closeTestLog();
  writeSummary();
  testSavedOK=verifyCurrentTestSaved();
  testStage=ST_DONE;
  if(calibrationRun && resultTLS.valid) drawCalibrationReview();
  else {calibrationRun=false;drawResults();}
}

// ============================================================================
// PARSER BLUETOOTH
// ============================================================================

String csvToken(const String &s,int index){
  int start=0,current=0;
  for(int i=0;i<=s.length();i++){
    if(i==s.length()||s.charAt(i)==','){
      if(current==index)return s.substring(start,i);
      current++; start=i+1;
    }
  }
  return "";
}

void processDataPacket(const String &line){
  String phase=csvToken(line,1);
  uint32_t sessionMs=(uint32_t)csvToken(line,2).toInt();
  uint32_t phaseMs=(uint32_t)csvToken(line,3).toInt();
  long tempMC=csvToken(line,4).toInt(),powerMW=csvToken(line,5).toInt();
  float tempC=tempMC/1000.0f,powerW=powerMW/1000.0f;
  liveTempC=tempC; livePowerW=powerW; float lengthM=activeLengthMm/1000.0f; liveQ=(lengthM>0)?powerW/lengthM:0;
  addGraphSample(sessionMs,tempC); logDataLine(sessionMs,phase,phaseMs,tempC,powerW);
  String p=phase; p.toUpperCase();
  if(testActive && testStage==ST_STABILIZING){ addStabSample(millis(),tempC); liveDriftCPerMin=calcDriftCPerMin(); if(stabilizationReached())startHeatingCommand(); }
  if(p.indexOf("HEAT")>=0){ if(testStage!=ST_HEATING){testStage=ST_HEATING;drawLive(true);} storeHeatingSample(phaseMs,tempC,powerW); }
  else if(p.indexOf("COOL")>=0){ if(testStage!=ST_COOLING){testStage=ST_COOLING;drawLive(true);} }
  if(screenState==SCR_LIVE && millis()-lastUIRefresh>700){ lastUIRefresh=millis(); drawLive(false); }
}


void processBatteryPacket(const String &line){
  // BAT,<tensao_mV>,<percentual>,<CHARGING|IDLE>
  long mv = csvToken(line,1).toInt();
  int pct = csvToken(line,2).toInt();
  String state = csvToken(line,3);
  state.trim();
  state.toUpperCase();

  bool hadPacket = batteryPacketSeen;
  bool wasCharging = batteryCharging;

  batteryPacketSeen = true;
  batteryMv = (mv > 0 && mv <= 65535L) ? (uint16_t)mv : 0;
  batteryValid = (mv > 0 && pct >= 0 && pct <= 100);
  batteryPercent = batteryValid ? constrain(pct,0,100) : -1;
  batteryCharging = (state == "CHARGING");

  // Atualiza somente o canto direito do cabecalho.
  if(screenState != SCR_SPLASH){
    drawHeaderIndicators();
  }

  // Popup somente na transicao para CHARGING.
  // Se a primeira informacao recebida ja diz CHARGING, tambem avisamos.
  if(batteryCharging && (!wasCharging || !hadPacket)){
    showChargingPopup();
  }
}

void processBTLine(const String &rawLine){
  String line=rawLine; line.trim(); if(line.length()==0)return;
  Serial.print("BT << "); Serial.println(line);
  if(line.startsWith("READY")){ meterReady=true; return; }
  if(line.startsWith("PONG")){ meterReady=true; return; }
  if(line.startsWith("BAT,")){ processBatteryPacket(line); return; }
  if(line.startsWith("STATUS,")){
    String code=line.substring(7);
    code.trim();
    decodeStatusError(code);

    bool actionable =
      code.indexOf("FAIL")>=0 ||
      code.indexOf("ABORT")>=0 ||
      code=="START_BLOCKED_LOW_BATTERY";

    if(actionable){
      if(testActive){
        testActive=false;calibrationRun=false;
        closeTestLog();
      }

      // Se o inicio foi recusado, nao deixa um CSV vazio do ensaio rejeitado.
      if(code=="START_BLOCKED_LOW_BATTERY"){
        if(sdOK && currentCsvPath.length() && SD.exists(currentCsvPath)){
          SD.remove(currentCsvPath);
        }
        currentCsvPath="";
        currentSummaryPath="";
      }

      drawError();
    }
    return;
  }
  if(line.startsWith("DATA,")){ processDataPacket(line); return; }
  if(line=="CYCLE_END"){ if(testActive)finalizeTest(); return; }
  if(line=="CYCLE_STOPPED"){
    if(!userStopRequested && testActive){ testActive=false;calibrationRun=false; closeTestLog(); setError("CYCLE_STOPPED","CICLO INTERROMPIDO","O medidor encerrou","o ciclo antes da conclusão."); drawError(); }
    return;
  }
}

void processBluetooth(){
  bool connectedNow=SerialBT.connected();

  // Em modo PC a IHM NAO pode reassumir o TLS-METER. Caso algum link
  // residual apareca, derrubamos imediatamente e mantemos os dados remotos
  // de bateria invalidos.
  if(pcMode){
    if(connectedNow){
      SerialBT.disconnect();
      delay(30);
    }

    btOK=false;
    meterReady=false;
    invalidateBatteryTelemetry();

    while(SerialBT.available()) SerialBT.read();
    btLine="";
    return;
  }

  if(btOK&&!connectedNow){
    btOK=false;
    meterReady=false;

    // Sem Bluetooth, o ultimo percentual nao e mais informacao atual.
    invalidateBatteryTelemetry();


    if(testActive){
      testActive=false;
      calibrationRun=false; // descarta proposta se o enlace caiu
      closeTestLog();

      setError("BT_LOST",
               "ENSAIO INTERROMPIDO",
               "Conexão Bluetooth perdida.",
               "Ensaio interrompido. Tente novamente.");
      drawError();
    }
    else if(screenState==SCR_HOME){
      drawHome();
      showHomeNoResponse();
    }
    else{
      // Atualiza o indicador BT mesmo fora da HOME.
      drawHeaderIndicators();
    }
  }
  else if(!btOK&&connectedNow){
    btOK=true;
    homeWarningActive=false;
    if(screenState==SCR_HOME) drawHome();
    else if(screenState!=SCR_SPLASH) drawHeaderIndicators();

    // Solicita uma leitura atual imediatamente apos recuperar o link.
    SerialBT.println("BAT?");
  }

  while(SerialBT.available()){
    char ch=(char)SerialBT.read();

    if(ch=='\n'||ch=='\r'){
      if(btLine.length()){
        processBTLine(btLine);
        btLine="";
      }
    }
    else{
      if(btLine.length()<180) btLine+=ch;
      else btLine="";
    }
  }
}

// ============================================================================
// EVENTOS TOUCH
// ============================================================================

void handleHomeTap(int16_t x,int16_t y){
  if(inRect(x,y,8,42,148,58)){ if(!btOK||!SerialBT.connected()){showHomeNoResponse();return;} startNewTest();return;}
  if(inRect(x,y,164,42,148,58)){historyOffset=0;drawHistory();return;}
  if(inRect(x,y,8,106,148,58)){
    if(pcMode) drawPCSettings();
    else {connectMeter(true);drawHome();}
    return;
  }
  if(inRect(x,y,164,106,148,58)){drawSettingsMenu();return;}
  if(inRect(x,y,8,170,304,34)){drawCredits();return;}
}

void handleLiveTap(int16_t x,int16_t y){ if(inRect(x,y,7,198,306,34))stopTestByUser(); }
void handleResultsTap(int16_t x,int16_t y){ if(inRect(x,y,7,207,148,28))drawHome(); }


void handleSettingsMenuTap(int16_t x,int16_t y){
  if(inRect(x,y,8,42,148,58)){drawDisplaySettings();return;}
  if(inRect(x,y,164,42,148,58)){drawSettings();return;}
  if(inRect(x,y,8,106,148,58)){enterPCMode();return;}
  if(inRect(x,y,164,106,148,58)){drawSystemSettings();return;}
  if(inRect(x,y,8,207,304,28)){drawHome();return;}
}

void handleDisplaySettingsTap(int16_t x,int16_t y){
  const uint8_t values[5]={10,25,50,75,100};

  for(int i=0;i<5;i++){
    int bx=10+i*61;
    if(inRect(x,y,bx,125,56,30)){
      screenBrightnessPercent=values[i];
      applyScreenBrightness();
      saveDisplaySettings();
      drawDisplaySettings();
      return;
    }
  }

  if(inRect(x,y,8,207,304,28)){drawSettingsMenu();return;}
}

void handlePCSettingsTap(int16_t x,int16_t y){
  if(inRect(x,y,8,174,304,28)){
    attemptReturnToIHM();
    return;
  }

  if(inRect(x,y,8,207,304,28)){
    drawSettingsMenu();
    return;
  }
}

void handleSystemSettingsTap(int16_t x,int16_t y){
  if(inRect(x,y,8,176,148,27)){
    // Mostra feedback antes do teste, que pode levar alguns segundos.
    tft.fillRoundRect(28,72,264,92,10,C_PANEL2);
    tft.drawRoundRect(28,72,264,92,10,C_CYAN);
    drawBitmapIcon(TLS_ICON_STORAGE,43,101,C_CYAN);
    drawCenteredFontText("Testando microSD...",82,82,195,32,&FreeSansBold9pt7b,C_CYAN);
    drawCenteredFontText("Aguarde.",82,120,195,24,&FreeSans9pt7b,C_TEXT);
    useDefaultFont();

    sdOK=initSD();
    drawSystemSettings();
    return;
  }

  if(inRect(x,y,164,176,148,27)){
    tft.fillScreen(C_BG);
    drawCenteredFontText("Reiniciando IHM...",0,92,SCREEN_W,40,&FreeSansBold12pt7b,C_CYAN);
    useDefaultFont();
    delay(350);
    ESP.restart();
    return;
  }

  if(inRect(x,y,8,207,304,28)){drawSettingsMenu();return;}
}

void handleSettingsTap(int16_t x,int16_t y){
  // Botao TROCAR muda de posicao quando ha o botao OPCOES.
  bool trocaTap = (currentProfile==PROFILE_DEV)
                   ? inRect(x,y,232,56,70,28)
                   : inRect(x,y,164,56,66,28);

  if(trocaTap){
    if(currentProfile==PROFILE_DEV) currentProfile=PROFILE_LAB;
    else if(currentProfile==PROFILE_LAB) currentProfile=PROFILE_FIELD;
    else currentProfile=PROFILE_DEV;
    saveSettings();
    drawSettings();
    return;
  }

  if(currentProfile!=PROFILE_DEV && inRect(x,y,236,56,66,28)){
    drawProfileOptions();
    return;
  }

  if(inRect(x,y,18,116,40,26)){activeLengthMm=max(20.0f,activeLengthMm-5.0f);saveSettings();drawSettings();return;}
  if(inRect(x,y,254,116,40,26)){activeLengthMm=min(500.0f,activeLengthMm+5.0f);saveSettings();drawSettings();return;}
  if(inRect(x,y,18,171,40,26)){probeDiameterMm=max(0.5f,probeDiameterMm-0.1f);saveSettings();drawSettings();return;}
  if(inRect(x,y,254,171,40,26)){probeDiameterMm=min(20.0f,probeDiameterMm+0.1f);saveSettings();drawSettings();return;}
  if(inRect(x,y,8,211,148,25)){drawCalibrationSettings();return;}
  if(inRect(x,y,164,211,148,25)){drawSettingsMenu();return;}
}

void handleProfileOptionsTap(int16_t x,int16_t y){
  if(currentProfile==PROFILE_DEV){drawSettings();return;}

  // Estabilizacao minima
  if(inRect(x,y,18,72,42,23)){
    if(currentProfile==PROFILE_LAB){
      if(labStabMin>5) labStabMin-=5;
    } else {
      fieldStabMin=10;
    }
    saveSettings(); drawProfileOptions(); return;
  }
  if(inRect(x,y,260,72,42,23)){
    if(currentProfile==PROFILE_LAB){
      if(labStabMin<15) labStabMin+=5;
    } else {
      fieldStabMin=15;
    }
    saveSettings(); drawProfileOptions(); return;
  }

  // Aquecimento
  if(inRect(x,y,18,120,42,23)){
    if(currentProfile==PROFILE_LAB){
      if(labHeatMin>10) labHeatMin-=5;
    } else {
      fieldHeatMin=30;
    }
    saveSettings(); drawProfileOptions(); return;
  }
  if(inRect(x,y,260,120,42,23)){
    if(currentProfile==PROFILE_LAB){
      if(labHeatMin<20) labHeatMin+=5;
    } else {
      fieldHeatMin=40;
    }
    saveSettings(); drawProfileOptions(); return;
  }

  // Restaurar padroes do perfil atual
  if(inRect(x,y,8,207,148,28)){
    if(currentProfile==PROFILE_LAB){
      labStabMin=10;
      labHeatMin=10;
    } else {
      fieldStabMin=10;
      fieldHeatMin=40;
    }
    saveSettings();
    drawProfileOptions();
    return;
  }

  if(inRect(x,y,164,207,148,28)){
    drawSettings();
    return;
  }
}

void handleHistoryTap(int16_t x,int16_t y){
  if(inRect(x,y,8,211,95,25)){drawHome();return;}
  if(inRect(x,y,112,211,95,25)){if(historyOffset>0){historyOffset=max(0,historyOffset-5);drawHistory();}return;}
  if(inRect(x,y,217,211,95,25)){if(historyOffset+5<historyCount){historyOffset+=5;drawHistory();}return;}
  int shown=min(5,historyCount-historyOffset);
  for(int i=0;i<shown;i++){int y0=42+i*33;if(inRect(x,y,8,y0,304,29)){drawHistoryDetail(historyFiles[historyOffset+i]);return;}}
}

void handleHistoryDetailTap(int16_t x,int16_t y){ if(inRect(x,y,8,207,304,28))drawHistory(); }
void handleCreditsTap(int16_t x,int16_t y){ if(inRect(x,y,8,207,304,28))drawHome(); }
void handleErrorTap(int16_t x,int16_t y){ if(inRect(x,y,18,195,284,35))drawHome(); }

void handleTouch(){
  int16_t x,y; if(!getTap(x,y))return; Serial.printf("TOUCH x=%d y=%d\n",x,y);

  // O aviso de falha do cartao fica 5 s na HOME sem bloquear Bluetooth/loop,
  // mas evita toques acidentais por baixo do popup.
  if(sdWarningActive && screenState==SCR_HOME) return;

  switch(screenState){
    case SCR_HOME:handleHomeTap(x,y);break;
    case SCR_LIVE:handleLiveTap(x,y);break;
    case SCR_RESULTS:handleResultsTap(x,y);break;
    case SCR_HISTORY:handleHistoryTap(x,y);break;
    case SCR_HISTORY_DETAIL:handleHistoryDetailTap(x,y);break;
    case SCR_SETTINGS_MENU:handleSettingsMenuTap(x,y);break;
    case SCR_DISPLAY_SETTINGS:handleDisplaySettingsTap(x,y);break;
    case SCR_SETTINGS:handleSettingsTap(x,y);break;
    case SCR_CAL_SETTINGS:handleCalibrationSettingsTap(x,y);break;
    case SCR_CAL_REVIEW:handleCalibrationReviewTap(x,y);break;
    case SCR_CAL_RESET:handleCalibrationResetTap(x,y);break;
    case SCR_PROFILE_OPTIONS:handleProfileOptionsTap(x,y);break;
    case SCR_PC_SETTINGS:handlePCSettingsTap(x,y);break;
    case SCR_SYSTEM_SETTINGS:handleSystemSettingsTap(x,y);break;
    case SCR_CREDITS:handleCreditsTap(x,y);break;
    case SCR_ERROR:handleErrorTap(x,y);break;
    default:break;
  }
}

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup(){
  Serial.begin(115200); delay(100);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  tftSpi.begin(TFT_CLK,TFT_MISO,TFT_MOSI,TFT_CS); tft.init(240,320); tft.invertDisplay(false); tft.setRotation(3); tft.fillScreen(C_BG);
  pinMode(TOUCH_MOSI,OUTPUT); pinMode(TOUCH_MISO,INPUT); pinMode(TOUCH_CLK,OUTPUT); pinMode(TOUCH_CS,OUTPUT); pinMode(TOUCH_IRQ,INPUT);
  digitalWrite(TOUCH_CS,HIGH); digitalWrite(TOUCH_CLK,LOW);
  prefs.begin("tlsdisplay",false);
  currentProfile=(Profile)prefs.getUChar("profile",PROFILE_DEV);
  if(currentProfile!=PROFILE_DEV && currentProfile!=PROFILE_LAB && currentProfile!=PROFILE_FIELD) currentProfile=PROFILE_DEV;
  activeLengthMm=prefs.getFloat("Lmm",100.0f);
  probeDiameterMm=prefs.getFloat("Dmm",1.5f);
  calibrationReferenceDraft=prefs.getFloat("cal_ref_d",0.0f);
  calibrationFactor=prefs.getFloat("cal_factor",1.0f);
  calibrationReferenceUsed=prefs.getFloat("cal_ref_u",0.0f);
  calibrationLastRawK=prefs.getFloat("cal_k_raw",0.0f);
  calibrationValid=prefs.getBool("cal_valid",false);
  calibrationActive=prefs.getBool("cal_active",false);
  if(!isfinite(calibrationReferenceDraft)||calibrationReferenceDraft<0||calibrationReferenceDraft>10)
    calibrationReferenceDraft=0;
  if(!isfinite(calibrationFactor)||calibrationFactor<0.1f||calibrationFactor>10.0f){
    calibrationFactor=1.0f;calibrationValid=false;calibrationActive=false;
  }
  if(!calibrationValid)calibrationActive=false;

  labStabMin=prefs.getUChar("labStab",10);
  labHeatMin=prefs.getUChar("labHeat",10);
  fieldStabMin=prefs.getUChar("fldStab",10);
  fieldHeatMin=prefs.getUChar("fldHeat",40);

  screenBrightnessPercent=prefs.getUChar("bright",75);
  if(screenBrightnessPercent!=10 && screenBrightnessPercent!=25 &&
     screenBrightnessPercent!=50 && screenBrightnessPercent!=75 &&
     screenBrightnessPercent!=100){
    screenBrightnessPercent=75;
  }
  applyScreenBrightness();

  if(labStabMin!=5 && labStabMin!=10 && labStabMin!=15) labStabMin=10;
  if(labHeatMin!=10 && labHeatMin!=15 && labHeatMin!=20) labHeatMin=10;
  if(fieldStabMin!=10 && fieldStabMin!=15) fieldStabMin=10;
  if(fieldHeatMin!=30 && fieldHeatMin!=40) fieldHeatMin=40;
  drawSplash();

  // O autoteste do microSD e executado antes do Bluetooth.
  // Assim o barramento VSPI e validado isoladamente durante o boot.
  sdOK=initSD();
  if(!SerialBT.begin(BT_LOCAL_NAME,true))Serial.println("Falha ao iniciar BluetoothSerial");
  delay(1500);
  int16_t tx,ty; for(int i=0;i<4;i++){readTouch(tx,ty);delay(20);}
  connectMeter(true);
  drawHome();
  if(sdDetectedButFailed) showSDFailurePopup();
}

void loop(){
  processBluetooth();
  handleTouch();
  updateTransientUI();

  // Nao fazemos reconexao automatica bloqueante na HOME.
  // Isso evita a impressao de travamento e mantem Historico, Creditos e Ajustes acessiveis.
  // A reconexao ocorre quando o usuario toca em CONECTAR.
  delay(5);
}
