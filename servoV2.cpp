#include <WiFi.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// Compatibilidad transparente con ArduinoJson v7 (actual) y v6
#if ARDUINOJSON_VERSION_MAJOR >= 7
  using DocJson = JsonDocument;
#else
  using DocJson = StaticJsonDocument<256>;
#endif

// ================= CONFIGURACIÓN SEGURA (ORIGINAL CONSERVADA) =================
const int PIN_SERVO_BLANCA = 27;      // GPIO 27: Caneca BLANCA (pin original)
const int PIN_SERVO_VERDE  = 26;      // GPIO 26: Caneca VERDE (nuevo)
const int PIN_LED          = 2;       // LED azul integrado en la placa

const int ANGULO_CERRADO     = 10;    // 10° en lugar de 0° evita forzar el tope mecánico
const int ANGULO_ABIERTO     = 90;    // Apertura de la tapa
const int TIEMPO_ABIERTA_MS  = 3500;  // Tiempo total abierta (exacto al firmware original)
const int TIEMPO_CIERRE_MS   = 600;   // Tiempo de recorrido para cerrar/abrir físicamente
// ==============================================================================

// ================= CONFIGURACIÓN WI-FI (ACCESS POINT) =========================
const char* WIFI_SSID = "RecicladorIA";
const char* WIFI_PASS = "reciclaje2026"; // >= 8 caracteres activa WPA2-PSK automáticamente
// ==============================================================================

// Objetos de hardware y red
Servo servoBlanca;
Servo servoVerde;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ================= MÁQUINA DE ESTADOS E IDEMPOTENCIA ==========================
// Por qué usamos millis() en lugar de delay() para el ciclo en ejecución:
// ESPAsyncWebServer corre en una tarea independiente (AsyncTCP), pero:
// 1) Nunca debe llamarse delay() dentro del callback onWsEvent, porque dispara
//    el watchdog (WDT) de AsyncTCP y reinicia el ESP32.
// 2) Usar millis() en loop() permite mantener ws.cleanupClients() activo,
//    responder PING o ERROR BUSY al instante y enviar OPENING -> OPENED -> CLOSED
//    sin bloquear el hilo principal ni arriesgar la conexión Wi-Fi.
enum EstadoSistema {
  ESTADO_IDLE,
  ESTADO_BUSY
};

enum FaseCiclo {
  FASE_NINGUNA,
  FASE_INICIAR_APERTURA,
  FASE_ABRIENDO,
  FASE_ESPERANDO_ABIERTA,
  FASE_CERRANDO
};

// Sección crítica para sincronizar datos entre el hilo de AsyncTCP y loop()
portMUX_TYPE muxEstado = portMUX_INITIALIZER_UNLOCKED;

EstadoSistema estadoActual = ESTADO_IDLE;
FaseCiclo faseCiclo = FASE_NINGUNA;
unsigned long tiempoInicioFase = 0;

// Referencias al servo y metadatos del ciclo activo
Servo* servoActivo = nullptr;
int pinActivo = -1;
int64_t idActivo = 0;
char binActivo[8] = "";

// Registro de idempotencia (último comando OPEN válido procesado)
bool hayIdPrevio = false;
int64_t ultimoIdProcesado = 0;
char ultimoBinProcesado[8] = "";
char ultimoStatusEnviado[12] = "";

// Buffer para ensamblar tramas de texto WebSocket sin fragmentar el heap
char bufferWs[512];
// ==============================================================================

// ================= FUNCIONES BASE DE CONTROL DE SERVO =========================
// Paso 1 y 2 originales: energizar PWM solo al mover y ordenar posición
void energizarYMoverServo(Servo &servo, int pin, int anguloDestino) {
  servo.setPeriodHertz(50);
  servo.attach(pin, 500, 2400);
  servo.write(anguloDestino);
}

