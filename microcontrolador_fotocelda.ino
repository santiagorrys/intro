// --- PINES DEL SISTEMA PRO-AVES ---
const int PIN_LDR    = 34; // Entrada analógica del sensor de luz (Fotocelda)
const int PIN_PIR    = 27; // Entrada digital del sensor de movimiento HC-SR501
const int PIN_MOSFET = 25; // Salida PWM hacia el transistor 2N2222A / IRF520
const int PIN_LED_INTERNO = 2; // LED integrado en el ESP32 para pruebas hoy

// Umbral de luz (Ajustable de 0 a 4095 según el salón de exposición)
const int UMBRAL_NOCHE = 2000; 

// Invierte la señal en GPIO 25 si usas el transistor 2N2222A como puente
// Ponlo en 'false' si conectas el módulo MOSFET directo sin el 2N2222A
const bool USAR_PUENTE_2N2222A = true;

int brilloActual = 0; // 0 a 255

void aplicarPWM(int valorBrillo) {
  // 1. El LED interno del ESP32 muestra el brillo real directamente
  analogWrite(PIN_LED_INTERNO, valorBrillo);
  
  // 2. El pin del MOSFET se adapta según si lleva el puente 2N2222A o no
  if (USAR_PUENTE_2N2222A) {
    analogWrite(PIN_MOSFET, 255 - valorBrillo); // Lógica invertida por el NPN
  } else {
    analogWrite(PIN_MOSFET, valorBrillo);       // Lógica directa
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR, INPUT_PULLDOWN); // Evita ruido si el PIR no está conectado hoy
  pinMode(PIN_LED_INTERNO, OUTPUT);
  pinMode(PIN_MOSFET, OUTPUT);
  
  aplicarPWM(0);
  Serial.println("=== Sistema Luminaria Pro-Aves Iniciado ===");
}

void loop() {
  int lecturaLuz = analogRead(PIN_LDR);
  int movimiento = digitalRead(PIN_PIR);
  int brilloObjetivo = 0;

  // En la mayoría de módulos LDR: mayor valor analógico = más oscuridad (noche)
  bool esDeNoche = (lecturaLuz > UMBRAL_NOCHE);

  if (!esDeNoche) {
    // MODO DÍA: Apagado (0%)
    brilloObjetivo = 0;
  } else {
    if (movimiento == HIGH) {
      // MODO NOCHE + PRESENCIA: 100% de brillo (255 en PWM de 8 bits)
      brilloObjetivo = 255;
    } else {
      // MODO NOCHE PRO-AVES (Reposo): 20% de brillo (51 de 255)
      brilloObjetivo = 51;
    }
  }

  // Transición suave de brillo para no encandilar ni estresar a la fauna
  if (brilloActual < brilloObjetivo) {
    brilloActual = min(brilloActual + 15, brilloObjetivo);
  } else if (brilloActual > brilloObjetivo) {
    brilloActual = max(brilloActual - 15, brilloObjetivo);
  }

  aplicarPWM(brilloActual);

  // Monitoreo en vivo cada 300 ms
  Serial.print("Luz (ADC): "); Serial.print(lecturaLuz);
  Serial.print(" | Noche: "); Serial.print(esDeNoche ? "SI" : "NO");
  Serial.print(" | PIR: "); Serial.print(movimiento ? "DETECTADO" : "Libre");
  Serial.print(" | Brillo PWM: "); Serial.print((brilloActual * 100) / 255);
  Serial.println("%");

  delay(100);
}