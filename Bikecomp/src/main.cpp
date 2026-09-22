#include <Arduino.h>
#include <cstring>

//#include <bb_captouch.h>

/*
  ============================================================
  ВЕЛОКОМПЬЮТЕР ESP32 (JC2432W328 / ST7789) — V4
  ============================================================

  Датчики / модули:
    KY-024 Hall       -> GPIO16
    BME280 (t/давл/влажн) -> I2C шина Wire: SDA=GPIO21, SCL=GPIO22
    DS3231 (RTC)      -> та же I2C шина Wire (SDA=GPIO21, SCL=GPIO22)
    CST820 (тач)      -> ОТДЕЛЬНАЯ I2C шина touchWire: SDA=GPIO33, SCL=GPIO32
    Батарея           -> GPIO39   (аналог, через делитель напряжения)
    Фоторезистор (LDR)-> GPIO34   (аналог)

    ПРОВЕРЬТЕ все пины на вашей плате! DS18B20 в текущем коде не
    используется — температура/давление/влажность берутся с BME280.

  Экран:
    320x240, Rotation 1
    Тема: белый фон / чёрный текст (день) <-> чёрный фон / белый текст (ночь)
    Оранжевые акценты — НЕ зависят от темы, остаются оранжевыми всегда

  РАСКЛАДКА:

    Верхняя строка:
      [температура]      [часы:минуты]      [батарея]

    Скорость:
      крупно по центру

    Если тема переключается "наоборот" (темнеет на свету) —
      поменяйте местами сравнения в updateAmbientLight() или
      просто инвертируйте LIGHT_THRESHOLD_DARK / LIGHT_THRESHOLD_LIGHT
      логику (< на >). Направление зависит от того, как включён
      делитель (LDR сверху к VCC или снизу к GND).
*/


#include <TFT_eSPI.h>
#include <Adafruit_BME280.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <CST820.h>
#include <SD.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
#include <FS.h>


// ============================================================
// ОБЪЕКТЫ
// ============================================================

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite speedSprite = TFT_eSprite(&tft);
Preferences prefs;
Adafruit_BME280 bme;

RTC_DS3231 rtc;
bool rtcAvailable = false;
bool bmeAvailable = false;
TwoWire touchWire = TwoWire(1);
int clockSetHour = 0;
int clockSetMinute = 0;
bool clockTimeChanged = false;


// ============================================================
// НАСТРОЙКИ / ПИНЫ
// ============================================================

const int HALL_PIN       = 16;
const int BACKLIGHT_PIN  = 27;   // TFT_BL на JC2432W328
const int BATTERY_PIN    = 39;   // input-only ADC, обычно свободен
const int LDR_PIN        = 34;   // фоторезистор, input-only ADC
const uint8_t BME280_ADDR = 0x76;

const int SD_CS_PIN = 5;   // ПРОВЕРЬТЕ пин CS для вашей ревизии JC2432W328!
const char* BOOT_IMAGE_PATH = "/logo.jpg";
const char* LIGHT_BACKGROUND_PATH = "/light.jpg";
const char* DARK_BACKGROUND_PATH = "/dark.jpg";
const char* LIGHT_MPH_BACKGROUND_PATH = "/lightmph.jpg";
const char* DARK_MPH_BACKGROUND_PATH = "/darkmph.jpg";
const char* SETTINGS_BACKGROUND_PATH = "/settings.jpg";
bool sdAvailable = false;

// Основная I2C шина для BME280 и DS3231.
const int I2C_SDA_PIN = 21;
const int I2C_SCL_PIN = 22;

// Отдельная I2C шина для CST820.
const int TOUCH_SDA_PIN = 33;
const int TOUCH_SCL_PIN = 32;

// CST820 — встроенный ёмкостный тач JC2432W328
const uint8_t TOUCH_I2C_ADDR = 0x15;
const int TOUCH_RST_PIN = 25;

// Длина окружности колеса (мм)
float wheelCircumferenceMm = 2275.0;
// Сколько магнитов установлено на колесе (равномерно по окружности).
// Меняет ТОЛЬКО частоту срабатывания датчика — settings и NVS
// по-прежнему хранят полную окружность колеса в мм.
int magnetsPerRevolution = 1;
const int MAGNETS_MIN = 1;
const int MAGNETS_MAX = 4;

// Шаг изменения окружности колеса в настройках
const float CIRCUMFERENCE_STEP_MM = 5.0;
const float CIRCUMFERENCE_MIN_MM  = 1000.0;
const float CIRCUMFERENCE_MAX_MM  = 3000.0;

// Антидребезг Hall
const unsigned long DEBOUNCE_US = 15000;

// Фильтр ложных импульсов Hall.
// Импульс полностью отбрасывается ДО увеличения pulseCount, если он
// соответствует физически неправдоподобной скорости или слишком резкому
// одиночному скачку относительно предыдущего принятого импульса.
const float MAX_PLAUSIBLE_SPEED_KMH = 65.0f;
const float MAX_SPEED_JUMP_KMH      = 15.0f;

// Скорость = 0, если импульсов нет дольше этого времени
const unsigned long SPEED_TIMEOUT_MS = 2000;

// Интервал обновления температуры
const unsigned long TEMP_UPDATE_MS = 5000;

// Интервал обновления батареи
const unsigned long BATTERY_UPDATE_MS = 5000;

// Сколько сэмплов усредняем при чтении батареи
const int BATTERY_SAMPLES = 16;

// Делитель напряжения: Vbat = Vadc * BATTERY_DIVIDER_RATIO
// Подберите под свои резисторы!
const float BATTERY_DIVIDER_RATIO = 1.61;

// Диапазон напряжения LiPo для расчёта процента
const float BATTERY_MIN_V = 3.3;
const float BATTERY_MAX_V = 4.2;


// Сохранять общий пробег каждые 0.1 км
const float ODOMETER_SAVE_STEP_KM = 0.1;

// Заставка
const unsigned long BOOT_LOGO_MS = 3000;

// -------------------- Фоторезистор / тема --------------------
const unsigned long LIGHT_UPDATE_MS = 2000;
const int LIGHT_SAMPLES = 16;

// Пороги с гистерезисом (сырые значения ADC, 0..4095).
// У вашей схемы: на свету ADC ~0, в темноте (закрыт) ~1770.
// Значит чем темнее, тем БОЛЬШЕ значение ADC.
const int LIGHT_THRESHOLD_DARK  = 100;   // выше -> переключаемся в тёмную тему
const int LIGHT_THRESHOLD_LIGHT = 30;   // ниже -> переключаемся в светлую тему


// ============================================================
// ЦВЕТА
// ============================================================

// Фон и текст — теперь переменные, т.к. меняются по освещённости.
uint16_t colBg   = TFT_WHITE;
uint16_t colText = TFT_BLACK;

// ============================================================
// HALL (без изменений по логике)
// ============================================================

volatile unsigned long pulseCount = 0;
volatile unsigned long lastPulseMicros = 0;
volatile unsigned long pulseIntervalMicros = 0;
// Минимальный допустимый интервал между настоящими импульсами Hall.
// Значение рассчитывается ВНЕ ISR, чтобы внутри прерывания не было float/FPU.
volatile unsigned long hallMinIntervalUs = DEBOUNCE_US;
unsigned long hallAbsoluteMinIntervalUs = DEBOUNCE_US;
// Суммарное время фактического движения. В него попадают только
// интервалы между последовательными импульсами колеса короче таймаута.
volatile uint64_t totalMovingMicros = 0;

// Пересчитать абсолютный предел Hall по длине колеса, количеству магнитов
// и MAX_PLAUSIBLE_SPEED_KMH. Вызывать только из обычного кода, НЕ из ISR.
void updateHallAbsoluteFilterLimit()
{
  float distancePerPulseMm = wheelCircumferenceMm / magnetsPerRevolution;
  unsigned long limitUs = (unsigned long)(
      (distancePerPulseMm * 3600.0f) / MAX_PLAUSIBLE_SPEED_KMH);

  if (limitUs < DEBOUNCE_US)
    limitUs = DEBOUNCE_US;

  hallAbsoluteMinIntervalUs = limitUs;
  hallMinIntervalUs = limitUs;
}

void IRAM_ATTR onHallPulse()
{
  unsigned long now = micros();

  if (lastPulseMicros == 0)
  {
    lastPulseMicros = now;
    pulseCount++;
    return;
  }

  unsigned long delta = now - lastPulseMicros;

  // В ISR только быстрые целочисленные операции.
  // Никаких float, делений или FPU внутри аппаратного прерывания.
  if (delta < hallMinIntervalUs)
    return;

  pulseIntervalMicros = delta;

  if (delta <= (SPEED_TIMEOUT_MS * 1000UL))
    totalMovingMicros += delta;

  lastPulseMicros = now;
  pulseCount++;
}

// ============================================================
// JPEG DECODER CALLBACK
// ============================================================
// TJpg_Decoder вызывает эту функцию для каждого декодированного
// блока изображения и сам передаёт координаты/размер/пиксели.
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
  if (y >= tft.height()) return 0;

  tft.pushImage(x, y, w, h, bitmap);

  return 1; // продолжить декодирование
}

uint16_t* backgroundTemp = nullptr;
uint16_t* backgroundClock = nullptr;
uint16_t* backgroundBattery = nullptr;
uint16_t* backgroundMetrics = nullptr;
uint16_t* backgroundTimer = nullptr;
uint16_t* backgroundTrip = nullptr;
uint16_t* backgroundOdometer = nullptr;
uint16_t* backgroundSpeed = nullptr;
uint16_t* backgroundTimerLabel = nullptr;
uint16_t* backgroundTripLabel = nullptr;

// Маленькие буферы фона Settings: позволяют менять значения без
// повторной отрисовки всего /settings.jpg.
uint16_t* settingsWheelBg = nullptr;
uint16_t* settingsMagnetsBg = nullptr;
uint16_t* settingsHoursBg = nullptr;
uint16_t* settingsMinutesBg = nullptr;

// ============================================================
// USB / SERIAL -> SD SERVICE MODE
// ============================================================
// Компьютер посылает строку "SDUP" через USB-COM. После этого
// основной интерфейс останавливается, крупные графические буферы
// освобождаются, а файлы принимаются небольшими блоками и
// записываются прямо на SD-карту. Wi-Fi для этого не нужен.
bool sdUsbMode = false;
const size_t SD_USB_BLOCK_SIZE = 512;
const unsigned long SD_USB_RX_TIMEOUT_MS = 10000;
const char* SD_USB_TEMP_PATH = "/.__upload.tmp";

const int SETTINGS_WHEEL_BG_X = 176;
const int SETTINGS_WHEEL_BG_Y = 54;
const int SETTINGS_WHEEL_BG_W = 82;
const int SETTINGS_WHEEL_BG_H = 28;

const int SETTINGS_MAGNETS_BG_X = 186;
const int SETTINGS_MAGNETS_BG_Y = 84;
const int SETTINGS_MAGNETS_BG_W = 45;
const int SETTINGS_MAGNETS_BG_H = 26;

const int SETTINGS_HOURS_BG_X = 191;
const int SETTINGS_HOURS_BG_Y = 114;
const int SETTINGS_HOURS_BG_W = 42;
const int SETTINGS_HOURS_BG_H = 24;

const int SETTINGS_MINUTES_BG_X = 196;
const int SETTINGS_MINUTES_BG_Y = 144;
const int SETTINGS_MINUTES_BG_W = 43;
const int SETTINGS_MINUTES_BG_H = 24;

void copyBackgroundBlock(int16_t x, int16_t y, uint16_t w, uint16_t h,
                         uint16_t* bitmap, int regionX, int regionY,
                         int regionW, int regionH, uint16_t* destination)
{
  if (!destination || x >= regionX + regionW || x + w <= regionX ||
      y >= regionY + regionH || y + h <= regionY)
    return;

  int startX = max((int)x, regionX);
  int endX = min((int)x + (int)w, regionX + regionW);
  int startY = max((int)y, regionY);
  int endY = min((int)y + (int)h, regionY + regionH);

  for (int row = startY; row < endY; row++)
  {
    uint16_t* source = bitmap + (row - y) * w + (startX - x);
    uint16_t* target = destination + (row - regionY) * regionW + (startX - regionX);
    memcpy(target, source, (endX - startX) * sizeof(uint16_t));
  }
}

