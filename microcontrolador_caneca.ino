#include <ESP32Servo.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ================= CONFIGURACIÓN SEGURA =================
const int PIN_SERVO = 27;             // GPIO 27: Pin silencioso al arrancar (sin pulsos basura)
const int PIN_LED = 2;                // LED azul integrado en la placa
const int ANGULO_CERRADO = 10;        // 10° en lugar de 0° evita forzar el tope mecánico interno
const int ANGULO_ABIERTO = 90;        // Apertura de la tapa
const int TIEMPO_ABIERTA_MS = 3500;   // Tiempo abierta (sincronizado con los 5.0s de Python)
// ========================================================

Servo tapaServo;

void moverServoSeguro(int anguloDestino, int tiempoEsperaMs) {
  // 1. Energizar la señal PWM solo en el instante del movimiento
  tapaServo.setPeriodHertz(50);
  tapaServo.attach(PIN_SERVO, 500, 2400);
  
  // 2. Ordenar posición y esperar a que llegue físicamente
  tapaServo.write(anguloDestino);
  delay(tiempoEsperaMs);
  
  // 3. PROTECCIÓN CLAVE: Cortar señal PWM (0 mA de consumo en reposo, cero zumbidos)
  tapaServo.detach();
  digitalWrite(PIN_SERVO, LOW); 
}

void ejecutarCicloApertura() {
  digitalWrite(PIN_LED, HIGH); // Encender LED indicador
  
  // Abrir tapa, mantener 3.5 segundos y apagar motor
  moverServoSeguro(ANGULO_ABIERTO, TIEMPO_ABIERTA_MS);
  
  // Cerrar tapa, dar 600 ms para que baje el brazo y apagar motor
  moverServoSeguro(ANGULO_CERRADO, 600);
  
  digitalWrite(PIN_LED, LOW);  // Apagar LED indicador

  // Limpiar cualquier letra 'B' repetida que haya llegado mientras la tapa estaba abierta
  while (Serial.available() > 0) {
    Serial.read();
  }
}

void setup() {
  // Evita reinicios en bucle si el puerto USB de la PC baja el voltaje por un milisegundo
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Asegurar que el pin 27 inicie en 0V absoluto antes de hacer nada
  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, LOW);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  delay(500);

  // Posicionar la tapa cerrada al encender y luego apagar el servo inmediatamente
  moverServoSeguro(ANGULO_CERRADO, 600);
}

void loop() {
  if (Serial.available() > 0) {
    char comando = Serial.read();
    
    if (comando == 'B') {
      ejecutarCicloApertura();
    }
  }
}