// Paso 3 original: cortar señal PWM (0 mA en reposo, cero zumbidos) y fijar LOW
void apagarServoSeguro(Servo &servo, int pin) {
  servo.detach();
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

// Misma lógica de moverServoSeguro original, adaptada para recibir Servo y pin.
// Se usa en setup() para dejar ambas tapas cerradas al arrancar (antes de abrir Wi-Fi).
void moverServoSeguro(Servo &servo, int pin, int anguloDestino, int tiempoEsperaMs) {
  energizarYMoverServo(servo, pin, anguloDestino);
  delay(tiempoEsperaMs);
  apagarServoSeguro(servo, pin);
}
// ==============================================================================

// ================= UTILIDADES DE RESPUESTA JSON ===============================
void enviarAckGlobal(int64_t id, const char* status, const char* bin) {
  DocJson doc;
  doc["ack"] = id;
  doc["status"] = status;
  doc["bin"] = bin;

  String salida;
  serializeJson(doc, salida);
  ws.textAll(salida);
  Serial.println("[WS -> TODOS] " + salida);
}

void enviarAckCliente(AsyncWebSocketClient *client, int64_t id, const char* status, const char* bin) {
  if (!client) return;
  DocJson doc;
  doc["ack"] = id;
  doc["status"] = status;
  doc["bin"] = bin;

  String salida;
  serializeJson(doc, salida);
  client->text(salida);
  Serial.println("[WS -> CLIENTE] " + salida);
}

void enviarPong(AsyncWebSocketClient *client, int64_t id, EstadoSistema estado) {
  if (!client) return;
  DocJson doc;
  doc["pong"] = id;
  doc["state"] = (estado == ESTADO_BUSY) ? "BUSY" : "IDLE";

  String salida;
  serializeJson(doc, salida);
  client->text(salida);
  Serial.println("[WS -> CLIENTE] " + salida);
}

void enviarError(AsyncWebSocketClient *client, const char* code) {
  if (!client) return;
  DocJson doc;
  doc["event"] = "ERROR";
  doc["code"] = code;

  String salida;
  serializeJson(doc, salida);
  client->text(salida);
  Serial.println("[WS -> CLIENTE] " + salida);
}
// ==============================================================================

// ================= CICLO DE APERTURA NO BLOQUEANTE ============================
// Misma secuencia eléctrica exacta que el ejecutarCicloApertura original:
//   1. LED ON -> energizar PWM a 90° -> mantener 3500 ms -> detach + LOW
//   2. energizar PWM a 10° -> mantener 600 ms -> detach + LOW -> LED OFF
// Implementado con millis() para no bloquear Wi-Fi y reportar OPENED y CLOSED.
void ejecutarCicloApertura(Servo &servo, int pin) {
  unsigned long ahora = millis();

  // Leemos la fase actual de forma segura
  portENTER_CRITICAL(&muxEstado);
  FaseCiclo fase = faseCiclo;
  int64_t id = idActivo;
  char bin[8];
  strlcpy(bin, binActivo, sizeof(bin));
  portEXIT_CRITICAL(&muxEstado);

  switch (fase) {
    case FASE_INICIAR_APERTURA:
      digitalWrite(PIN_LED, HIGH); // Encender LED indicador
      energizarYMoverServo(servo, pin, ANGULO_ABIERTO);
      tiempoInicioFase = ahora;

      portENTER_CRITICAL(&muxEstado);
      faseCiclo = FASE_ABRIENDO;
      portEXIT_CRITICAL(&muxEstado);
      break;

    case FASE_ABRIENDO:
      // Tras 600 ms (mismo tiempo de recorrido mecánico usado al cerrar),
      // la tapa ya llegó a 90°: notificamos OPENED sin cortar el PWM aún.
      if (ahora - tiempoInicioFase >= (unsigned long)TIEMPO_CIERRE_MS) {
        portENTER_CRITICAL(&muxEstado);
        strlcpy(ultimoStatusEnviado, "OPENED", sizeof(ultimoStatusEnviado));
        faseCiclo = FASE_ESPERANDO_ABIERTA;
        portEXIT_CRITICAL(&muxEstado);

        enviarAckGlobal(id, "OPENED", bin);
      }
      break;

    case FASE_ESPERANDO_ABIERTA:
      // Se cumplen los 3500 ms totales desde que se ordenó abrir:
      // cortamos PWM (igual que el fin del 1er moverServoSeguro) e iniciamos cierre.
      if (ahora - tiempoInicioFase >= (unsigned long)TIEMPO_ABIERTA_MS) {
        apagarServoSeguro(servo, pin);
        energizarYMoverServo(servo, pin, ANGULO_CERRADO);
        tiempoInicioFase = ahora;

        portENTER_CRITICAL(&muxEstado);
        faseCiclo = FASE_CERRANDO;
        portEXIT_CRITICAL(&muxEstado);
      }
      break;

    case FASE_CERRANDO:
      // Tras 600 ms cerrando, cortamos PWM, apagamos LED y volvemos a IDLE.
      if (ahora - tiempoInicioFase >= (unsigned long)TIEMPO_CIERRE_MS) {
        apagarServoSeguro(servo, pin);
        digitalWrite(PIN_LED, LOW); // Apagar LED indicador

        portENTER_CRITICAL(&muxEstado);
        strlcpy(ultimoStatusEnviado, "CLOSED", sizeof(ultimoStatusEnviado));
        faseCiclo = FASE_NINGUNA;
        estadoActual = ESTADO_IDLE;
        servoActivo = nullptr;
        pinActivo = -1;
        portEXIT_CRITICAL(&muxEstado);

        enviarAckGlobal(id, "CLOSED", bin);
      }
      break;

    case FASE_NINGUNA:
    default:
      break;
  }
}
// ==============================================================================

// ================= PROCESAMIENTO DEL PROTOCOLO JSON ===========================
void procesarMensajeWS(AsyncWebSocketClient *client, const char* payload, size_t len) {
  Serial.print("[WS <- CLIENTE] ");
  Serial.println(payload);

  DocJson doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    enviarError(client, "INVALID_CMD");
    return;
  }

  // Validar campo obligatorio "cmd"
  if (!doc["cmd"].is<const char*>()) {
    enviarError(client, "INVALID_CMD");
    return;
  }
  const char* cmd = doc["cmd"];

  // --- COMANDO PING ---
  if (strcmp(cmd, "PING") == 0) {
    if (!doc["id"].is<int64_t>()) {
      enviarError(client, "INVALID_CMD");
      return;
    }
    int64_t id = doc["id"].as<int64_t>();

    portENTER_CRITICAL(&muxEstado);
    EstadoSistema estadoSnapshot = estadoActual;
    portEXIT_CRITICAL(&muxEstado);

    enviarPong(client, id, estadoSnapshot);
    return;
  }

  // --- COMANDO OPEN ---
  if (strcmp(cmd, "OPEN") == 0) {
    if (!doc["id"].is<int64_t>() || !doc["bin"].is<const char*>()) {
      enviarError(client, "INVALID_CMD");
      return;
    }

    int64_t id = doc["id"].as<int64_t>();
    const char* bin = doc["bin"];

    Servo* servoSeleccionado = nullptr;
    int pinSeleccionado = -1;

    if (strcmp(bin, "WHITE") == 0) {
      servoSeleccionado = &servoBlanca;
      pinSeleccionado = PIN_SERVO_BLANCA;
    } else if (strcmp(bin, "GREEN") == 0) {
      servoSeleccionado = &servoVerde;
      pinSeleccionado = PIN_SERVO_VERDE;
    } else {
      enviarError(client, "INVALID_CMD");
      return;
    }

    // Decisión atómica: 1) Idempotencia, 2) Verificar BUSY, 3) Aceptar ciclo
    bool esRepetido = false;
    bool estaOcupado = false;
    char statusRepetido[12] = "";
    char binRepetido[8] = "";

    portENTER_CRITICAL(&muxEstado);
    if (hayIdPrevio && id == ultimoIdProcesado) {
      esRepetido = true;
      strlcpy(statusRepetido, ultimoStatusEnviado, sizeof(statusRepetido));
      strlcpy(binRepetido, ultimoBinProcesado, sizeof(binRepetido));
    } else if (estadoActual == ESTADO_BUSY) {
      estaOcupado = true;
    } else {
      // Reservamos la máquina de estados inmediatamente para evitar condiciones de carrera
      estadoActual = ESTADO_BUSY;
      faseCiclo = FASE_INICIAR_APERTURA;
      servoActivo = servoSeleccionado;
      pinActivo = pinSeleccionado;
      idActivo = id;
      strlcpy(binActivo, bin, sizeof(binActivo));

      // Actualizamos registro de idempotencia
      hayIdPrevio = true;
      ultimoIdProcesado = id;
      strlcpy(ultimoBinProcesado, bin, sizeof(ultimoBinProcesado));
      strlcpy(ultimoStatusEnviado, "OPENING", sizeof(ultimoStatusEnviado));
    }
    portEXIT_CRITICAL(&muxEstado);

    if (esRepetido) {
      Serial.printf("[IDEMPOTENCIA] ID repetido (%lld). Reenviando último ACK.\n", (long long)id);
      enviarAckCliente(client, id, statusRepetido, binRepetido);
      return;
    }

    if (estaOcupado) {
      enviarError(client, "BUSY");
      return;
    }

    // Confirmación inmediata de inicio de apertura; loop() moverá el servo enseguida
    enviarAckGlobal(id, "OPENING", bin);
    return;
  }

  // Comando desconocido
  enviarError(client, "INVALID_CMD");
}