bool backgroundBufferOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
  copyBackgroundBlock(x, y, w, h, bitmap, 0, 0, 120, 72, backgroundTemp);
  copyBackgroundBlock(x, y, w, h, bitmap, 112, 0, 100, 60, backgroundClock);
  // Y = 6 соответствует BATTERY_Y - 12 (см. restoreMainBackgroundRegion
  // и вызов в drawBatteryIcon) — раньше здесь было 8, из-за чего
  // буфер фона и место восстановления не совпадали на 2px.
  copyBackgroundBlock(x, y, w, h, bitmap, 240, 6, 75, 22, backgroundBattery);
  copyBackgroundBlock(x, y, w, h, bitmap, 0, 60, 118, 126, backgroundMetrics);
  copyBackgroundBlock(x, y, w, h, bitmap, 2, 202, 102, 32, backgroundTimer);
  copyBackgroundBlock(x, y, w, h, bitmap, 108, 202, 103, 32, backgroundTrip);
  copyBackgroundBlock(x, y, w, h, bitmap, 215, 202, 103, 32, backgroundOdometer);
  copyBackgroundBlock(x, y, w, h, bitmap, 138, 80, 165, 70, backgroundSpeed);
  copyBackgroundBlock(x, y, w, h, bitmap, 0, 177, 106, 28, backgroundTimerLabel);
  copyBackgroundBlock(x, y, w, h, bitmap, 108, 177, 103, 28, backgroundTripLabel);
  return 1;
}

bool settingsBackgroundBufferOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
  copyBackgroundBlock(x, y, w, h, bitmap,
                      SETTINGS_WHEEL_BG_X, SETTINGS_WHEEL_BG_Y,
                      SETTINGS_WHEEL_BG_W, SETTINGS_WHEEL_BG_H, settingsWheelBg);

  copyBackgroundBlock(x, y, w, h, bitmap,
                      SETTINGS_MAGNETS_BG_X, SETTINGS_MAGNETS_BG_Y,
                      SETTINGS_MAGNETS_BG_W, SETTINGS_MAGNETS_BG_H, settingsMagnetsBg);

  copyBackgroundBlock(x, y, w, h, bitmap,
                      SETTINGS_HOURS_BG_X, SETTINGS_HOURS_BG_Y,
                      SETTINGS_HOURS_BG_W, SETTINGS_HOURS_BG_H, settingsHoursBg);

  copyBackgroundBlock(x, y, w, h, bitmap,
                      SETTINGS_MINUTES_BG_X, SETTINGS_MINUTES_BG_Y,
                      SETTINGS_MINUTES_BG_W, SETTINGS_MINUTES_BG_H, settingsMinutesBg);

  return 1;
}

// ============================================================
// СОСТОЯНИЕ
// ============================================================

float speedKmh = 0.0;
float trip1Km = 0.0;
float trip2Km = 0.0;
// avg.speed и max.speed считаются отдельно для trip1 и trip2,
// чтобы долгое нажатие (сброс trip) сбрасывало их вместе с пробегом.
float trip1MaxSpeedKmh = 0.0;
float trip2MaxSpeedKmh = 0.0;
// База накопленного времени движения для каждого trip.
uint64_t trip1MovingBaseMicros = 0;
uint64_t trip2MovingBaseMicros = 0;
float odometerKm = 0.0;
float odometerAtLastSave = 0.0;
unsigned long lastProcessedPulseCount = 0;

float currentTempC = NAN;
float currentPressurePa = 0.0f;
float currentHumidityPercent = 0.0f;
unsigned long lastTempRequest = 0;
enum SpeedUnit { SPEED_KMH = 0, SPEED_MPH = 1 };
enum TemperatureUnit { TEMP_CELSIUS = 0, TEMP_FAHRENHEIT = 1 };
enum PressureUnit { PRESSURE_HPA = 0, PRESSURE_MMHG = 1, PRESSURE_MBAR = 2, PRESSURE_INHG = 3 };

const float KMH_TO_MPH = 0.621371f;

SpeedUnit speedUnit = SPEED_KMH;
TemperatureUnit temperatureUnit = TEMP_CELSIUS;
PressureUnit pressureUnit = PRESSURE_HPA;

int batteryPercent = -1;
float batteryVoltageV = 0.0;
bool batteryShowVoltage = false;
unsigned long lastBatteryRead = 0;

enum ThemeMode {
  THEME_WHITE = 0,
  THEME_DARK  = 1,
  THEME_AUTO  = 2
};

ThemeMode themeMode = THEME_WHITE;
bool isDarkTheme = false;
unsigned long lastLightRead = 0;

bool settingsScreen = false;
bool touchWasDown = false;
unsigned long lastTouchMs = 0;
char lastDrawnAvgLabel[16] = "";
char lastDrawnMaxLabel[16] = "";
float lastDrawnAvgMetricValue = NAN;
float lastDrawnMaxMetricValue = NAN;

int activeTimerIndex = 0;
unsigned long timer1StartedAtMs = 0;
unsigned long timer2StartedAtMs = 0;
unsigned long timer1AccumulatedMs = 0;
unsigned long timer2AccumulatedMs = 0;
bool timerPressActive = false;
unsigned long timerPressStartMs = 0;
char lastDrawnTimerLabel[16] = "";

int activeTripIndex = 0;
bool tripPressActive = false;
unsigned long tripPressStartMs = 0;
char lastDrawnTripLabel[8] = "";

const char* SPEED_UNIT_KEY = "speed_unit";
const char* TEMP_UNIT_KEY = "temp_unit";
const char* PRESSURE_UNIT_KEY = "pressure_unit";

// последние отрисованные значения (для частичной перерисовки)
float lastDrawnSpeed = -1.0;
float animatedSpeedKmh = 0.0f;
unsigned long lastSpeedAnimMs = 0;

const float SPEED_ANIM_STEP_KMH = 1.0f;
const unsigned long SPEED_ANIM_INTERVAL_MS = 50;

float lastDrawnAvgSpeed = -1.0;
float lastDrawnMaxSpeed = -1.0;
float lastDrawnTrip = -1.0;
float lastDrawnOdo = -1.0;
float lastDrawnTemp = -999.0;
float lastDrawnPressure = NAN;
float lastDrawnHumidity = NAN;
int lastDrawnBatteryPct = -999;

int lastDrawnClockH = -1;
int lastDrawnClockM = -1;
int lastDrawnDateDay = -1;
int lastDrawnDateMonth = -1;
int lastDrawnDateYear = -1;

long lastDrawnTimerTotalSec = -1;


// ============================================================
// РАСКЛАДКА
// ============================================================

// ---------------- Верхняя строка ----------------
const int TOP_Y = 20;
const int TEMP_X = 10;
const int TEMP_Y = TOP_Y - 5;
const int CLOCK_X = 166;
const int CLOCK_Y = TOP_Y - 8;
const int DATE_X = CLOCK_X + 2;
const int DATE_Y = TOP_Y + 18;
const int BATT_ICON_RIGHT_X = 314;   // правый край иконки
const int BATTERY_Y = TOP_Y - 2;
const int BATT_ICON_W = 26;
const int BATT_NUB_W = 3;
const int BATT_PERCENT_AREA_X1 = 240;
const int BATT_PERCENT_AREA_X2 = 320;
const int BATT_PERCENT_AREA_Y1 = 5;
const int BATT_PERCENT_AREA_Y2 = 35;

// ---------------- Нижние две строки ----------------
const int LABEL_Y = 195;
const int VALUE_Y = 218;

const int COL1_X = 53;    // timer
const int COL2_X = 160;   // trip
const int COL3_X = 267;   // odo

const int DIV1_X = 106;
const int DIV2_X = 213;
// ---------------- Иконка настроек ----------------
const int WRENCH_TOUCH_X1 = 270;
const int WRENCH_TOUCH_X2 = 319;
const int WRENCH_TOUCH_Y1 = 40;
const int WRENCH_TOUCH_Y2 = 82;

// ---------------- Экран Settings ----------------
// Все координаты ниже относятся к готовому фону /settings.jpg.

// Выбор темы.
const int SETTINGS_LIGHT_X1 = 125;
const int SETTINGS_LIGHT_Y1 = 22;
const int SETTINGS_LIGHT_X2 = 180;
const int SETTINGS_LIGHT_Y2 = 46;

const int SETTINGS_DARK_X1 = 185;
const int SETTINGS_DARK_Y1 = 22;
const int SETTINGS_DARK_X2 = 238;
const int SETTINGS_DARK_Y2 = 46;

const int SETTINGS_AUTO_X1 = 245;
const int SETTINGS_AUTO_Y1 = 22;
const int SETTINGS_AUTO_X2 = 298;
const int SETTINGS_AUTO_Y2 = 46;

// Значения. X/Y — правый нижний угол текста.
const int SETTINGS_WHEEL_X = 253;
const int SETTINGS_WHEEL_Y = 76;
const int SETTINGS_MAGNETS_X = 226;
const int SETTINGS_MAGNETS_Y = 106;
const int SETTINGS_HOURS_X = 231;
const int SETTINGS_HOURS_Y = 136;
const int SETTINGS_MINUTES_X = 232;
const int SETTINGS_MINUTES_Y = 166;

// Кнопки "-" / "+".
const int SETTINGS_WHEEL_MINUS_X1 = 125;
const int SETTINGS_WHEEL_MINUS_Y1 = 52;
const int SETTINGS_WHEEL_MINUS_X2 = 165;
const int SETTINGS_WHEEL_MINUS_Y2 = 76;
const int SETTINGS_WHEEL_PLUS_X1 = 276;
const int SETTINGS_WHEEL_PLUS_Y1 = 52;
const int SETTINGS_WHEEL_PLUS_X2 = 316;
const int SETTINGS_WHEEL_PLUS_Y2 = 76;

const int SETTINGS_MAGNETS_MINUS_X1 = 125;
const int SETTINGS_MAGNETS_MINUS_Y1 = 82;
const int SETTINGS_MAGNETS_MINUS_X2 = 165;
const int SETTINGS_MAGNETS_MINUS_Y2 = 106;
const int SETTINGS_MAGNETS_PLUS_X1 = 276;
const int SETTINGS_MAGNETS_PLUS_Y1 = 82;
const int SETTINGS_MAGNETS_PLUS_X2 = 316;
const int SETTINGS_MAGNETS_PLUS_Y2 = 106;

const int SETTINGS_HOURS_MINUS_X1 = 125;
const int SETTINGS_HOURS_MINUS_Y1 = 112;
const int SETTINGS_HOURS_MINUS_X2 = 165;
const int SETTINGS_HOURS_MINUS_Y2 = 136;
const int SETTINGS_HOURS_PLUS_X1 = 276;
const int SETTINGS_HOURS_PLUS_Y1 = 112;
const int SETTINGS_HOURS_PLUS_X2 = 316;
const int SETTINGS_HOURS_PLUS_Y2 = 136;

const int SETTINGS_MINUTES_MINUS_X1 = 125;
const int SETTINGS_MINUTES_MINUS_Y1 = 142;
const int SETTINGS_MINUTES_MINUS_X2 = 165;
const int SETTINGS_MINUTES_MINUS_Y2 = 167;
const int SETTINGS_MINUTES_PLUS_X1 = 275;
const int SETTINGS_MINUTES_PLUS_Y1 = 142;
const int SETTINGS_MINUTES_PLUS_X2 = 316;
const int SETTINGS_MINUTES_PLUS_Y2 = 167;

// Набор единиц.
const int SETTINGS_METRIC_X1 = 62;
const int SETTINGS_METRIC_Y1 = 177;
const int SETTINGS_METRIC_X2 = 160;
const int SETTINGS_METRIC_Y2 = 200;

