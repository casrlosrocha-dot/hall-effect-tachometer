#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Dirección I2C 0x20 (A0, A1, A2 a GND)
LiquidCrystal_I2C lcd(0x20, 16, 2); 

const int pinPot = PA0;
const int pinSensor = PA1;
const int pinPWM = PA8;

volatile unsigned long tiempoAnterior = 0;
volatile unsigned long periodo = 0;
volatile bool nuevoPulso = false;

// Rutina de Interrupción Externa (ISR)
void capturarPulso() {
  unsigned long tiempoActual = micros();
  periodo = tiempoActual - tiempoAnterior;
  tiempoAnterior = tiempoActual;
  nuevoPulso = true;
}

void setup() {
  pinMode(pinPot, INPUT_ANALOG);
  pinMode(pinSensor, INPUT_PULLDOWN);
  pinMode(pinPWM, OUTPUT);

  // Configurar interrupción en el pin PA1 por flanco de subida
  attachInterrupt(digitalPinToInterrupt(pinSensor), capturarPulso, RISING);

  // Inicializar comunicación I2C en pines PB6 (SCL) y PB7 (SDA)
  Wire.setSCL(PB6);
  Wire.setSDA(PB7);
  Wire.begin();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(" Control Motor  ");
  lcd.setCursor(0, 1);
  lcd.print("Medidor de RPM ");
  delay(1500);
  lcd.clear();
}

void loop() {
  // 1. Lectura del potenciómetro de velocidad (ADC 12 bits: 0 a 4095)
  int lecturaADC = analogRead(pinPot);
  int valorPWM = map(lecturaADC, 0, 4095, 0, 255);
  analogWrite(pinPWM, valorPWM);

  // 2. Cálculo de RPM con resolución de 0.5
  if (nuevoPulso && periodo > 0) {
    float rps = 1000000.0 / periodo;
    float rpm = rps * 60.0;

    // Redondeo estricto a resolución de 0,5 RPM
    rpm = round(rpm * 2.0) / 2.0;

    lcd.setCursor(0, 0);
    lcd.print("RPM: ");
    lcd.print(rpm, 1);
    lcd.print("      ");

    lcd.setCursor(0, 1);
    lcd.print("PWM: ");
    lcd.print((valorPWM * 100) / 255);
    lcd.print("%   ");

    nuevoPulso = false;
  }

  // Protección si la señal se detiene
  if (micros() - tiempoAnterior > 2000000) {
    lcd.setCursor(0, 0);
    lcd.print("RPM: 0.0      ");
  }

  delay(100);
}