// Callback de eventos de WebSocket (ejecuta en contexto de AsyncTCP)
void onWsEvent(AsyncWebSocket *serverPtr, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WS] Cliente #%u conectado desde %s\n",
                    client->id(), client->remoteIP().toString().c_str());
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] Cliente #%u desconectado\n", client->id());
      break;

    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->message_opcode == WS_TEXT) {
        if (info->len >= sizeof(bufferWs)) {
          enviarError(client, "INVALID_CMD");
          return;
        }
        if (info->index + len <= info->len) {
          memcpy(bufferWs + info->index, data, len);
        }
        if (info->final && (info->index + len == info->len)) {
          bufferWs[info->len] = '\0';
          procesarMensajeWS(client, bufferWs, info->len);
        }
      } else {
        enviarError(client, "INVALID_CMD");
      }
      break;
    }

    case WS_EVT_PONG:
    case WS_EVT_ERROR:
    default:
      break;
  }
}
// ==============================================================================

void setup() {
  // Evita reinicios en bucle si el voltaje cae por un milisegundo (conservado)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Asegurar que ambos pines de servo y el LED inicien en 0V absoluto
  pinMode(PIN_SERVO_BLANCA, OUTPUT);
  digitalWrite(PIN_SERVO_BLANCA, LOW);

  pinMode(PIN_SERVO_VERDE, OUTPUT);
  digitalWrite(PIN_SERVO_VERDE, LOW);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Iniciando Firmware Canecas Wi-Fi ===");

  // Reservar timers de PWM para ESP32Servo (buena práctica al usar 2 servos)
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);

  // Posicionar AMBAS tapas cerradas al encender y apagar los servos inmediatamente
  moverServoSeguro(servoBlanca, PIN_SERVO_BLANCA, ANGULO_CERRADO, TIEMPO_CIERRE_MS);
  moverServoSeguro(servoVerde,  PIN_SERVO_VERDE,  ANGULO_CERRADO, TIEMPO_CIERRE_MS);

  // Garantizar estado inicial IDLE
  estadoActual = ESTADO_IDLE;
  faseCiclo = FASE_NINGUNA;

  // Levantar red Wi-Fi propia en modo Access Point (WPA2)
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(WIFI_SSID, WIFI_PASS);
  if (apOk) {
    Serial.printf("[WIFI] Access Point activo: %s\n", WIFI_SSID);
    Serial.print("[WIFI] IP del ESP32: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("[WS]   Endpoint: ws://192.168.4.1/ws");
  } else {
    Serial.println("[WIFI] Error al iniciar el Access Point.");
  }

  // Configurar y arrancar servidor WebSocket en puerto 80, ruta /ws
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

void loop() {
  // Limpia clientes desconectados para no agotar sockets TCP
  ws.cleanupClients();

  // Verificar si hay un ciclo activo y avanzarlo sin bloquear el Wi-Fi
  portENTER_CRITICAL(&muxEstado);
  EstadoSistema estado = estadoActual;
  Servo* servo = servoActivo;
  int pin = pinActivo;
  portEXIT_CRITICAL(&muxEstado);

  if (estado == ESTADO_BUSY && servo != nullptr && pin >= 0) {
    ejecutarCicloApertura(*servo, pin);
  }
}