const int SETTINGS_IMPERIAL_X1 = 168;
const int SETTINGS_IMPERIAL_Y1 = 177;
const int SETTINGS_IMPERIAL_X2 = 268;
const int SETTINGS_IMPERIAL_Y2 = 200;

// Cancel / Save.
const int SETTINGS_CANCEL_X1 = 62;
const int SETTINGS_CANCEL_Y1 = 212;
const int SETTINGS_CANCEL_X2 = 160;
const int SETTINGS_CANCEL_Y2 = 236;

const int SETTINGS_SAVE_X1 = 168;
const int SETTINGS_SAVE_Y1 = 212;
const int SETTINGS_SAVE_X2 = 268;
const int SETTINGS_SAVE_Y2 = 236;

// Индикаторы выбранных настроек: координаты центра кружка.
const int SETTINGS_THEME_LIGHT_DOT_X = 151;
const int SETTINGS_THEME_LIGHT_DOT_Y = 33;
const int SETTINGS_THEME_DARK_DOT_X = 229;
const int SETTINGS_THEME_DARK_DOT_Y = 33;
const int SETTINGS_THEME_AUTO_DOT_X = 304;
const int SETTINGS_THEME_AUTO_DOT_Y = 33;

const int SETTINGS_METRIC_DOT_X = 144;
const int SETTINGS_METRIC_DOT_Y = 186;
const int SETTINGS_IMPERIAL_DOT_X = 255;
const int SETTINGS_IMPERIAL_DOT_Y = 186;

const int SETTINGS_DOT_RADIUS = 4;

// Внутренняя рамка обратной связи для Cancel / Save.
const int SETTINGS_ACTION_FLASH_MS = 120;


// ============================================================
// ПРОТОТИПЫ ФУНКЦИЙ
// ============================================================

void showBootLogo();
const char* getMainBackgroundPath();
void drawMainBackground();
void drawSettingsBackground();
void captureSettingsBackgroundRegions();
void drawSettingsWheelValue(bool restoreBackground);
void drawSettingsMagnetsValue(bool restoreBackground);
void drawSettingsHoursValue(bool restoreBackground);
void drawSettingsMinutesValue(bool restoreBackground);
void drawSettingsSelectionIndicators();
void drawSettingsActionFeedback(int x1, int y1, int x2, int y2);
void captureMainBackgroundRegions();
void restoreMainBackgroundRegion(int x, int y, int width, int height);


void updateSpeedAndDistance();
void updateHallAbsoluteFilterLimit();
void updateTemperature(unsigned long nowMs);
void updateBattery(unsigned long nowMs);
void updateAmbientLight(unsigned long nowMs);
void maybeSaveOdometer();

int readLightRaw();
bool readIsDark();
void applyThemeColors(bool dark);
void forceFullRedraw();

void drawTopRow();
void drawBatteryIcon(int pct);
void syncRtcDateFromBuildDate();

bool readTouch(uint16_t &x, uint16_t &y);
void drawSettingsScreen();
void changeTheme(int newMode);
void beginClockEdit();
void saveSettings();
void discardSettingsAndReturnToMain();
void returnToMainScreen();
void handleSettingsTouch(uint16_t x, uint16_t y);
void handleTouch();

void drawBoldSpeedTo(TFT_eSprite &spr, const char *text, int rightX, int centerY);
void drawSpeed();

void drawBottomValues(unsigned long nowMs);

void checkForSdUsbRequest();
void enterSdUsbMode();
void sdUsbServiceLoop();
void freeGraphicsBuffers();
bool receiveFileToSd(const char* path, size_t fileSize);



// ============================================================
// SETUP
// ============================================================

void syncRtcDateFromBuildDate()
{
  const char* buildDate = __DATE__;
  const char* monthNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month = 0;

  for (int index = 0; index < 12; index++)
  {
    if (strncmp(buildDate, monthNames + index * 3, 3) == 0)
    {
      month = index + 1;
      break;
    }
  }

  int day = (buildDate[4] == ' ' ? buildDate[5] - '0' :
             (buildDate[4] - '0') * 10 + buildDate[5] - '0');
  int year = (buildDate[7] - '0') * 1000 + (buildDate[8] - '0') * 100 +
             (buildDate[9] - '0') * 10 + buildDate[10] - '0';

  if (month < 1 || day < 1 || day > 31 || year < 2000)
    return;

  DateTime now = rtc.now();
  rtc.adjust(DateTime(year, month, day, now.hour(), now.minute(), now.second()));
}

void setup()
{
  Serial.begin(115200);

  prefs.begin("bike", false);

  // ---------------- HALL ----------------
  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), onHallPulse, FALLING);

  // ---------------- BME280 ----------------
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  touchWire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
  touchWire.setClock(400000);

  bmeAvailable = bme.begin(BME280_ADDR);
  if (!bmeAvailable)
  {
    Serial.println("BME280 not found. Check I2C wiring and address.");
  }
  else
  {
    Serial.println("BME280 initialized");
    bme.setSampling(
      Adafruit_BME280::MODE_NORMAL,
      Adafruit_BME280::SAMPLING_X1,
      Adafruit_BME280::SAMPLING_X1,
      Adafruit_BME280::SAMPLING_X1,
      Adafruit_BME280::FILTER_OFF,
      Adafruit_BME280::STANDBY_MS_0_5
    );
  }

  // ---------------- Подсветка ----------------
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  // ---------------- I2C / RTC ----------------

  // Сброс CST820. INT не используем — опрашиваем тач программно.
  pinMode(TOUCH_RST_PIN, OUTPUT);
  digitalWrite(TOUCH_RST_PIN, LOW);
  delay(10);
  digitalWrite(TOUCH_RST_PIN, HIGH);
  delay(50);

  // Запрещаем автоматический уход CST820 в low-power.
  touchWire.beginTransmission(TOUCH_I2C_ADDR);
  touchWire.write(0xFE);
  touchWire.write(0xFF);
  touchWire.endTransmission();

  if (rtc.begin())
  {
    rtcAvailable = true;
    syncRtcDateFromBuildDate();

    if (rtc.lostPower())
    {
      Serial.println("RTC потерял питание — выставьте время отдельным скетчем!");
    }
  }
  else
  {
    Serial.println("DS3231 не найден — проверьте I2C_SDA_PIN / I2C_SCL_PIN");
  }

  // ---------------- Батарея / Фоторезистор ----------------
  analogReadResolution(12);

  // ---------------- NVS ----------------
  //prefs.clear(); // Очищает память
  odometerKm = prefs.getFloat("odom_km", 0.0);
  odometerAtLastSave = odometerKm;
  lastProcessedPulseCount = 0;

  wheelCircumferenceMm = prefs.getFloat("circ_mm", 2275.0);
  if (wheelCircumferenceMm < CIRCUMFERENCE_MIN_MM ||
      wheelCircumferenceMm > CIRCUMFERENCE_MAX_MM) {
    wheelCircumferenceMm = 2275.0;
  }

  magnetsPerRevolution = prefs.getInt("magnets", 1);
  if (magnetsPerRevolution < MAGNETS_MIN || magnetsPerRevolution > MAGNETS_MAX)
    magnetsPerRevolution = 1;

  updateHallAbsoluteFilterLimit();

  int savedTheme = prefs.getInt("theme", THEME_WHITE);
  if (savedTheme < THEME_WHITE || savedTheme > THEME_AUTO)
    savedTheme = THEME_WHITE;
  themeMode = (ThemeMode)savedTheme;

  int savedSpeedUnit = prefs.getInt(SPEED_UNIT_KEY, SPEED_KMH);
  if (savedSpeedUnit < SPEED_KMH || savedSpeedUnit > SPEED_MPH)
    savedSpeedUnit = SPEED_KMH;
  speedUnit = (SpeedUnit)savedSpeedUnit;

  int savedTemperatureUnit = prefs.getInt(TEMP_UNIT_KEY, TEMP_CELSIUS);
  if (savedTemperatureUnit < TEMP_CELSIUS || savedTemperatureUnit > TEMP_FAHRENHEIT)
    savedTemperatureUnit = TEMP_CELSIUS;
  temperatureUnit = (TemperatureUnit)savedTemperatureUnit;

  int savedPressureUnit = prefs.getInt(PRESSURE_UNIT_KEY, PRESSURE_HPA);
  if (savedPressureUnit < PRESSURE_HPA || savedPressureUnit > PRESSURE_INHG)
    savedPressureUnit = PRESSURE_HPA;
  pressureUnit = (PressureUnit)savedPressureUnit;

  // ---------------- TFT ----------------
  tft.init();
  tft.setRotation(1);

  // Буфер области спидометра.
  // ВАЖНО: спрайт не должен заходить на левый блок
  // avg.speed / pressure и max.speed / humidity,
  // иначе при смене скорости он стирает эти элементы.
  speedSprite.setColorDepth(16);
  speedSprite.createSprite(165, 70);

  backgroundTemp = (uint16_t*)malloc(120 * 72 * sizeof(uint16_t));
  backgroundClock = (uint16_t*)malloc(100 * 60 * sizeof(uint16_t));
  backgroundBattery = (uint16_t*)malloc(75 * 22 * sizeof(uint16_t));
  backgroundMetrics = (uint16_t*)malloc(118 * 126 * sizeof(uint16_t));
  backgroundTimer = (uint16_t*)malloc(102 * 32 * sizeof(uint16_t));
  backgroundTrip = (uint16_t*)malloc(103 * 32 * sizeof(uint16_t));
  backgroundOdometer = (uint16_t*)malloc(103 * 32 * sizeof(uint16_t));
  backgroundSpeed = (uint16_t*)malloc(165 * 70 * sizeof(uint16_t));
  backgroundTimerLabel = (uint16_t*)malloc(106 * 28 * sizeof(uint16_t));
  backgroundTripLabel = (uint16_t*)malloc(103 * 28 * sizeof(uint16_t));

  settingsWheelBg = (uint16_t*)malloc(SETTINGS_WHEEL_BG_W * SETTINGS_WHEEL_BG_H * sizeof(uint16_t));
  settingsMagnetsBg = (uint16_t*)malloc(SETTINGS_MAGNETS_BG_W * SETTINGS_MAGNETS_BG_H * sizeof(uint16_t));
  settingsHoursBg = (uint16_t*)malloc(SETTINGS_HOURS_BG_W * SETTINGS_HOURS_BG_H * sizeof(uint16_t));
  settingsMinutesBg = (uint16_t*)malloc(SETTINGS_MINUTES_BG_W * SETTINGS_MINUTES_BG_H * sizeof(uint16_t));

  if (!backgroundTemp || !backgroundClock || !backgroundBattery || !backgroundMetrics ||
      !backgroundTimer || !backgroundTrip || !backgroundOdometer || !backgroundSpeed ||
      !backgroundTimerLabel || !backgroundTripLabel)
    Serial.println("WARNING: one or more main background buffers failed to allocate");
  if (!settingsWheelBg || !settingsMagnetsBg || !settingsHoursBg || !settingsMinutesBg)
    Serial.println("WARNING: one or more Settings background buffers failed to allocate");

  showBootLogo();

  // Определяем начальную тему ДО первой отрисовки, чтобы не было
  // "вспышки" неправильным фоном при старте в темноте.
  if (themeMode == THEME_AUTO)
    isDarkTheme = readIsDark();
  else
    isDarkTheme = (themeMode == THEME_DARK);

  applyThemeColors(isDarkTheme);

  drawMainBackground();
  captureMainBackgroundRegions();

}


// ============================================================
// ЗАСТАВКА
// ============================================================

