/*
  TACÓMETRO CON SENSOR EFECTO HALL - ESP32 (Arduino IDE)

  MODO_ADC (por defecto):
    El sensor Hall analógico entra directo a un pin ADC1 (GPIO34).
    - Atenuación del ADC: 11 dB (intervalo máximo, ~0 a 3.1 V)
    - Muestreo a 5 kHz con un temporizador
    - Filtro digital: media móvil exponencial
    - Disparador por software con histéresis (tipo Schmitt)
    IMPORTANTE: la entrada ADC NO debe superar 3.3 V. Alimenta el Hall a
    3.3 V (el SS49E reposa en ~1.65 V) o usa tu etapa con op-amp.

  MODO_DIGITAL:
    Salida del comparador con op-amp -> GPIO27 (interrupción por flanco).

  Medición: periodo entre pulsos. RPM = 60e6 / (periodo_us * PULSOS_POR_REV)

  LCD 16x2 I2C: SDA = GPIO21, SCL = GPIO22 (0x27 o 0x3F)
  Librería: "LiquidCrystal I2C" de Frank de Brabander
  Placa: "ESP32 Dev Module", Serial a 115200.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "esp_timer.h"

// ======================= CONFIGURACIÓN =======================
#define MODO_DIGITAL      0
#define MODO_ADC          1
#define MODO_ENTRADA      MODO_ADC   // elige MODO_ADC o MODO_DIGITAL

// --- Modo digital ---
#define HALL_PIN          27
#define USAR_PULLUP       false

// --- Modo ADC ---
#define HALL_ADC_PIN      34         // ADC1_CH6 (solo entrada)
#define REPOSO_MV         1650.0f    // Salida del Hall sin imán (mV). SS49E a 3.3 V = ~1650
#define DELTA_ALTO_MV     150.0f     // Dispara cuando la señal sube REPOSO + este valor
#define DELTA_BAJO_MV     80.0f      // Se rearma cuando baja de REPOSO + este valor (histéresis)
#define INVERTIR_POLO     false      // true si tu imán hace BAJAR la señal en vez de subirla
#define ALFA_FILTRO       0.4f       // Filtro EMA (0.1 = muy suave, 1 = sin filtro)
#define PERIODO_MUESTREO_US 200      // 5 kHz
#define DEBUG_ADC         false      // true: imprime mV filtrados para calibrar umbrales

// --- General ---
#define PULSOS_POR_REV    1          // Número de imanes en el eje
#define NUM_PROMEDIO      3          // Periodos promediados
#define RPM_MIN_DETECTA   10.0f      // Debajo de esto se muestra 0
#define RPM_MAX_VALIDA    1000.0f    // Pulsos más rápidos se descartan
#define RESOLUCION_RPM    0.5f

#define LCD_ADDR          0x27
#define LCD_COLS          16
#define LCD_ROWS          2
#define SDA_PIN           21
#define SCL_PIN           22

#define T_LCD_MS          250
#define T_SERIAL_MS       100
// =============================================================

const uint32_t PERIODO_MIN_US = (uint32_t)(60e6f / (RPM_MAX_VALIDA * PULSOS_POR_REV));
const uint32_t TIMEOUT_US     = (uint32_t)(60e6f / (RPM_MIN_DETECTA * PULSOS_POR_REV));

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t ultimoPulso_us = 0;
volatile bool     hayPulsoPrevio = false;
volatile uint32_t periodos[NUM_PROMEDIO];
volatile uint8_t  idxPeriodo = 0;
volatile uint8_t  cuenta = 0;

// Registra un pulso (llamado desde la ISR o desde el temporizador ADC)
void IRAM_ATTR registrarPulso(uint32_t t) {
  uint32_t dt = t - ultimoPulso_us;

  if (hayPulsoPrevio) {
    if (dt < PERIODO_MIN_US) return;          // antirruido
    if (dt > TIMEOUT_US) {                    // motor estuvo detenido
      cuenta = 0;
      idxPeriodo = 0;
    } else {
      periodos[idxPeriodo] = dt;
      idxPeriodo = (idxPeriodo + 1) % NUM_PROMEDIO;
      if (cuenta < NUM_PROMEDIO) cuenta++;
    }
  }
  ultimoPulso_us = t;
  hayPulsoPrevio = true;
}

#if MODO_ENTRADA == MODO_DIGITAL
void IRAM_ATTR isrHall() {
  registrarPulso((uint32_t)esp_timer_get_time());
}
#else
volatile float mV_filtrado = 0;

// Se ejecuta cada PERIODO_MUESTREO_US: lee, filtra y detecta el pulso
void muestrearHall(void* arg) {
  static float filtrado = REPOSO_MV;
  static bool  alto = false;

  float v = (float)analogReadMilliVolts(HALL_ADC_PIN);
  if (INVERTIR_POLO) v = 2.0f * REPOSO_MV - v;

  filtrado += ALFA_FILTRO * (v - filtrado);   // filtro EMA
  mV_filtrado = filtrado;

  if (!alto && filtrado > REPOSO_MV + DELTA_ALTO_MV) {
    alto = true;
    uint32_t t = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&mux);
    registrarPulso(t);
    portEXIT_CRITICAL(&mux);
  } else if (alto && filtrado < REPOSO_MV + DELTA_BAJO_MV) {
    alto = false;                               // rearmar
  }
}
#endif

float calcularRPM() {
  uint32_t ultimo, suma = 0;
  uint8_t  n;
  bool     previo;

  portENTER_CRITICAL(&mux);
  ultimo = ultimoPulso_us;
  n      = cuenta;
  previo = hayPulsoPrevio;
  for (uint8_t i = 0; i < n; i++) suma += periodos[i];
  portEXIT_CRITICAL(&mux);

  if (!previo) return 0.0f;

  uint32_t transcurrido = (uint32_t)esp_timer_get_time() - ultimo;
  if (n == 0 || transcurrido > TIMEOUT_US) return 0.0f;

  float periodoProm = (float)suma / n;
  float rpm = 60e6f / (periodoProm * PULSOS_POR_REV);

  // Motor frenando: la RPM real es como máximo la que implica el tiempo transcurrido
  if (transcurrido > periodoProm) {
    float rpmTope = 60e6f / ((float)transcurrido * PULSOS_POR_REV);
    if (rpmTope < rpm) rpm = rpmTope;
  }
  return rpm;
}

void mostrarLCD(float rpm) {
  float omega = rpm * 2.0f * PI / 60.0f;   // rad/s
  char l0[17], l1[17];
  snprintf(l0, sizeof(l0), "RPM: %-11.1f", rpm);
  snprintf(l1, sizeof(l1), "w: %-6.2f rad/s ", omega);
  lcd.setCursor(0, 0); lcd.print(l0);
  lcd.setCursor(0, 1); lcd.print(l1);
}

void setup() {
  Serial.begin(115200);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Tacometro Hall");
  lcd.setCursor(0, 1); lcd.print("ESP32 listo...");
  delay(1200);
  lcd.clear();

#if MODO_ENTRADA == MODO_DIGITAL
  pinMode(HALL_PIN, USAR_PULLUP ? INPUT_PULLUP : INPUT);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), isrHall, FALLING);
  Serial.println("Modo DIGITAL");
#else
  analogReadResolution(12);
  analogSetPinAttenuation(HALL_ADC_PIN, ADC_11db);   // atenuación 11 dB

  esp_timer_handle_t timerADC;
  esp_timer_create_args_t args = {};
  args.callback = &muestrearHall;
  args.name = "muestreo_hall";
  esp_timer_create(&args, &timerADC);
  esp_timer_start_periodic(timerADC, PERIODO_MUESTREO_US);
  Serial.println("Modo ADC (11 dB)");
#endif
}

void loop() {
  static uint32_t tLcd = 0, tSerial = 0;
  uint32_t ahora = millis();

  float rpm = calcularRPM();
  float rpmMostrar = roundf(rpm / RESOLUCION_RPM) * RESOLUCION_RPM;

  if (ahora - tSerial >= T_SERIAL_MS) {
    tSerial = ahora;
#if (MODO_ENTRADA == MODO_ADC) && DEBUG_ADC
    Serial.print("mV:");
    Serial.println(mV_filtrado, 0);
#else
    Serial.print("RPM:");
    Serial.println(rpmMostrar, 1);
#endif
  }

  if (ahora - tLcd >= T_LCD_MS) {
    tLcd = ahora;
    mostrarLCD(rpmMostrar);
  }
}