void showBootLogo()
{
  tft.fillScreen(TFT_BLACK);

  bool sdOk = SD.begin(SD_CS_PIN);
  sdAvailable = sdOk;

  if (!sdOk)
  {
    Serial.println("SD-карта не найдена — проверьте SD_CS_PIN и подключение!");
    delay(BOOT_LOGO_MS);
    return;
  }

  if (!SD.exists(BOOT_IMAGE_PATH))
  {
    Serial.println("Файл заставки не найден на SD-карте!");
    delay(BOOT_LOGO_MS);
    return;
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);

  TJpgDec.drawSdJpg(0, 0, BOOT_IMAGE_PATH);

  delay(BOOT_LOGO_MS);
}

const char* getMainBackgroundPath()
{
  if (speedUnit == SPEED_MPH)
    return isDarkTheme ? DARK_MPH_BACKGROUND_PATH : LIGHT_MPH_BACKGROUND_PATH;

  return isDarkTheme ? DARK_BACKGROUND_PATH : LIGHT_BACKGROUND_PATH;
}

void drawMainBackground()
{
  tft.fillScreen(colBg);

  if (!sdAvailable)
    return;

  const char* backgroundPath = getMainBackgroundPath();

  if (!SD.exists(backgroundPath))
    return;

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);
  TJpgDec.drawSdJpg(0, 0, backgroundPath);
}

void drawSettingsBackground()
{
  tft.fillScreen(colBg);

  if (!sdAvailable || !SD.exists(SETTINGS_BACKGROUND_PATH))
    return;

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);
  TJpgDec.drawSdJpg(0, 0, SETTINGS_BACKGROUND_PATH);
}

void captureSettingsBackgroundRegions()
{
  if (!sdAvailable || !SD.exists(SETTINGS_BACKGROUND_PATH))
    return;

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(settingsBackgroundBufferOutput);
  TJpgDec.drawSdJpg(0, 0, SETTINGS_BACKGROUND_PATH);
  TJpgDec.setCallback(tftOutput);
}

void captureMainBackgroundRegions()
{
  if (!sdAvailable)
    return;

  const char* backgroundPath = getMainBackgroundPath();

  if (!SD.exists(backgroundPath))
    return;

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(backgroundBufferOutput);
  TJpgDec.drawSdJpg(0, 0, backgroundPath);
  TJpgDec.setCallback(tftOutput);
}

void restoreMainBackgroundRegion(int x, int y, int width, int height)
{
  if (x == 0 && y == 0 && width == 120 && height == 72 && backgroundTemp)
    tft.pushImage(x, y, width, height, backgroundTemp);
  else if (x == 112 && y == 0 && width == 100 && height == 60 && backgroundClock)
    tft.pushImage(x, y, width, height, backgroundClock);
  else if (x == 240 && y == BATTERY_Y - 12 && width == 75 && height == 22 && backgroundBattery)
    tft.pushImage(x, y, width, height, backgroundBattery);
  else if (x == 0 && y == 60 && width == 118 && height == 126 && backgroundMetrics)
    tft.pushImage(x, y, width, height, backgroundMetrics);
  else if (x == 2 && y == VALUE_Y - 16 && width == DIV1_X - 4 && height == 32 && backgroundTimer)
    tft.pushImage(x, y, width, height, backgroundTimer);
  else if (x == DIV1_X + 2 && y == VALUE_Y - 16 && width == DIV2_X - DIV1_X - 4 && height == 32 && backgroundTrip)
    tft.pushImage(x, y, width, height, backgroundTrip);
  else if (x == DIV2_X + 2 && y == VALUE_Y - 16 && width == 320 - DIV2_X - 4 && height == 32 && backgroundOdometer)
    tft.pushImage(x, y, width, height, backgroundOdometer);
  else if (x == 138 && y == 80 && width == 165 && height == 70 && backgroundSpeed)
    tft.pushImage(x, y, width, height, backgroundSpeed);
  else if (x == 0 && y == LABEL_Y - 18 && width == DIV1_X && height == 28 && backgroundTimerLabel)
    tft.pushImage(x, y, width, height, backgroundTimerLabel);
  else if (x == DIV1_X + 2 && y == LABEL_Y - 18 && width == DIV2_X - DIV1_X - 4 && height == 28 && backgroundTripLabel)
    tft.pushImage(x, y, width, height, backgroundTripLabel);
}


// ============================================================
// USB / SERIAL -> SD SERVICE MODE
// ============================================================

void freeGraphicsBuffers()
{
  speedSprite.deleteSprite();

  uint16_t** buffers[] = {
    &backgroundTemp, &backgroundClock, &backgroundBattery, &backgroundMetrics,
    &backgroundTimer, &backgroundTrip, &backgroundOdometer, &backgroundSpeed,
    &backgroundTimerLabel, &backgroundTripLabel,
    &settingsWheelBg, &settingsMagnetsBg, &settingsHoursBg, &settingsMinutesBg
  };

  for (uint16_t** slot : buffers)
  {
    if (*slot)
    {
      free(*slot);
      *slot = nullptr;
    }
  }
}

void enterSdUsbMode()
{
  sdUsbMode = true;

  // В сервисном режиме нам не нужны измерения и крупные буферы экрана.
  detachInterrupt(digitalPinToInterrupt(HALL_PIN));
  freeGraphicsBuffers();

  if (!sdAvailable)
    sdAvailable = SD.begin(SD_CS_PIN);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString("SD USB UPDATE", 160, 100);
  tft.drawString(sdAvailable ? "READY" : "SD ERROR", 160, 130);

  // Убираем случайно накопившиеся символы после команды входа.
  while (Serial.available())
    Serial.read();

  if (sdAvailable)
    Serial.println("SDUP READY");
  else
    Serial.println("SDUP ERROR SD");
}

void checkForSdUsbRequest()
{
  static char command[16];
  static size_t len = 0;

  while (Serial.available() && !sdUsbMode)
  {
    char c = (char)Serial.read();

    if (c == '\r')
      continue;

    if (c == '\n')
    {
      command[len] = '\0';
      if (strcmp(command, "SDUP") == 0)
      {
        len = 0;
        enterSdUsbMode();
        return;
      }
      len = 0;
      continue;
    }

    if (len < sizeof(command) - 1)
      command[len++] = c;
    else
      len = 0;
  }
}

bool receiveFileToSd(const char* path, size_t fileSize)
{
  if (!sdAvailable || !path || path[0] != '/' || strstr(path, ".."))
    return false;

  if (SD.exists(SD_USB_TEMP_PATH))
    SD.remove(SD_USB_TEMP_PATH);

  File file = SD.open(SD_USB_TEMP_PATH, FILE_WRITE);
  if (!file)
    return false;

  uint8_t buffer[SD_USB_BLOCK_SIZE];
  size_t received = 0;
  unsigned long lastDataMs = millis();

  while (received < fileSize)
  {
    int available = Serial.available();
    if (available <= 0)
    {
      if (millis() - lastDataMs > SD_USB_RX_TIMEOUT_MS)
      {
        file.close();
        SD.remove(SD_USB_TEMP_PATH);
        return false;
      }
      delay(1);
      continue;
    }

    size_t remaining = fileSize - received;
    size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    if ((size_t)available < chunk)
      chunk = (size_t)available;

    size_t got = Serial.readBytes(buffer, chunk);
    if (got == 0)
      continue;

    if (file.write(buffer, got) != got)
    {
      file.close();
      SD.remove(SD_USB_TEMP_PATH);
      return false;
    }

    received += got;
    lastDataMs = millis();
  }

  file.flush();
  file.close();

  // Только после полного получения удаляем старый файл.
  if (SD.exists(path) && !SD.remove(path))
  {
    SD.remove(SD_USB_TEMP_PATH);
    return false;
  }

  if (!SD.rename(SD_USB_TEMP_PATH, path))
  {
    SD.remove(SD_USB_TEMP_PATH);
    return false;
  }

  return true;
}

void sdUsbServiceLoop()
{
  static char line[128];
  static size_t len = 0;

  while (Serial.available())
  {
    char c = (char)Serial.read();

    if (c == '\r')
      continue;

    if (c != '\n')
    {
      if (len < sizeof(line) - 1)
        line[len++] = c;
      else
        len = 0;
      continue;
    }

    line[len] = '\0';
    len = 0;

    if (strcmp(line, "PING") == 0)
    {
      Serial.println("PONG");
      continue;
    }

    if (strcmp(line, "REBOOT") == 0)
    {
      Serial.println("REBOOTING");
      Serial.flush();
      delay(100);
      ESP.restart();
    }

    if (strncmp(line, "PUT ", 4) == 0)
    {
      char* sizeText = line + 4;
      char* separator = strchr(sizeText, ' ');
      if (!separator)
      {
        Serial.println("ERR COMMAND");
        continue;
      }

      *separator = '\0';
      const char* path = separator + 1;
      unsigned long fileSize = strtoul(sizeText, nullptr, 10);

      if (fileSize == 0 || path[0] != '/' || strstr(path, ".."))
      {
        Serial.println("ERR PARAM");
        continue;
      }

      Serial.println("SEND");
      Serial.flush();

      if (receiveFileToSd(path, (size_t)fileSize))
      {
        Serial.print("OK ");
        Serial.println(fileSize);
      }
      else
      {
        Serial.println("ERR WRITE");
      }
      return;
    }

    Serial.println("ERR COMMAND");
  }
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  if (sdUsbMode)
  {
    sdUsbServiceLoop();
    delay(1);
    return;
  }

  checkForSdUsbRequest();
  if (sdUsbMode)
    return;

  unsigned long nowMs = millis();

  updateSpeedAndDistance();
  updateTemperature(nowMs);
  updateBattery(nowMs);
  updateAmbientLight(nowMs);
  maybeSaveOdometer();

  handleTouch();

  if (!settingsScreen)
  {
    drawTopRow();
    drawSpeed();
    drawBottomValues(nowMs);
  }
}


// ============================================================
// HALL — СКОРОСТЬ + ПРОБЕГ
// ============================================================

void updateSpeedAndDistance()
{
  unsigned long count;
  unsigned long lastPulse;
  unsigned long interval;

  noInterrupts();
  count = pulseCount;
  lastPulse = lastPulseMicros;
  interval = pulseIntervalMicros;
  interrupts();

  float distancePerPulseMm = wheelCircumferenceMm / magnetsPerRevolution;

  if (count != lastProcessedPulseCount)
  {
    unsigned long newPulses = count - lastProcessedPulseCount;
    float addedKm = (newPulses * distancePerPulseMm) / 1000000.0;

    trip1Km += addedKm;
    trip2Km += addedKm;

    odometerKm += addedKm;
    lastProcessedPulseCount = count;
  }

  if (lastPulse == 0)
  {
    speedKmh = 0.0;
    hallMinIntervalUs = hallAbsoluteMinIntervalUs;
    return;
  }

  unsigned long sinceLastPulseMs = (micros() - lastPulse) / 1000;

  if (sinceLastPulseMs > SPEED_TIMEOUT_MS)
  {
    speedKmh = 0.0;

    // Сбрасываем метку последнего импульса: иначе первый импульс
    // после долгой остановки посчитает интервал от импульса ДО
    // остановки и даст заниженную (почти нулевую) скорость на кадр.
    // Проверка lastPulseMicros == lastPulse защищает от гонки,
    // если импульс успел прийти между чтением выше и этим сбросом.
    noInterrupts();
    if (lastPulseMicros == lastPulse)
    {
      lastPulseMicros = 0;
      pulseIntervalMicros = 0;
      hallMinIntervalUs = hallAbsoluteMinIntervalUs;
    }
    interrupts();

    return;
  }

  if (interval == 0)
  {
    speedKmh = 0.0;
    return;
  }

  float intervalSec = interval / 1000000.0;
  float distancePerPulseM = distancePerPulseMm / 1000.0;
  float speedMs = distancePerPulseM / intervalSec;

  speedKmh = speedMs * 3.6;

  // Рассчитать порог для следующего импульса вне ISR.
  float allowedNextSpeedKmh = speedKmh + MAX_SPEED_JUMP_KMH;
  if (allowedNextSpeedKmh > MAX_PLAUSIBLE_SPEED_KMH)
    allowedNextSpeedKmh = MAX_PLAUSIBLE_SPEED_KMH;

  unsigned long dynamicMinIntervalUs = (unsigned long)(
      (distancePerPulseMm * 3600.0f) / allowedNextSpeedKmh);
  if (dynamicMinIntervalUs < hallAbsoluteMinIntervalUs)
    dynamicMinIntervalUs = hallAbsoluteMinIntervalUs;

  hallMinIntervalUs = dynamicMinIntervalUs;

  if (speedKmh > trip1MaxSpeedKmh)
    trip1MaxSpeedKmh = speedKmh;
  if (speedKmh > trip2MaxSpeedKmh)
    trip2MaxSpeedKmh = speedKmh;
}

// ============================================================
// ТЕМПЕРАТУРА (без изменений)
// ============================================================

void updateTemperature(unsigned long nowMs)
{
  if (nowMs - lastTempRequest < TEMP_UPDATE_MS && lastTempRequest != 0)
    return;

  lastTempRequest = nowMs;

  if (!bmeAvailable)
  {
    currentTempC = NAN;
    currentPressurePa = 0.0f;
    currentHumidityPercent = 0.0f;
    return;
  }

  currentTempC = bme.readTemperature();
  currentPressurePa = bme.readPressure();
  currentHumidityPercent = bme.readHumidity();

  if (isnan(currentTempC))
  {
    Serial.println("BME280 read failed");
  }
}


// ============================================================
// БАТАРЕЯ
// ============================================================

void updateBattery(unsigned long nowMs)
{
  if (nowMs - lastBatteryRead < BATTERY_UPDATE_MS && lastBatteryRead != 0)
  {
    return;
  }

  lastBatteryRead = nowMs;

  long sum = 0;

  for (int i = 0; i < BATTERY_SAMPLES; i++)
  {
    sum += analogRead(BATTERY_PIN);
    delay(2);
  }

  float raw = sum / (float)BATTERY_SAMPLES;
  float vAdc = (raw / 4095.0) * 3.3;
  float vBat = vAdc * BATTERY_DIVIDER_RATIO;
  batteryVoltageV = vBat;

  float pct = (vBat - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0;

  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  batteryPercent = (int)(pct + 0.5);

  // Вывод значения GPIO39 в Serial Monitor
  Serial.print("GPIO39 raw: ");
  Serial.print(raw);
  Serial.print(" | Vadc: ");
  Serial.print(vAdc, 3);
  Serial.print(" V | Vbat: ");
  Serial.print(vBat, 3);
  Serial.print(" V | Battery: ");
  Serial.print(batteryPercent);
  Serial.println(" %");
}


// ============================================================
// ФОТОРЕЗИСТОР / АВТО-ТЕМА
// ============================================================

// Усреднённое сырое значение ADC с фоторезистора.
int readLightRaw()
{
  long sum = 0;

  for (int i = 0; i < LIGHT_SAMPLES; i++)
  {
    sum += analogRead(LDR_PIN);
  }

  return sum / LIGHT_SAMPLES;
}

// Разовое определение "темно сейчас или нет" — используется в setup(),
// без гистерезиса (гистерезис нужен только чтобы не мигало в loop()).
bool readIsDark()
{
  int raw = readLightRaw();
  return raw > LIGHT_THRESHOLD_DARK;
}

// Применяет цвета выбранной темы (не трогает экран, только переменные).
void applyThemeColors(bool dark)
{
  if (dark)
  {
    colBg   = TFT_BLACK;
    colText = TFT_WHITE;
  }
  else
  {
    colBg   = TFT_WHITE;
    colText = TFT_BLACK;
  }
}

// Сбрасывает все "последние отрисованные" значения, чтобы на
// следующем кадре все элементы перерисовались с новыми цветами.
void forceFullRedraw()
{
  lastDrawnSpeed = -1.0;
  lastDrawnAvgSpeed = -1.0;
  lastDrawnMaxSpeed = -1.0;
  lastDrawnTrip = -1.0;
  lastDrawnOdo = -1.0;
  lastDrawnTemp = -999.0;
  lastDrawnPressure = NAN;
  lastDrawnHumidity = NAN;
  lastDrawnBatteryPct = -999;
  lastDrawnClockH = -1;
  lastDrawnClockM = -1;
  lastDrawnDateDay = -1;
  lastDrawnDateMonth = -1;
  lastDrawnDateYear = -1;
  lastDrawnTimerTotalSec = -1;
  lastDrawnTimerLabel[0] = '\0';
  lastDrawnTripLabel[0] = '\0';
  lastDrawnAvgLabel[0] = '\0';
  lastDrawnMaxLabel[0] = '\0';
  lastDrawnAvgMetricValue = NAN;
  lastDrawnMaxMetricValue = NAN;
}

// Периодическая проверка освещённости с гистерезисом.
void updateAmbientLight(unsigned long nowMs)
{
  if (themeMode != THEME_AUTO)
    return;

  if (nowMs - lastLightRead < LIGHT_UPDATE_MS && lastLightRead != 0)
  {
    return;
  }

  lastLightRead = nowMs;

  int raw = readLightRaw();

  Serial.print("LDR raw: ");
  Serial.println(raw);

  bool shouldBeDark = isDarkTheme;

  if (!isDarkTheme && raw > LIGHT_THRESHOLD_DARK)
  {
    shouldBeDark = true;
  }
  else if (isDarkTheme && raw < LIGHT_THRESHOLD_LIGHT)
  {
    shouldBeDark = false;
  }

  if (shouldBeDark != isDarkTheme)
  {
    isDarkTheme = shouldBeDark;
    applyThemeColors(isDarkTheme);

    if (settingsScreen)
    {
      // На Settings фон не перерисовываем. Меняется только состояние темы.
    }
    else
    {
      drawMainBackground();
      captureMainBackgroundRegions();
        }
    forceFullRedraw();
  }
}


// ============================================================
// СОХРАНЕНИЕ ОДОМЕТРА (без изменений)
// ============================================================

void maybeSaveOdometer()
{
  float distanceSinceSave = odometerKm - odometerAtLastSave;

  if (distanceSinceSave >= ODOMETER_SAVE_STEP_KM)
  {
    prefs.putFloat("odom_km", odometerKm);
    odometerAtLastSave = odometerKm;
  }
}


// ============================================================
// ВЕРХНЯЯ СТРОКА: температура | часы | батарея
// ============================================================

void drawTopRow()
{
  tft.setFreeFont(&Conthrax_SB_15);
  tft.setTextColor(colText);
  tft.setTextSize(1);

  // -------------------- Температура / давление / влажность --------------------
  float displayedTemp = !isnan(currentTempC)
    ? (temperatureUnit == TEMP_FAHRENHEIT ? currentTempC * 9.0f / 5.0f + 32.0f : currentTempC)
    : NAN;

  float displayedPressure = NAN;
  if (bmeAvailable && currentPressurePa > 0.0f)
  {
    if (pressureUnit == PRESSURE_MMHG)
      displayedPressure = currentPressurePa / 133.322f;
    else if (pressureUnit == PRESSURE_INHG)
      displayedPressure = currentPressurePa / 3386.389f;
    else
      displayedPressure = currentPressurePa / 100.0f;
  }

  float displayedHumidity = bmeAvailable && currentHumidityPercent >= 0.0f
    ? currentHumidityPercent
    : NAN;

  const bool tempDirty = !isnan(displayedTemp) && (isnan(lastDrawnTemp) || abs(displayedTemp - lastDrawnTemp) >= 0.1f);
  const float pressureRedrawThreshold = (pressureUnit == PRESSURE_INHG) ? 0.01f : 1.0f;
  const bool pressureDirty = !isnan(displayedPressure) && (isnan(lastDrawnPressure) || abs(displayedPressure - lastDrawnPressure) >= pressureRedrawThreshold);
  const bool humidityDirty = !isnan(displayedHumidity) && (isnan(lastDrawnHumidity) || abs(displayedHumidity - lastDrawnHumidity) >= 1.0f);

  if (tempDirty || pressureDirty || humidityDirty)
  {
    if (!isnan(displayedTemp))
      lastDrawnTemp = displayedTemp;
    if (!isnan(displayedPressure))
      lastDrawnPressure = displayedPressure;
    if (!isnan(displayedHumidity))
      lastDrawnHumidity = displayedHumidity;

    // Координаты очистки температуры / давления / влажности: X=0, Y=0, ширина=120, высота=72.
    restoreMainBackgroundRegion(0, 0, 120, 72);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(colText);

    if (!isnan(displayedTemp))
    {
      char buf[10];
      if (temperatureUnit == TEMP_FAHRENHEIT && displayedTemp > 100.0f)
        snprintf(buf, sizeof(buf), "%.0f", displayedTemp);
      else
        snprintf(buf, sizeof(buf), "%.1f", displayedTemp);

      tft.setFreeFont(&Conthrax_SB_15);
tft.setTextSize(1);

// Фиксированная позиция знака градуса.
// degreeX — центр кружка °
const int degreeX = 84;
const int degreeY = TEMP_Y - 3;

// Значение температуры всегда заканчивается
// за 5 px до знака градуса.
const int tempRightX = degreeX - 5;

// Температуру привязываем правым краем.
tft.setTextDatum(MR_DATUM);
// Координаты вывода температуры: правый край X=tempRightX (degreeX - 5), Y=TEMP_Y.
tft.drawString(buf, tempRightX, TEMP_Y);

// Знак ° и C/F всегда стоят на одном месте.
tft.drawCircle(degreeX, degreeY, 2, colText);

tft.setTextDatum(ML_DATUM);
tft.drawString(
    temperatureUnit == TEMP_FAHRENHEIT ? "F" : "C",
    degreeX + 5,
    TEMP_Y
);
    }

    if (!isnan(displayedPressure))
    {
      tft.setTextFont(2);
      tft.setTextDatum(ML_DATUM);
      char pressureBuf[12];
      if (pressureUnit == PRESSURE_INHG)
        snprintf(pressureBuf, sizeof(pressureBuf), "%.1f", displayedPressure);
      else
        snprintf(pressureBuf, sizeof(pressureBuf), "%.0f", displayedPressure);
      // Координаты вывода давления: X=32, Y=42.
      tft.drawString(pressureBuf, 32, 42);
    }

    if (!isnan(displayedHumidity))
    {
      tft.setTextFont(2);
      tft.setTextDatum(ML_DATUM);
      char humidityBuf[8];
      snprintf(humidityBuf, sizeof(humidityBuf), "%.0f", displayedHumidity);
      // Координаты вывода влажности: X=86, Y=42.
      tft.drawString(humidityBuf, 86, 42);
    }
  }

  // -------------------- Часы (DS3231) --------------------
  int clockH = -1;
  int clockM = -1;
  int dateDay = -1;
  int dateMonth = -1;
  int dateYear = -1;

  if (rtcAvailable)
  {
    DateTime now = rtc.now();
    clockH = now.hour();
    clockM = now.minute();
    dateDay = now.day();
    dateMonth = now.month();
    dateYear = now.year();
  }

  if (clockH != lastDrawnClockH || clockM != lastDrawnClockM ||
      dateDay != lastDrawnDateDay || dateMonth != lastDrawnDateMonth ||
      dateYear != lastDrawnDateYear)
  {
    lastDrawnClockH = clockH;
    lastDrawnClockM = clockM;
    lastDrawnDateDay = dateDay;
    lastDrawnDateMonth = dateMonth;
    lastDrawnDateYear = dateYear;

    // Координаты очистки времени и даты: X=112, Y=0, ширина=100, высота=60.
    restoreMainBackgroundRegion(112, 0, 100, 60);
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&Conthrax_SB_15);

    char buf[8];

    if (rtcAvailable)
    {
      snprintf(buf, sizeof(buf), "%02d:%02d", clockH, clockM);
    }
    else
    {
      snprintf(buf, sizeof(buf), "--:--");
    }

    // Координаты вывода времени: X=CLOCK_X, Y=CLOCK_Y.
    tft.drawString(buf, CLOCK_X, CLOCK_Y);

    tft.setTextFont(2);
    tft.setTextSize(1);
    char dateBuf[11];
    if (rtcAvailable)
      snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%04d", dateDay, dateMonth, dateYear);
    else
      snprintf(dateBuf, sizeof(dateBuf), "--.--.----");
    // Координаты вывода даты: X=DATE_X, Y=DATE_Y.
    tft.drawString(dateBuf, DATE_X, DATE_Y);
  }

  // -------------------- Батарея --------------------
  // Переключение % <-> вольты (см. handleTouch) само выставляет
  // lastDrawnBatteryPct = -999, так что достаточно сверять только pct.
  if (batteryPercent != lastDrawnBatteryPct)
  {
    lastDrawnBatteryPct = batteryPercent;
    drawBatteryIcon(batteryPercent);
  }

}


// ============================================================
// ИКОНКА БАТАРЕИ
// ============================================================

void drawBatteryIcon(int pct)
{
  // Восстанавливаем фон всей зоны батареи: значение + 5 индикаторов.
  // Это также стирает лишние сегменты, когда уровень заряда уменьшается.
  // Координаты очистки значения батареи и прямоугольников уровня заряда: X=240, Y=BATTERY_Y-12, ширина=75, высота=22.
  restoreMainBackgroundRegion(240, BATTERY_Y - 12, 75, 22);

  if (pct < 0)
    return;

  // -------------------- ПРОЦЕНТ / НАПРЯЖЕНИЕ --------------------
  tft.setTextFont(2);
  tft.setTextColor(colText);
  tft.setTextDatum(MR_DATUM);
  tft.setTextSize(1);

  char buf[8];
  if (batteryShowVoltage)
    snprintf(buf, sizeof(buf), "%.2fV", batteryVoltageV);
  else
    snprintf(buf, sizeof(buf), "%d%%", pct);

  // Координата значения оставлена без изменений.
  tft.drawString(buf, BATT_ICON_RIGHT_X - BATT_ICON_W - BATT_NUB_W - 4, BATTERY_Y);

  // -------------------- ИНДИКАТОР УРОВНЯ ЗАРЯДА --------------------
  // Все координаты — левый верхний угол прямоугольника.
  // Каждый сегмент: 4x8 px, Y = 12. Между сегментами 1 px.
  int segments = 0;
  if (pct >= 80)      segments = 5;  // 80–100%
  else if (pct >= 60) segments = 4;  // 60–79%
  else if (pct >= 40) segments = 3;  // 40–59%
  else if (pct >= 20) segments = 2;  // 20–39%
  else if (pct >= 10) segments = 1;  // 10–19%
  // 0–9%: сегментов нет

  for (int i = 0; i < segments; i++)
  {
    // Координаты вывода прямоугольников уровня заряда: X=286,291,296,301,306; Y=12; размер каждого 4x8 px.
    const int x = 286 + i * 5;   // 286, 291, 296, 301, 306
    tft.fillRect(x, 12, 4, 8, TFT_GREEN);
  }
}


// ============================================================
// TOUCH CST820
// ============================================================

bool readTouch(uint16_t &x, uint16_t &y)
{
  touchWire.beginTransmission(TOUCH_I2C_ADDR);
  touchWire.write(0x02);
  if (touchWire.endTransmission(false) != 0)
    return false;

  if (touchWire.requestFrom((int)TOUCH_I2C_ADDR, 5) != 5)
    return false;

  uint8_t fingers = touchWire.read();
  uint8_t xh = touchWire.read();
  uint8_t xl = touchWire.read();
  uint8_t yh = touchWire.read();
  uint8_t yl = touchWire.read();

  if (fingers == 0)
    return false;

  uint16_t rawX = ((xh & 0x0F) << 8) | xl;
  uint16_t rawY = ((yh & 0x0F) << 8) | yl;

  // Портретный диапазон CST820: rawX 0..239, rawY 0..319.
  // Значения за пределами — шум/битые данные, а не край экрана,
  // поэтому такое касание отбрасываем целиком, а не подставляем 0.
  if (rawX >= 240 || rawY >= 320)
    return false;

  // CST820 установлен как портретный 240x320.
  // TFT установлен Rotation 1 -> 320x240:
  // X = rawY, Y = 239 - rawX.
  x = rawY;
  y = 239 - rawX;

  return true;
}

static void prepareSettingsValueText()
{
  tft.setFreeFont(&Conthrax_SB_15);
  tft.setTextColor(TFT_BLACK);  // Settings: значения всегда чёрные, независимо от темы
  tft.setTextDatum(BR_DATUM);
  tft.setTextSize(1);
}

void drawSettingsWheelValue(bool restoreBackground)
{
  if (restoreBackground && settingsWheelBg)
    tft.pushImage(SETTINGS_WHEEL_BG_X, SETTINGS_WHEEL_BG_Y,
                  SETTINGS_WHEEL_BG_W, SETTINGS_WHEEL_BG_H, settingsWheelBg);

  prepareSettingsValueText();
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f", wheelCircumferenceMm);
  tft.drawString(buf, SETTINGS_WHEEL_X, SETTINGS_WHEEL_Y);
}

void drawSettingsMagnetsValue(bool restoreBackground)
{
  if (restoreBackground && settingsMagnetsBg)
    tft.pushImage(SETTINGS_MAGNETS_BG_X, SETTINGS_MAGNETS_BG_Y,
                  SETTINGS_MAGNETS_BG_W, SETTINGS_MAGNETS_BG_H, settingsMagnetsBg);

  prepareSettingsValueText();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", magnetsPerRevolution);
  tft.drawString(buf, SETTINGS_MAGNETS_X, SETTINGS_MAGNETS_Y);
}

void drawSettingsHoursValue(bool restoreBackground)
{
  if (restoreBackground && settingsHoursBg)
    tft.pushImage(SETTINGS_HOURS_BG_X, SETTINGS_HOURS_BG_Y,
                  SETTINGS_HOURS_BG_W, SETTINGS_HOURS_BG_H, settingsHoursBg);

  prepareSettingsValueText();
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d", clockSetHour);
  tft.drawString(buf, SETTINGS_HOURS_X, SETTINGS_HOURS_Y);
}

void drawSettingsMinutesValue(bool restoreBackground)
{
  if (restoreBackground && settingsMinutesBg)
    tft.pushImage(SETTINGS_MINUTES_BG_X, SETTINGS_MINUTES_BG_Y,
                  SETTINGS_MINUTES_BG_W, SETTINGS_MINUTES_BG_H, settingsMinutesBg);

  prepareSettingsValueText();
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d", clockSetMinute);
  tft.drawString(buf, SETTINGS_MINUTES_X, SETTINGS_MINUTES_Y);
}


void drawSettingsSelectionIndicators()
{
  // Сначала убираем все возможные старые кружки маленькими заливками
  // цветом фона Settings. Затем рисуем только активные.
  // Координаты кружков находятся в белых областях фона settings.jpg.
  tft.fillCircle(SETTINGS_THEME_LIGHT_DOT_X, SETTINGS_THEME_LIGHT_DOT_Y,
                 SETTINGS_DOT_RADIUS, TFT_WHITE);
  tft.fillCircle(SETTINGS_THEME_DARK_DOT_X, SETTINGS_THEME_DARK_DOT_Y,
                 SETTINGS_DOT_RADIUS, TFT_WHITE);
  tft.fillCircle(SETTINGS_THEME_AUTO_DOT_X, SETTINGS_THEME_AUTO_DOT_Y,
                 SETTINGS_DOT_RADIUS, TFT_WHITE);

  tft.fillCircle(SETTINGS_METRIC_DOT_X, SETTINGS_METRIC_DOT_Y,
                 SETTINGS_DOT_RADIUS, TFT_WHITE);
  tft.fillCircle(SETTINGS_IMPERIAL_DOT_X, SETTINGS_IMPERIAL_DOT_Y,
                 SETTINGS_DOT_RADIUS, TFT_WHITE);

  if (themeMode == THEME_WHITE)
    tft.fillCircle(SETTINGS_THEME_LIGHT_DOT_X, SETTINGS_THEME_LIGHT_DOT_Y,
                   SETTINGS_DOT_RADIUS, TFT_ORANGE);
  else if (themeMode == THEME_DARK)
    tft.fillCircle(SETTINGS_THEME_DARK_DOT_X, SETTINGS_THEME_DARK_DOT_Y,
                   SETTINGS_DOT_RADIUS, TFT_ORANGE);
  else
    tft.fillCircle(SETTINGS_THEME_AUTO_DOT_X, SETTINGS_THEME_AUTO_DOT_Y,
                   SETTINGS_DOT_RADIUS, TFT_ORANGE);

  if (speedUnit == SPEED_MPH)
    tft.fillCircle(SETTINGS_IMPERIAL_DOT_X, SETTINGS_IMPERIAL_DOT_Y,
                   SETTINGS_DOT_RADIUS, TFT_ORANGE);
  else
    tft.fillCircle(SETTINGS_METRIC_DOT_X, SETTINGS_METRIC_DOT_Y,
                   SETTINGS_DOT_RADIUS, TFT_ORANGE);
}

void drawSettingsActionFeedback(int x, int y, int w, int h)
{
  // Рамка толщиной 3 px, внутри ничего не закрашиваем.
  tft.drawRect(x,     y,     w,     h,     TFT_ORANGE);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_ORANGE);
  tft.drawRect(x + 2, y + 2, w - 4, h - 4, TFT_ORANGE);

  delay(SETTINGS_ACTION_FLASH_MS);
}

void drawSettingsScreen()
{
  // Полный фон Settings рисуется только при входе на экран.
  drawSettingsBackground();

  // Один раз сохраняем чистые участки под четырьмя изменяемыми значениями.
  captureSettingsBackgroundRegions();

  drawSettingsWheelValue(false);
  drawSettingsMagnetsValue(false);
  drawSettingsHoursValue(false);
  drawSettingsMinutesValue(false);

  drawSettingsSelectionIndicators();
}

void changeTheme(int newMode)
{
  themeMode = (ThemeMode)newMode;

  if (themeMode == THEME_AUTO)
    isDarkTheme = readIsDark();
  else
    isDarkTheme = (themeMode == THEME_DARK);

  applyThemeColors(isDarkTheme);
}

void beginClockEdit()
{
  clockTimeChanged = false;
  if (rtcAvailable)
  {
    DateTime now = rtc.now();
    clockSetHour = now.hour();
    clockSetMinute = now.minute();
  }
  else
  {
    clockSetHour = 0;
    clockSetMinute = 0;
  }
}

void saveSettings()
{
  prefs.putInt("theme", (int)themeMode);
  prefs.putFloat("circ_mm", wheelCircumferenceMm);
  prefs.putInt("magnets", magnetsPerRevolution);
  prefs.putInt(SPEED_UNIT_KEY, (int)speedUnit);
  prefs.putInt(TEMP_UNIT_KEY, (int)temperatureUnit);
  prefs.putInt(PRESSURE_UNIT_KEY, (int)pressureUnit);

  if (rtcAvailable && clockTimeChanged)
  {
    DateTime now = rtc.now();
    rtc.adjust(DateTime(now.year(), now.month(), now.day(), clockSetHour, clockSetMinute, 0));
  }

  returnToMainScreen();
}

void returnToMainScreen()
{
  settingsScreen = false;
  drawMainBackground();
  captureMainBackgroundRegions();
  forceFullRedraw();
}

void discardSettingsAndReturnToMain()
{
  int savedTheme = prefs.getInt("theme", THEME_WHITE);
  if (savedTheme < THEME_WHITE || savedTheme > THEME_AUTO)
    savedTheme = THEME_WHITE;

  themeMode = (ThemeMode)savedTheme;
  wheelCircumferenceMm = prefs.getFloat("circ_mm", wheelCircumferenceMm);
  magnetsPerRevolution = prefs.getInt("magnets", magnetsPerRevolution);
  updateHallAbsoluteFilterLimit();
  speedUnit = (SpeedUnit)prefs.getInt(SPEED_UNIT_KEY, SPEED_KMH);
  temperatureUnit = (TemperatureUnit)prefs.getInt(TEMP_UNIT_KEY, TEMP_CELSIUS);
  pressureUnit = (PressureUnit)prefs.getInt(PRESSURE_UNIT_KEY, PRESSURE_HPA);

  if (themeMode == THEME_AUTO)
    isDarkTheme = readIsDark();
  else
    isDarkTheme = (themeMode == THEME_DARK);

  applyThemeColors(isDarkTheme);
  clockTimeChanged = false;
  returnToMainScreen();
}

static bool pointInRect(uint16_t x, uint16_t y,
                        int x1, int y1, int x2, int y2)
{
  return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

void handleSettingsTouch(uint16_t x, uint16_t y)
{
  // 1. Тема.
  if (pointInRect(x, y, SETTINGS_LIGHT_X1, SETTINGS_LIGHT_Y1,
                  SETTINGS_LIGHT_X2, SETTINGS_LIGHT_Y2))
  {
    changeTheme(THEME_WHITE);
    drawSettingsSelectionIndicators();
    return;
  }

  if (pointInRect(x, y, SETTINGS_DARK_X1, SETTINGS_DARK_Y1,
                  SETTINGS_DARK_X2, SETTINGS_DARK_Y2))
  {
    changeTheme(THEME_DARK);
    drawSettingsSelectionIndicators();
    return;
  }

  if (pointInRect(x, y, SETTINGS_AUTO_X1, SETTINGS_AUTO_Y1,
                  SETTINGS_AUTO_X2, SETTINGS_AUTO_Y2))
  {
    changeTheme(THEME_AUTO);
    drawSettingsSelectionIndicators();
    return;
  }

  // 2. Длина колеса, шаг 5 мм.
  if (pointInRect(x, y, SETTINGS_WHEEL_MINUS_X1, SETTINGS_WHEEL_MINUS_Y1,
                  SETTINGS_WHEEL_MINUS_X2, SETTINGS_WHEEL_MINUS_Y2))
  {
    wheelCircumferenceMm -= CIRCUMFERENCE_STEP_MM;
    if (wheelCircumferenceMm < CIRCUMFERENCE_MIN_MM)
      wheelCircumferenceMm = CIRCUMFERENCE_MIN_MM;
    updateHallAbsoluteFilterLimit();
    drawSettingsWheelValue(true);
    return;
  }

  if (pointInRect(x, y, SETTINGS_WHEEL_PLUS_X1, SETTINGS_WHEEL_PLUS_Y1,
                  SETTINGS_WHEEL_PLUS_X2, SETTINGS_WHEEL_PLUS_Y2))
  {
    wheelCircumferenceMm += CIRCUMFERENCE_STEP_MM;
    if (wheelCircumferenceMm > CIRCUMFERENCE_MAX_MM)
      wheelCircumferenceMm = CIRCUMFERENCE_MAX_MM;
    updateHallAbsoluteFilterLimit();
    drawSettingsWheelValue(true);
    return;
  }

  // 3. Количество магнитов, шаг 1.
  if (pointInRect(x, y, SETTINGS_MAGNETS_MINUS_X1, SETTINGS_MAGNETS_MINUS_Y1,
                  SETTINGS_MAGNETS_MINUS_X2, SETTINGS_MAGNETS_MINUS_Y2))
  {
    if (magnetsPerRevolution > MAGNETS_MIN)
      magnetsPerRevolution--;
    updateHallAbsoluteFilterLimit();
    drawSettingsMagnetsValue(true);
    return;
  }

  if (pointInRect(x, y, SETTINGS_MAGNETS_PLUS_X1, SETTINGS_MAGNETS_PLUS_Y1,
                  SETTINGS_MAGNETS_PLUS_X2, SETTINGS_MAGNETS_PLUS_Y2))
  {
    if (magnetsPerRevolution < MAGNETS_MAX)
      magnetsPerRevolution++;
    updateHallAbsoluteFilterLimit();
    drawSettingsMagnetsValue(true);
    return;
  }

  // 4. Часы.
  if (pointInRect(x, y, SETTINGS_HOURS_MINUS_X1, SETTINGS_HOURS_MINUS_Y1,
                  SETTINGS_HOURS_MINUS_X2, SETTINGS_HOURS_MINUS_Y2))
  {
    clockSetHour = (clockSetHour + 23) % 24;
    clockTimeChanged = true;
    drawSettingsHoursValue(true);
    return;
  }

  if (pointInRect(x, y, SETTINGS_HOURS_PLUS_X1, SETTINGS_HOURS_PLUS_Y1,
                  SETTINGS_HOURS_PLUS_X2, SETTINGS_HOURS_PLUS_Y2))
  {
    clockSetHour = (clockSetHour + 1) % 24;
    clockTimeChanged = true;
    drawSettingsHoursValue(true);
    return;
  }

  // 5. Минуты.
  if (pointInRect(x, y, SETTINGS_MINUTES_MINUS_X1, SETTINGS_MINUTES_MINUS_Y1,
                  SETTINGS_MINUTES_MINUS_X2, SETTINGS_MINUTES_MINUS_Y2))
  {
    clockSetMinute = (clockSetMinute + 59) % 60;
    clockTimeChanged = true;
    drawSettingsMinutesValue(true);
    return;
  }

  if (pointInRect(x, y, SETTINGS_MINUTES_PLUS_X1, SETTINGS_MINUTES_PLUS_Y1,
                  SETTINGS_MINUTES_PLUS_X2, SETTINGS_MINUTES_PLUS_Y2))
  {
    clockSetMinute = (clockSetMinute + 1) % 60;
    clockTimeChanged = true;
    drawSettingsMinutesValue(true);
    return;
  }

  // 6. Набор единиц.
  if (pointInRect(x, y, SETTINGS_METRIC_X1, SETTINGS_METRIC_Y1,
                  SETTINGS_METRIC_X2, SETTINGS_METRIC_Y2))
  {
    speedUnit = SPEED_KMH;
    temperatureUnit = TEMP_CELSIUS;
    pressureUnit = PRESSURE_MMHG;
    drawSettingsSelectionIndicators();
    return;
  }

  if (pointInRect(x, y, SETTINGS_IMPERIAL_X1, SETTINGS_IMPERIAL_Y1,
                  SETTINGS_IMPERIAL_X2, SETTINGS_IMPERIAL_Y2))
  {
    speedUnit = SPEED_MPH;
    temperatureUnit = TEMP_FAHRENHEIT;
    pressureUnit = PRESSURE_INHG;
    drawSettingsSelectionIndicators();
    return;
  }

  // 7. Cancel / Save.
  if (pointInRect(x, y, SETTINGS_CANCEL_X1, SETTINGS_CANCEL_Y1,
                  SETTINGS_CANCEL_X2, SETTINGS_CANCEL_Y2))
  {
    drawSettingsActionFeedback(63, 210, 96, 24);
    discardSettingsAndReturnToMain();
    return;
  }

  if (pointInRect(x, y, SETTINGS_SAVE_X1, SETTINGS_SAVE_Y1,
                  SETTINGS_SAVE_X2, SETTINGS_SAVE_Y2))
  {
    drawSettingsActionFeedback(169, 210, 96, 24);
    saveSettings();
    return;
  }
}

void handleTouch()
{
  uint16_t x = 0, y = 0;
  bool down = readTouch(x, y);

  if (!down)
  {
    if (timerPressActive)
    {
      const unsigned long holdMs = millis() - timerPressStartMs;

      if (holdMs >= 700)
      {
        if (activeTimerIndex == 0)
          timer1AccumulatedMs = 0;
        else
          timer2AccumulatedMs = 0;

        if (activeTimerIndex == 0)
          timer1StartedAtMs = millis();
        else
          timer2StartedAtMs = millis();

        lastDrawnTimerTotalSec = -1;
        lastDrawnTimerLabel[0] = '\0';
      }
      else
      {
        activeTimerIndex = (activeTimerIndex + 1) % 2;
        lastDrawnTimerTotalSec = -1;
        lastDrawnTimerLabel[0] = '\0';
      }

      timerPressActive = false;
      timerPressStartMs = 0;
    }

    if (tripPressActive)
    {
      const unsigned long holdMs = millis() - tripPressStartMs;

      if (holdMs >= 700)
      {
        if (activeTripIndex == 0)
        {
          trip1Km = 0.0;
          trip1MaxSpeedKmh = 0.0;
          noInterrupts();
          trip1MovingBaseMicros = totalMovingMicros;
          interrupts();
        }
        else
        {
          trip2Km = 0.0;
          trip2MaxSpeedKmh = 0.0;
          noInterrupts();
          trip2MovingBaseMicros = totalMovingMicros;
          interrupts();
        }

        lastDrawnTrip = -1.0;
        lastDrawnTripLabel[0] = '\0';
      }
      else
      {
        activeTripIndex = (activeTripIndex + 1) % 2;
        lastDrawnTrip = -1.0;
        lastDrawnTripLabel[0] = '\0';
      }

      // avg.speed/max.speed теперь зависят от активного trip —
      // форсируем их перерисовку и при сбросе, и при переключении.
      lastDrawnAvgMetricValue = NAN;
      lastDrawnMaxMetricValue = NAN;

      tripPressActive = false;
      tripPressStartMs = 0;
    }

    touchWasDown = false;
    return;
  }

  if (touchWasDown)
    return;

  touchWasDown = true;

  const bool inTimerArea = x <= DIV1_X && y >= 190 && y <= 238;
  const bool inTripArea = x >= DIV1_X + 2 && x <= DIV2_X - 2 && y >= 190 && y <= 238;

  if (settingsScreen)
  {
    handleSettingsTouch(x, y);
    return;
  }

  if (x >= BATT_PERCENT_AREA_X1 && x <= BATT_PERCENT_AREA_X2 &&
      y >= BATT_PERCENT_AREA_Y1 && y <= BATT_PERCENT_AREA_Y2)
  {
    batteryShowVoltage = !batteryShowVoltage;
    lastDrawnBatteryPct = -999;
    return;
  }

  if (x >= WRENCH_TOUCH_X1 && x <= WRENCH_TOUCH_X2 &&
      y >= WRENCH_TOUCH_Y1 && y <= WRENCH_TOUCH_Y2)
  {
    settingsScreen = true;
    beginClockEdit();
    drawSettingsScreen();
    return;
  }

  if (inTimerArea)
  {
    timerPressActive = true;
    timerPressStartMs = millis();
    return;
  }

  if (inTripArea)
  {
    tripPressActive = true;
    tripPressStartMs = millis();
    return;
  }

  if (millis() - lastTouchMs < 180)
    return;

  lastTouchMs = millis();

}


// ============================================================
// ЖИРНАЯ СКОРОСТЬ
// ============================================================

void drawBoldSpeedTo(TFT_eSprite &spr, const char *text, int centerX, int centerY)
{
  spr.setFreeFont(&Conthrax_SB_56);
  spr.setTextColor(colText);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(1);
  spr.drawString(text, centerX, centerY);
}


// ============================================================
// СКОРОСТЬ НА ЭКРАНЕ — плавная анимация
// ============================================================

void drawSpeed()
{
  unsigned long nowMs = millis();

  // ------------------------------------------------------------
  // Реальная скорость -> целевая скорость для анимации
  // ------------------------------------------------------------
  float targetSpeedKmh = speedKmh;

  // Ограничение 0...99 км/ч
  if (targetSpeedKmh < 0.0f)
    targetSpeedKmh = 0.0f;

  if (targetSpeedKmh > 99.0f)
    targetSpeedKmh = 99.0f;

  // ------------------------------------------------------------
  // Плавно приближаем отображаемую скорость к реальной
  // ------------------------------------------------------------
  if (nowMs - lastSpeedAnimMs >= SPEED_ANIM_INTERVAL_MS)
  {
    lastSpeedAnimMs = nowMs;

    if (animatedSpeedKmh < targetSpeedKmh)
    {
      animatedSpeedKmh += SPEED_ANIM_STEP_KMH;

      if (animatedSpeedKmh > targetSpeedKmh)
        animatedSpeedKmh = targetSpeedKmh;
    }
    else if (animatedSpeedKmh > targetSpeedKmh)
    {
      animatedSpeedKmh -= SPEED_ANIM_STEP_KMH;

      if (animatedSpeedKmh < targetSpeedKmh)
        animatedSpeedKmh = targetSpeedKmh;
    }
  }

  // ------------------------------------------------------------
  // Переводим именно отображаемую скорость в выбранные единицы
  // ------------------------------------------------------------
  float displayedSpeed =
      speedUnit == SPEED_MPH
          ? animatedSpeedKmh * KMH_TO_MPH
          : animatedSpeedKmh;

  if (displayedSpeed > 99.0f)
    displayedSpeed = 99.0f;

  // ------------------------------------------------------------
  // Если визуально значение не изменилось — ничего не рисуем
  // ------------------------------------------------------------
  if (abs(displayedSpeed - lastDrawnSpeed) < 0.1f)
  {
    return;
  }

  lastDrawnSpeed = displayedSpeed;

  char buf[8];
  snprintf(buf, sizeof(buf), "%.0f", displayedSpeed);

  // ------------------------------------------------------------
  // Спрайт касается только правой области скорости.
  // Левый блок avg/max НЕ затрагивается.
  // ------------------------------------------------------------

  // Координаты зоны очистки скорости (спрайт): левый верхний угол X/Y, размер 165x70.
  const int SPRITE_X = 138;
  const int SPRITE_Y = 80;
  // Координаты вывода скорости: центр значения скорости на основном экране.
  const int SPEED_CENTER_X = 216;
  const int SPEED_CENTER_Y = 116;

  // ------------------------------------------------------------
  // Восстанавливаем фон внутри спрайта
  // ------------------------------------------------------------

  if (backgroundSpeed)
    speedSprite.pushImage(0, 0, 165, 70, backgroundSpeed);
  else
    speedSprite.fillSprite(colBg);

  // ------------------------------------------------------------
  // Скорость
  // ------------------------------------------------------------

  drawBoldSpeedTo(
    speedSprite,
    buf,
    SPEED_CENTER_X - SPRITE_X,
    SPEED_CENTER_Y - SPRITE_Y
  );

  // ------------------------------------------------------------
  // Выводим готовый кадр целиком
  // ------------------------------------------------------------

  speedSprite.pushSprite(SPRITE_X, SPRITE_Y);
}

// ============================================================
// НИЖНЯЯ СТРОКА ЗНАЧЕНИЙ: timer | trip | odo
// ============================================================

void drawBottomValues(unsigned long nowMs)
{
  tft.setFreeFont(&Conthrax_SB_15);
  tft.setTextColor(colText);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);

  const char* activeTimerLabel = activeTimerIndex == 0 ? "timer1" : "timer2";
  const bool timerLabelDirty = strcmp(lastDrawnTimerLabel, activeTimerLabel) != 0;
  if (timerLabelDirty)
  {
    strcpy(lastDrawnTimerLabel, activeTimerLabel);
    // Координаты очистки прямоугольника активного timer: X=0, Y=LABEL_Y-18, ширина=DIV1_X, высота=28.
    restoreMainBackgroundRegion(0, LABEL_Y - 18, DIV1_X, 28);
    // Подпись timer удалена — оставляем только числовое значение.
  }

  // avg.speed/max.speed относятся к текущему выбранному trip (1 или 2).
  // avg.speed считается только по времени фактического движения:
  // остановки длиннее SPEED_TIMEOUT_MS в расчёт не входят.
  float activeTripDistanceKm = activeTripIndex == 0 ? trip1Km : trip2Km;
  float activeTripMaxSpeedKmh = activeTripIndex == 0 ? trip1MaxSpeedKmh : trip2MaxSpeedKmh;

  uint64_t movingMicrosSnapshot;
  noInterrupts();
  movingMicrosSnapshot = totalMovingMicros;
  interrupts();

  uint64_t activeTripMovingBaseMicros =
      activeTripIndex == 0 ? trip1MovingBaseMicros : trip2MovingBaseMicros;
  uint64_t activeTripMovingMicros =
      movingMicrosSnapshot >= activeTripMovingBaseMicros
          ? (movingMicrosSnapshot - activeTripMovingBaseMicros)
          : 0;

  float averageSpeedKmh = 0.0f;
  if (activeTripMovingMicros > 0)
  {
    const double movingHours = (double)activeTripMovingMicros / 3600000000.0;
    averageSpeedKmh = (float)(activeTripDistanceKm / movingHours);
  }

  float averageSpeed = speedUnit == SPEED_MPH ? averageSpeedKmh * KMH_TO_MPH : averageSpeedKmh;
  float maxSpeed = speedUnit == SPEED_MPH ? activeTripMaxSpeedKmh * KMH_TO_MPH : activeTripMaxSpeedKmh;

  const char* avgLabel = "avg.speed";
  const char* maxLabel = "max.speed";
  float avgValue = averageSpeed;
  float maxValue = maxSpeed;

  const bool avgDirty =
      lastDrawnAvgSpeed < 0.0f ||
      strcmp(lastDrawnAvgLabel, avgLabel) != 0 ||
      isnan(lastDrawnAvgMetricValue) ||
      abs(avgValue - lastDrawnAvgMetricValue) >= 0.1f;

  const bool maxDirty =
      lastDrawnMaxSpeed < 0.0f ||
      strcmp(lastDrawnMaxLabel, maxLabel) != 0 ||
      isnan(lastDrawnMaxMetricValue) ||
      abs(maxValue - lastDrawnMaxMetricValue) >= 0.1f;

  if (avgDirty || maxDirty)
  {
    strcpy(lastDrawnAvgLabel, avgLabel);
    strcpy(lastDrawnMaxLabel, maxLabel);
    lastDrawnAvgMetricValue = avgValue;
    lastDrawnMaxMetricValue = maxValue;
    lastDrawnAvgSpeed = avgValue;
    lastDrawnMaxSpeed = maxValue;

    // Координаты очистки области max.speed и avg.speed: X=0, Y=60, ширина=118, высота=126.
    restoreMainBackgroundRegion(0, 60, 118, 126);

    char avgBuf[16];
    char maxBuf[16];
    snprintf(avgBuf, sizeof(avgBuf), "%.1f", avgValue);
    snprintf(maxBuf, sizeof(maxBuf), "%.1f", maxValue);

    tft.setFreeFont(&Conthrax_SB_15);
    tft.setTextDatum(BR_DATUM);
    tft.setTextSize(1);

    // Значения на левой панели: верхняя ячейка = max.speed, нижняя = avg.speed.
    // X/Y задают правый нижний угол значения; цифры растут влево и вверх.
    // Координаты вывода max.speed: X=95, Y=111 (правый нижний угол значения).
    tft.drawString(maxBuf, 95, 116);
    // Координаты вывода avg.speed: X=95, Y=169 (правый нижний угол значения).
    tft.drawString(avgBuf, 95, 174);

    // Остальные элементы ниже используют центрированную привязку.
    tft.setTextDatum(MC_DATUM);
  }

  // -------------------- TIMER 1 / TIMER 2 --------------------
  if (timer1StartedAtMs == 0 && activeTimerIndex == 0)
    timer1StartedAtMs = nowMs;
  if (timer2StartedAtMs == 0 && activeTimerIndex == 1)
    timer2StartedAtMs = nowMs;

  unsigned long timer1ElapsedMs = timer1StartedAtMs == 0 ? 0 : (nowMs - timer1StartedAtMs) + timer1AccumulatedMs;
  unsigned long timer2ElapsedMs = timer2StartedAtMs == 0 ? 0 : (nowMs - timer2StartedAtMs) + timer2AccumulatedMs;

  unsigned long selectedTimerElapsedMs = activeTimerIndex == 0 ? timer1ElapsedMs : timer2ElapsedMs;
  long totalSec = selectedTimerElapsedMs / 1000;

  if (totalSec != lastDrawnTimerTotalSec)
  {
    lastDrawnTimerTotalSec = totalSec;

    int h = (totalSec / 3600) % 24;
    int m = (totalSec / 60) % 60;

    // Координаты очистки значения timer: X=2, Y=VALUE_Y-16, ширина=DIV1_X-4, высота=32.
    restoreMainBackgroundRegion(2, VALUE_Y - 16, DIV1_X - 4, 32);

    // Координаты вывода прямоугольника активного timer: timer1 X=64,Y=181; timer2 X=85,Y=181; размер 17x15.
    if (activeTimerIndex == 0)
      tft.drawRect(65, 187, 18, 15, isDarkTheme ? TFT_WHITE : TFT_RED);
    else
      tft.drawRect(85, 187, 18, 15, isDarkTheme ? TFT_WHITE : TFT_RED);

    char buf[10];
    snprintf(buf, sizeof(buf), "%01d:%02d", h, m);

    tft.setFreeFont(&Conthrax_SB_15);
    tft.setTextSize(1);
    // Координаты вывода timer: X=COL1_X+10, Y=VALUE_Y-4.
    tft.drawString(buf, COL1_X + 10, VALUE_Y - 4);
  }

  // -------------------- TRIP 1 / TRIP 2 --------------------
  float activeTripValueKm = activeTripIndex == 0 ? trip1Km : trip2Km;
  float activeTripValue = speedUnit == SPEED_MPH ? activeTripValueKm * 0.621371f : activeTripValueKm;

  if (abs(activeTripValue - lastDrawnTrip) >= 0.01)
  {
    lastDrawnTrip = activeTripValue;

    // Координаты очистки прямоугольника активного trip: X=DIV1_X+2, Y=LABEL_Y-18, ширина=DIV2_X-DIV1_X-4, высота=28.
    restoreMainBackgroundRegion(DIV1_X + 2, LABEL_Y - 18,
                                DIV2_X - DIV1_X - 4, 28);
    // Координаты очистки значения trip: X=DIV1_X+2, Y=VALUE_Y-16, ширина=DIV2_X-DIV1_X-4, высота=32.
    restoreMainBackgroundRegion(DIV1_X + 2, VALUE_Y - 16, DIV2_X - DIV1_X - 4, 32);

    // Координаты вывода прямоугольника активного trip: trip1 X=160,Y=181; trip2 X=182,Y=181; размер 17x15.
    if (activeTripIndex == 0)
      tft.drawRect(161, 187, 18, 15, isDarkTheme ? TFT_WHITE : TFT_RED);
    else
      tft.drawRect(182, 187, 18, 15, isDarkTheme ? TFT_WHITE : TFT_RED);

    char buf[10];
    snprintf(buf, sizeof(buf), "%.1f", activeTripValue);
    tft.setFreeFont(&Conthrax_SB_15);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    // Координаты вывода trip: X=COL2_X+10, Y=VALUE_Y-4.
    tft.drawString(buf, COL2_X + 10, VALUE_Y - 4);
  }

  // -------------------- ODO --------------------
  float displayedOdometer = speedUnit == SPEED_MPH ? odometerKm * 0.621371f : odometerKm;
  if (abs(displayedOdometer - lastDrawnOdo) >= 0.01)
  {
    lastDrawnOdo = displayedOdometer;

    // Координаты очистки значения odo: X=DIV2_X+2, Y=VALUE_Y-16, ширина=320-DIV2_X-4, высота=32.
    restoreMainBackgroundRegion(DIV2_X + 2, VALUE_Y - 16, 320 - DIV2_X - 4, 32);

    char buf[10];
    snprintf(buf, sizeof(buf), "%06.1f", displayedOdometer);
    // Координаты вывода odo: X=COL3_X-2, Y=VALUE_Y-4.
    tft.drawString(buf, COL3_X - 2, VALUE_Y - 4);
  }
}