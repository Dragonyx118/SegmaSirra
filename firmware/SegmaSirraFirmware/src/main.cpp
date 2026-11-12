// ============================================================
// SegmaSirra 17.0V senza delay bloccanti
// DEBUG VERSION — ultrasuoni con pulseIn (funzionante) ogni 7s
// MODIFICA: servo tetto con movimento graduale (~2s) via FreeRTOS
// + Integrazione MQTT (LegmaMiteo pattern via Tailscale Funnel) e ArduinoOTA
// ============================================================

#include <Arduino.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Adafruit_SHT4x.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

WiFiClientSecure secureClient;

#define MQTT_HOST "legmamiteo-server.tail1c95b4.ts.net"
#define MQTT_PORT 8443
const char *SERRA_ID = "serra-001";
const char *SERRA_MQTT_USER = "serra-esp32";
const char *SERRA_MQTT_PASSWORD = "9ETvWVXVf2qbT2sy3yOz9XylPwIp8qdo";

const char *MQTT_TOPIC_AUTOMATION = "legmamiteo/automation/serra";

char otaControlTopic[64]; // "serra/<SERRA_ID>/ota/comando", costruito in setup()
char otaStatusTopic[64];  // "serra/<SERRA_ID>/ota/stato", costruito in setup()

WiFiClientSecure mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

// Forward declaration necessaria: applyAutomationDecisions() (più sotto) la
// chiama, ma setRoof() è definita fisicamente più avanti nel file (sezione
// task servo). Senza questa riga il file non compila.
void setRoof(bool open);

// ------------------------------------------------------------
// Stato automazione ricevuto da LegmaMiteo
// ------------------------------------------------------------
struct AutomationData
{
    bool roofShouldClose = false;
    bool lightsShouldBeOff = false;
    bool lightningAlert = false;
    String condition = "sconosciuta";
    unsigned long lastUpdateMs = 0;
    bool everReceived = false;
} automationData;

const unsigned long AUTOMATION_STALE_AFTER_MS = 15UL * 60UL * 1000UL; // 15 minuti

// ------------------------------------------------------------
// OTA gated via MQTT
// ------------------------------------------------------------
const bool OTA_ABILITATO = true;
const unsigned long OTA_SESSIONE_MAX_MS = 10UL * 60UL * 1000UL; // failsafe 10 minuti

volatile bool modalitaOTARichiesta = false;
volatile bool otaInCorso = false;
bool otaInizializzato = false;
unsigned long otaSessioneIniziataMs = 0;

#define DEBUG_ULTRASUONI true
#define DEBUG_SENSORI false // debug sensori silenziato
#define DEBUG_FIREBASE true
#define DEBUG_WIFI true
#define DEBUG_ATTUATORI true
#define DEBUG_LCD false
#define DEBUG_LOOP_TIMING false

#define DBG_US(...)                     \
    do                                  \
    {                                   \
        if (DEBUG_ULTRASUONI)           \
        {                               \
            Serial.print("[US]   ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)
#define DBG_SEN(...)                    \
    do                                  \
    {                                   \
        if (DEBUG_SENSORI)              \
        {                               \
            Serial.print("[SEN]  ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)
#define DBG_FB(...)                     \
    do                                  \
    {                                   \
        if (DEBUG_FIREBASE)             \
        {                               \
            Serial.print("[FB]   ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)
#define DBG_WIFI(...)                   \
    do                                  \
    {                                   \
        if (DEBUG_WIFI)                 \
        {                               \
            Serial.print("[WIFI] ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)
#define DBG_ATT(...)                    \
    do                                  \
    {                                   \
        if (DEBUG_ATTUATORI)            \
        {                               \
            Serial.print("[ATT]  ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)
#define DBG_LCD(...)                    \
    do                                  \
    {                                   \
        if (DEBUG_LCD)                  \
        {                               \
            Serial.print("[LCD]  ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)
#define DBG_LOOP(...)                   \
    do                                  \
    {                                   \
        if (DEBUG_LOOP_TIMING)          \
        {                               \
            Serial.print("[LOOP] ");    \
            Serial.printf(__VA_ARGS__); \
        }                               \
    } while (0)

// --- COSTANTI FISICHE ---
const float VELOCITA_SUONO_CM_US = 0.034f;
const float TANK_HEIGHT = 21.37f;
const float SOGLIA_ACQUA_MINIMA = 10.0f;

// --- COSTANTI CALIBRAZIONE ACQUA ---
const float DIST_PIENA = 4.56f;  // cm quando tanica piena
const float DIST_VUOTA = 16.37f; // cm quando rimangono 5cm (= 0% utile)

// --- COSTANTI TIMING (ms) ---
const unsigned long INTERVALLO_SENSORI = 500;
const unsigned long INTERVALLO_LCD = 3000;
const unsigned long INTERVALLO_FIREBASE = 1000;
const unsigned long INTERVALLO_COMANDI = 500;
const unsigned long INTERVALLO_WIFI_CHECK = 30000;
const unsigned long DHT_WARMUP_MS = 2000;

// --- ULTRASUONI ---
const unsigned long INTERVALLO_ULTRASUONI = 7000;
const int CAMPIONI_ULTRASUONI = 3;
const unsigned long PULSIN_TIMEOUT_US = 30000;
const unsigned long PAUSA_TRA_PING_MS = 50;

// --- SERVO TETTO ---
const int SERVO_A_APERTO = 0;
const int SERVO_B_APERTO = 180;
const int SERVO_A_CHIUSO = 90;
const int SERVO_B_CHIUSO = 90;
const unsigned long SERVO_STEP_DELAY_MS = 22;

// --- PESI MEDIA PONDERATA TEMP/HUM ---
const float PESO_DHT = 0.3f;
const float PESO_SHT = 0.7f;

// --- CALIBRAZIONE LUCE ---
const int LUCE_MIN = 2000;
const int LUCE_MAX = 4095;

// --- PIN ---
const int luci = 23;
const int sensoreTerreno = 35;
const int sensoreTerreno2 = 36;
const int trigPin = 14;
const int echoPin = 27;
const int lightPin = 34;
const int umidificatore1 = 32;
const int umidificatore2 = 33;
const int ventolaSerra = 25;
const int ventolaScatola = 26;
const int pompa2 = 13;

// ============================================================
// --- VARIABILI GLOBALI ---
// ============================================================

struct SensorData
{
    float temperature;
    float humidity;
    int soilMoisture;
    bool valid;
};
SensorData lastSensorData = {-99.0f, -99.0f, 0, false};
float lastWaterLevel = -1.0f;
int lastLightPct = 0;

unsigned long loopCount = 0;
unsigned long usAvvii = 0;
unsigned long usSuccessi = 0;
unsigned long usErrori = 0;
unsigned long fbInvii = 0;
unsigned long fbErrori = 0;

unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSendData = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastWifiCheck = 0;

bool roofOpen = false;

// --- Task servo: stato condiviso ---
volatile bool servoTargetOpen = false;
volatile bool servoMoving = false;
SemaphoreHandle_t servoMutex = NULL;

#define DHTPIN 5
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
unsigned long dhtReadyAt = 0;

Adafruit_SHT4x sht45 = Adafruit_SHT4x();
bool sht45Ok = false;

Servo servoA;
Servo servoB;
const int servoAPin = 4;
const int servoBPin = 16;

#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

const char *ssid = "Home&Life SuperWiFi-6CCD";
const char *password = "44AMD8347RH7GMB3";

const char *firebaseCmdUrl = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/commands.json";
const char *firebaseSensorsUrl = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/sensors.json";

// ============================================================
// --- WIFI SM ---
// ============================================================
enum WiFiStato
{
    WIFI_OK,
    WIFI_RECONNECTING,
    WIFI_FALLBACK_SCAN,
    WIFI_FALLBACK_WAIT_SSID,
    WIFI_FALLBACK_WAIT_PASS,
    WIFI_FALLBACK_CONNECTING
};
struct WiFiSM
{
    WiFiStato stato = WIFI_OK;
    int tentativi = 0;
    unsigned long tStato = 0;
    String nuovoSSID = "";
    String nuovaPassword = "";
} wifiSM;

// ============================================================
// --- OTA FUNZIONI ---
// ============================================================
void setupOTA()
{
    ArduinoOTA.setHostname(SERRA_ID);
    ArduinoOTA
        .onStart([]()
                 {
             otaInCorso = true;
             String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
             DBG_FB("OTA: inizio aggiornamento %s\n", type.c_str()); })
        .onEnd([]()
               { DBG_FB("OTA: completato, riavvio...\n");
                    otaInCorso = false; })
        .onProgress([](unsigned int progress, unsigned int total)
                    {
             static unsigned long ultimaStampa = 0;
             if (millis() - ultimaStampa > 2000) {
                 DBG_FB("OTA: %u%%\n", (progress / (total / 100)));
                 ultimaStampa = millis();
             } })
        .onError([](ota_error_t error)
                 {
             otaInCorso = false;
             DBG_FB("OTA: errore [%u]\n", error); });
    ArduinoOTA.begin();
    otaInizializzato = true;
    DBG_FB("OTA: ArduinoOTA inizializzato e in ascolto\n");
}

// ============================================================
// --- CALLBACK MQTT ---
// ============================================================
void pubblicaStatoOta(bool attivo)
{
    if (!mqttClient.connected()) return;
    mqttClient.publish(otaStatusTopic, attivo ? "attivo" : "disattivo", true);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
    if (strcmp(topic, otaControlTopic) == 0)
    {
        String msg;
        msg.reserve(length);
        for (unsigned int i = 0; i < length; i++)
            msg += (char)payload[i];
        msg.trim();
        msg.toLowerCase();

        bool nuovoStato = (msg == "on" || msg == "1" || msg == "true" || msg == "start");
        if (nuovoStato != modalitaOTARichiesta)
        {
            DBG_FB("OTA: comando ricevuto -> %s\n", nuovoStato ? "ENTRA in modalita' OTA" : "ESCI da modalita' OTA");
            if (nuovoStato && !otaInizializzato)
                setupOTA();
            if (nuovoStato)
                otaSessioneIniziataMs = millis();
        }
        modalitaOTARichiesta = nuovoStato;
        pubblicaStatoOta(nuovoStato);
        return;
    }

    if (strcmp(topic, MQTT_TOPIC_AUTOMATION) == 0)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err)
        {
            DBG_FB("MQTT: errore parsing automazione: %s\n", err.c_str());
            return;
        }
        automationData.roofShouldClose = doc["roof_should_close"] | false;
        automationData.lightsShouldBeOff = doc["lights_should_be_off"] | false;
        automationData.lightningAlert = doc["lightning_alert"] | false;
        automationData.condition = doc["condition"].as<String>();
        automationData.lastUpdateMs = millis();
        automationData.everReceived = true;
        DBG_FB("MQTT: automazione -> roof_close=%d lights_off=%d cond=%s\n",
               automationData.roofShouldClose, automationData.lightsShouldBeOff,
               automationData.condition.c_str());
    }
}

bool connectMqttLegmamiteo()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;
    if (mqttClient.connected())
        return true;

    mqttWifiClient.setInsecure();
    mqttWifiClient.setTimeout(10000); // evita blocchi lunghi se il broker non risponde
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setBufferSize(512);
    mqttClient.setCallback(onMqttMessage);

    DBG_FB("MQTT: connessione a %s:%d...\n", MQTT_HOST, MQTT_PORT);
    if (mqttClient.connect(SERRA_ID, SERRA_MQTT_USER, SERRA_MQTT_PASSWORD))
    {
        DBG_FB("MQTT: connesso\n");
        pubblicaStatoOta(false); // baseline alla (ri)connessione
        mqttClient.subscribe(MQTT_TOPIC_AUTOMATION, 1);
        if (OTA_ABILITATO)
            mqttClient.subscribe(otaControlTopic, 1);
        return true;
    }

    DBG_FB("MQTT: connessione fallita, rc=%d\n", mqttClient.state());
    return false;
}

void tickMqttLegmamiteo()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (!mqttClient.connected())
    {
        static unsigned long lastRetry = 0;
        unsigned long now = millis();
        if (now - lastRetry >= 5000)
        {
            lastRetry = now;
            connectMqttLegmamiteo();
        }
        return;
    }
    mqttClient.loop();

    if (modalitaOTARichiesta && !otaInCorso &&
        (millis() - otaSessioneIniziataMs >= OTA_SESSIONE_MAX_MS))
    {
        DBG_FB("OTA: timeout di sicurezza raggiunto, esco dalla modalita'\n");
        modalitaOTARichiesta = false;
    }
}

void tickOta()
{
    if (modalitaOTARichiesta && otaInizializzato)
        ArduinoOTA.handle();
}

bool isAutomationDataFresh()
{
    if (!automationData.everReceived)
        return false;
    return (millis() - automationData.lastUpdateMs) < AUTOMATION_STALE_AFTER_MS;
}

// ============================================================
// --- ARBITRAGGIO TETTO E LUCI ---
// ============================================================
void applyAutomationDecisions(bool appRoofOpenRequested, bool appManualOverrideActive)
{
    if (isAutomationDataFresh() && automationData.roofShouldClose)
    {
        if (roofOpen)
            setRoof(false);
        return;
    }

    if (appRoofOpenRequested != roofOpen)
        setRoof(appRoofOpenRequested);

    if (isAutomationDataFresh() && !appManualOverrideActive)
    {
        bool shouldBeOn = !automationData.lightsShouldBeOff;
        digitalWrite(luci, shouldBeOn ? HIGH : LOW);
    }
}

// ============================================================
// --- TASK FREERTOS: ULTRASUONI (Core 0) ---
// ============================================================
TaskHandle_t taskUltrasuoniHandle = NULL;

void taskUltrasuoni(void *param)
{
    DBG_US("Task avviato su Core %d\n", xPortGetCoreID());
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(INTERVALLO_ULTRASUONI));

        usAvvii++;
        float somma = 0.0f;
        int validi = 0;

        DBG_US("--- Inizio misura #%lu (campioni=%d) ---\n", usAvvii, CAMPIONI_ULTRASUONI);

        for (int i = 0; i < CAMPIONI_ULTRASUONI; i++)
        {
            digitalWrite(trigPin, LOW);
            delayMicroseconds(4);
            digitalWrite(trigPin, HIGH);
            delayMicroseconds(10);
            digitalWrite(trigPin, LOW);

            long duration = pulseIn(echoPin, HIGH, PULSIN_TIMEOUT_US);

            if (duration == 0)
            {
                DBG_US("  Campione %d/%d: TIMEOUT\n", i + 1, CAMPIONI_ULTRASUONI);
            }
            else
            {
                float dist = duration * VELOCITA_SUONO_CM_US / 2.0f;
                float livello = constrain(
                    (DIST_VUOTA - dist) / (DIST_VUOTA - DIST_PIENA) * 100.0f,
                    0.0f, 100.0f);
                somma += livello;
                validi++;
                DBG_US("  Campione %d/%d: %ldus -> %.1fcm -> %.1f%%\n",
                       i + 1, CAMPIONI_ULTRASUONI, duration, dist, livello);
            }

            if (i < CAMPIONI_ULTRASUONI - 1)
            {
                vTaskDelay(pdMS_TO_TICKS(PAUSA_TRA_PING_MS));
            }
        }

        if (validi > 0)
        {
            lastWaterLevel = somma / validi;
            usSuccessi++;
            DBG_US("OK: media=%.1f%% (%d/%d validi) [tot ok=%lu err=%lu]\n",
                   lastWaterLevel, validi, CAMPIONI_ULTRASUONI, usSuccessi, usErrori);
        }
        else
        {
            usErrori++;
            lastWaterLevel = -1.0f;
            DBG_US("ERRORE: 0 campioni validi [tot ok=%lu err=%lu]\n", usSuccessi, usErrori);
        }
    }
}

// ============================================================
// --- TASK FREERTOS: SERVO TETTO (Core 1) ---
// ============================================================
TaskHandle_t taskServoHandle = NULL;

void taskServo(void *param)
{
    DBG_ATT("Task servo avviato su Core %d\n", xPortGetCoreID());

    int posA = SERVO_A_CHIUSO;
    int posB = SERVO_B_CHIUSO;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool targetOpen;
        xSemaphoreTake(servoMutex, portMAX_DELAY);
        targetOpen = servoTargetOpen;
        servoMoving = true;
        xSemaphoreGive(servoMutex);

        int targetA = targetOpen ? SERVO_A_APERTO : SERVO_A_CHIUSO;
        int targetB = targetOpen ? SERVO_B_APERTO : SERVO_B_CHIUSO;

        DBG_ATT("Servo: movimento %s | A: %d->%d  B: %d->%d\n",
                targetOpen ? "APERTO" : "CHIUSO", posA, targetA, posB, targetB);

        int stepsA = abs(targetA - posA);
        int stepsB = abs(targetB - posB);
        int steps = max(stepsA, stepsB);

        for (int i = 1; i <= steps; i++)
        {
            int newA = posA + (int)roundf((float)(targetA - posA) * i / steps);
            int newB = posB + (int)roundf((float)(targetB - posB) * i / steps);

            servoA.write(newA);
            servoB.write(newB);

            vTaskDelay(pdMS_TO_TICKS(SERVO_STEP_DELAY_MS));

            if (ulTaskNotifyTake(pdTRUE, 0) > 0)
            {
                posA = newA;
                posB = newB;
                DBG_ATT("Servo: movimento interrotto a step %d/%d (nuovo comando)\n", i, steps);
                xTaskNotifyGive(taskServoHandle);
                goto next_command;
            }
        }

        servoA.write(targetA);
        servoB.write(targetB);
        posA = targetA;
        posB = targetB;
        DBG_ATT("Servo: %s completato (A=%d B=%d)\n",
                targetOpen ? "APERTO" : "CHIUSO", posA, posB);

    next_command:
        xSemaphoreTake(servoMutex, portMAX_DELAY);
        servoMoving = false;
        xSemaphoreGive(servoMutex);
    }
}

void setRoof(bool open)
{
    xSemaphoreTake(servoMutex, portMAX_DELAY);
    servoTargetOpen = open;
    xSemaphoreGive(servoMutex);

    xTaskNotifyGive(taskServoHandle);

    roofOpen = open;
    DBG_ATT("Tetto: richiesta %s\n", open ? "APERTURA" : "CHIUSURA");
}

// ============================================================
// --- PROTOTIPI ---
// ============================================================
void connectWiFiBlocking();
void tickWiFiSM();
void sendToFirebase(const SensorData &s, float waterLevel, int lightPct);
void updateLCD(float umiditaTerreno, float acqua, float temp, float umidita, int luce);
void readFirebaseCommands();
void startIrrigation(float waterLevel);
void stopIrrigation();
void startHumidifier();
void stopHumidifier();
void startFan();
void stopFan();
SensorData readNewSensors();
void printDebugSummary();
void scanI2C();

// ============================================================
// --- SETUP ---
// ============================================================
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("\n\n============================================");
    Serial.println("  SegmaFuckingSirra - DEBUG MODE ATTIVO");
    Serial.println("============================================");
    Serial.printf("  Chip: %s  CPU: %dMHz  Heap: %d bytes\n",
                  ESP.getChipModel(), ESP.getCpuFreqMHz(), ESP.getFreeHeap());
    Serial.println("  PIN MAP:");
    Serial.printf("  TRIG=%d  ECHO=%d  DHT=%d  LUCE=%d\n", trigPin, echoPin, DHTPIN, lightPin);
    Serial.printf("  TERRENO=%d/%d  SERVO=%d/%d\n", sensoreTerreno, sensoreTerreno2, servoAPin, servoBPin);
    Serial.printf("  UMIDIF=%d/%d  VENTOLA=%d  POMPA=%d  LUCI=%d\n",
                  umidificatore1, umidificatore2, ventolaSerra, pompa2, luci);
    Serial.printf("  CALIBRAZIONE ACQUA: piena=%.2fcm vuota(0%%)=%.2fcm\n", DIST_PIENA, DIST_VUOTA);
    Serial.printf("  INTERVALLO_ULTRASUONI=%lums  CAMPIONI=%d\n",
                  INTERVALLO_ULTRASUONI, CAMPIONI_ULTRASUONI);
    Serial.printf("  SERVO: aperto(A=%d B=%d) chiuso(A=%d B=%d) step_delay=%lums\n",
                  SERVO_A_APERTO, SERVO_B_APERTO, SERVO_A_CHIUSO, SERVO_B_CHIUSO, SERVO_STEP_DELAY_MS);
    Serial.println("============================================\n");

    pinMode(trigPin, OUTPUT);
    pinMode(echoPin, INPUT);
    pinMode(lightPin, INPUT);
    digitalWrite(trigPin, LOW);

    pinMode(umidificatore1, OUTPUT);
    digitalWrite(umidificatore1, LOW);
    pinMode(umidificatore2, OUTPUT);
    digitalWrite(umidificatore2, LOW);
    pinMode(ventolaSerra, OUTPUT);
    digitalWrite(ventolaSerra, LOW);
    pinMode(ventolaScatola, OUTPUT);
    digitalWrite(ventolaScatola, LOW);
    pinMode(pompa2, OUTPUT);
    digitalWrite(pompa2, LOW);
    pinMode(luci, OUTPUT);
    digitalWrite(luci, LOW);

    servoA.attach(servoAPin);
    servoA.write(SERVO_A_CHIUSO);
    servoB.attach(servoBPin);
    servoB.write(SERVO_B_CHIUSO);
    roofOpen = false;
    servoTargetOpen = false;

    Wire.begin(21, 22);
    scanI2C();

    dht.begin();
    dhtReadyAt = millis() + DHT_WARMUP_MS;

    if (sht45.begin())
    {
        sht45Ok = true;
        sht45.setPrecision(SHT4X_HIGH_PRECISION);
        sht45.setHeater(SHT4X_NO_HEATER);
        Serial.println("[INIT] SHT45: OK");
    }
    else
    {
        sht45Ok = false;
        Serial.println("[INIT] SHT45: NON TROVATO");
    }

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Serra avviata...");

    connectWiFiBlocking();

    if (WiFi.status() == WL_CONNECTED)
    {
        bool dnsOk = WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                                  IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
        DBG_WIFI("Riconfigurazione DNS (8.8.8.8/1.1.1.1): %s\n", dnsOk ? "OK" : "FALLITA");
    }

    secureClient.setInsecure();
    DBG_WIFI("TLS client configurato in modalità insecure\n");

    // Inizializzazione topic OTA e connessione broker MQTT LegmaMiteo
    snprintf(otaControlTopic, sizeof(otaControlTopic), "serra/%s/ota/comando", SERRA_ID);
    snprintf(otaStatusTopic, sizeof(otaStatusTopic), "serra/%s/ota/stato", SERRA_ID);
    connectMqttLegmamiteo();

    servoMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(
        taskUltrasuoni,
        "UltrasuoniTask",
        4096,
        NULL,
        1,
        &taskUltrasuoniHandle,
        0);

    xTaskCreatePinnedToCore(
        taskServo,
        "ServoTask",
        2048,
        NULL,
        2,
        &taskServoHandle,
        1);
    Serial.println("[INIT] Setup completato. Avvio loop.\n");
}

// ============================================================
// --- LOOP ---
// ============================================================
void loop()
{
    unsigned long now = millis();
    loopCount++;

    tickWiFiSM();
    tickMqttLegmamiteo();
    tickOta();

    if (now - lastWifiCheck >= INTERVALLO_WIFI_CHECK)
    {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED && wifiSM.stato == WIFI_OK)
        {
            DBG_WIFI("Connessione persa!\n");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
            wifiSM.stato = WIFI_RECONNECTING;
            wifiSM.tentativi = 0;
            wifiSM.tStato = now;
        }
    }

    if (now - lastSensorRead >= INTERVALLO_SENSORI)
    {
        lastSensorRead = now;
        lastSensorData = readNewSensors();

        int luceRaw = analogRead(lightPin);
        lastLightPct = constrain(
            (int)((float)(luceRaw - LUCE_MIN) / (LUCE_MAX - LUCE_MIN) * 100.0f),
            0, 100);
    }

    if (now - lastDisplayUpdate >= INTERVALLO_LCD)
    {
        lastDisplayUpdate = now;
        updateLCD(lastSensorData.soilMoisture, lastWaterLevel,
                  lastSensorData.temperature, lastSensorData.humidity, lastLightPct);
        printDebugSummary();
    }

    // Durante un aggiornamento OTA effettivo (otaInCorso), le chiamate HTTP a
    // Firebase possono bloccare il loop fino a 5s e far perdere pacchetti
    // ArduinoOTA: le saltiamo finché il flash non è terminato.
    if (!otaInCorso)
    {
        if (now - lastSendData >= INTERVALLO_FIREBASE)
        {
            lastSendData = now;
            sendToFirebase(lastSensorData, lastWaterLevel, lastLightPct);
        }

        if (now - lastCommandCheck >= INTERVALLO_COMANDI)
        {
            lastCommandCheck = now;
            readFirebaseCommands();
        }
    }

    DBG_LOOP("loop #%lu heap=%d\n", loopCount, ESP.getFreeHeap());
}

// ============================================================
// --- RIEPILOGO DEBUG ---
// ============================================================
void printDebugSummary()
{
    bool moving;
    xSemaphoreTake(servoMutex, portMAX_DELAY);
    moving = servoMoving;
    xSemaphoreGive(servoMutex);

    Serial.println("\n========== RIEPILOGO ==========");
    Serial.printf("  Uptime: %lus  Loop: %lu  Heap: %d\n",
                  millis() / 1000, loopCount, ESP.getFreeHeap());
    Serial.println("  SENSORI:");
    if (lastSensorData.valid)
    {
        Serial.printf("    T=%.1f°C  H=%.1f%%\n",
                      lastSensorData.temperature, lastSensorData.humidity);
    }
    else
    {
        Serial.println("    T/H: ERRORE");
    }
    Serial.printf("    Suolo=%d%%  Luce=%d%%\n", lastSensorData.soilMoisture, lastLightPct);
    if (lastWaterLevel >= 0)
    {
        Serial.printf("    Acqua=%.1f%%\n", lastWaterLevel);
    }
    else
    {
        Serial.println("    Acqua: in attesa prima misura...");
    }
    Serial.printf("  ULTRASUONI: misure=%lu ok=%lu err=%lu (%.0f%%)\n",
                  usAvvii, usSuccessi, usErrori,
                  usAvvii > 0 ? (float)usSuccessi / usAvvii * 100.0f : 0.0f);
    Serial.printf("  WIFI: %s  RSSI:%ddBm\n",
                  WiFi.status() == WL_CONNECTED ? "OK" : "DISCONNESSO", WiFi.RSSI());
    Serial.printf("  MQTT LegmaMiteo: %s  OTA:%s%s\n",
                  mqttClient.connected() ? "OK" : "DISCONNESSO",
                  modalitaOTARichiesta ? "ATTIVA" : "spenta",
                  otaInCorso ? " (flash in corso)" : "");
    Serial.printf("  FIREBASE: invii=%lu err=%lu\n", fbInvii, fbErrori);
    Serial.printf("  ATTUATORI: pompa=%s umid=%s/%s ventola=%s luci=%s tetto=%s%s\n",
                  digitalRead(pompa2) ? "ON" : "OFF",
                  digitalRead(umidificatore1) ? "ON" : "OFF",
                  digitalRead(umidificatore2) ? "ON" : "OFF",
                  digitalRead(ventolaSerra) ? "ON" : "OFF",
                  digitalRead(luci) ? "ON" : "OFF",
                  roofOpen ? "APERTO" : "CHIUSO",
                  moving ? " (in movimento...)" : "");
    Serial.println("================================\n");
}

// ============================================================
// --- SCAN I2C ---
// ============================================================
void scanI2C()
{
    Serial.println("[I2C] Scansione dispositivi...");
    int trovati = 0;
    for (byte addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            trovati++;
        }
    }
    Serial.printf("[I2C] Totale: %d dispositivo/i\n", trovati);
}

// ============================================================
// --- WIFI BLOCKING ---
// ============================================================
void connectWiFiBlocking()
{
    DBG_WIFI("Connessione a '%s'...\n", ssid);
    WiFi.begin(ssid, password);
    unsigned long t0 = millis(), tPoll = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000)
    {
        if (millis() - tPoll >= 500)
        {
            tPoll = millis();
            Serial.print(".");
        }
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        DBG_WIFI("\nConnesso! IP:%s RSSI:%ddBm\n",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
        wifiSM.stato = WIFI_OK;
    }
    else
    {
        DBG_WIFI("\nFallito, avvio fallback\n");
        wifiSM.tStato = millis();
        wifiSM.stato = WIFI_FALLBACK_SCAN;
    }
}

// ============================================================
// --- WIFI SM ---
// ============================================================
void tickWiFiSM()
{
    unsigned long now = millis();
    switch (wifiSM.stato)
    {
    case WIFI_OK:
        break;
    case WIFI_RECONNECTING:
        if (now - wifiSM.tStato >= 500)
        {
            wifiSM.tStato = now;
            wifiSM.tentativi++;
            if (WiFi.status() == WL_CONNECTED)
            {
                WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                            IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
                wifiSM.stato = WIFI_OK;
            }
            else if (wifiSM.tentativi >= 20)
            {
                wifiSM.stato = WIFI_OK;
            }
        }
        break;
    case WIFI_FALLBACK_SCAN:
    {
        int n = WiFi.scanNetworks();
        wifiSM.stato = WIFI_FALLBACK_WAIT_SSID;
        wifiSM.tStato = now;
        break;
    }
    case WIFI_FALLBACK_WAIT_SSID:
        if (Serial.available())
        {
            wifiSM.nuovoSSID = Serial.readStringUntil('\n');
            wifiSM.nuovoSSID.trim();
            if (wifiSM.nuovoSSID.length() > 0)
            {
                wifiSM.stato = WIFI_FALLBACK_WAIT_PASS;
                wifiSM.tStato = now;
            }
        }
        break;
    case WIFI_FALLBACK_WAIT_PASS:
        if (Serial.available())
        {
            wifiSM.nuovaPassword = Serial.readStringUntil('\n');
            wifiSM.nuovaPassword.trim();
            wifiSM.stato = WIFI_FALLBACK_CONNECTING;
            wifiSM.tStato = now;
            wifiSM.tentativi = 0;
            if (wifiSM.nuovaPassword.length() > 0)
                WiFi.begin(wifiSM.nuovoSSID.c_str(), wifiSM.nuovaPassword.c_str());
            else
                WiFi.begin(wifiSM.nuovoSSID.c_str());
        }
        else if (now - wifiSM.tStato >= 30000)
        {
            wifiSM.stato = WIFI_FALLBACK_CONNECTING;
            wifiSM.tStato = now;
            wifiSM.tentativi = 0;
            WiFi.begin(wifiSM.nuovoSSID.c_str());
        }
        break;
    case WIFI_FALLBACK_CONNECTING:
        if (now - wifiSM.tStato >= 500)
        {
            wifiSM.tStato = now;
            wifiSM.tentativi++;
            if (WiFi.status() == WL_CONNECTED)
            {
                wifiSM.stato = WIFI_OK;
            }
            else if (wifiSM.tentativi >= 30)
            {
                wifiSM.stato = WIFI_OK;
            }
        }
        break;
    }
}

void sendToFirebase(const SensorData &s, float waterLevel, int lightPct)
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    JsonDocument doc;
    if (s.valid)
    {
        doc["temperature"] = serialized(String(s.temperature, 1));
        doc["humidity"] = serialized(String(s.humidity, 1));
    }
    else
    {
        doc["sensorError"] = true;
    }
    doc["soil"] = s.soilMoisture;
    if (waterLevel >= 0)
        doc["remWater"] = serialized(String(waterLevel, 1));
    else
        doc["waterSensorError"] = true;
    doc["light"] = lightPct;

    String jsonData;
    serializeJson(doc, jsonData);

    HTTPClient http;
    if (!http.begin(secureClient, firebaseSensorsUrl))
    {
        fbErrori++;
        return;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    int code = http.PUT(jsonData);
    fbInvii++;

    if (code > 0)
    {
        if (code != 200)
        {
            fbErrori++;
        }
    }
    else
    {
        fbErrori++;
    }
    http.end();
}

// ============================================================
// --- LCD ---
// ============================================================
void updateLCD(float umiditaTerreno, float acqua, float temp, float umidita, int luce)
{
    static int displayState = 0;
    lcd.clear();
    switch (displayState)
    {
    case 0:
        lcd.setCursor(0, 0);
        lcd.print("Umid.T: ");
        lcd.print(umiditaTerreno, 0);
        lcd.print("%");
        lcd.setCursor(0, 1);
        lcd.print("H2O: ");
        if (acqua >= 0)
        {
            lcd.print(acqua, 1);
            lcd.print("%");
        }
        else
        {
            lcd.print("attesa...");
        }
        break;
    case 1:
        lcd.setCursor(0, 0);
        lcd.print("Temp: ");
        lcd.print(temp, 1);
        lcd.print((char)223);
        lcd.print("C");
        lcd.setCursor(0, 1);
        lcd.print("Umid: ");
        lcd.print(umidita, 1);
        lcd.print("%");
        break;
    case 2:
        lcd.setCursor(0, 0);
        lcd.print("Luce: ");
        lcd.print(luce);
        lcd.print("%");
        lcd.setCursor(0, 1);
        lcd.print("Tetto: ");
        lcd.print(roofOpen ? "APERTO" : "CHIUSO");
        break;
    }
    displayState = (displayState + 1) % 3;
}

void readFirebaseCommands()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    HTTPClient http;
    if (!http.begin(secureClient, firebaseCmdUrl))
        return;

    http.setTimeout(5000);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err)
        {
            if (doc.containsKey("lightOn"))
            {
                bool on = doc["lightOn"];
                digitalWrite(luci, on ? HIGH : LOW);
            }
            if (doc.containsKey("roofOpen") || doc.containsKey("manualOverride"))
            {
                applyAutomationDecisions(doc["roofOpen"] | roofOpen,
                                          doc["manualOverride"] | false);
            }
            if (doc.containsKey("irrigationOn"))
            {
                doc["irrigationOn"] ? startIrrigation(lastWaterLevel) : stopIrrigation();
            }
            if (doc.containsKey("humidifierOn"))
            {
                doc["humidifierOn"] ? startHumidifier() : stopHumidifier();
            }
            if (doc.containsKey("fanOn"))
            {
                doc["fanOn"] ? startFan() : stopFan();
            }
        }
    }
    http.end();
}

// ============================================================
// --- ATTUATORI ---
// ============================================================
void startIrrigation(float waterLevel)
{
    if (waterLevel >= 0 && waterLevel < SOGLIA_ACQUA_MINIMA)
    {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("ACQUA ESAURITA");
        lcd.setCursor(0, 1);
        lcd.print("Irrigaz. STOP");
        return;
    }
    if (waterLevel < 0)
        return;
    digitalWrite(pompa2, HIGH);
}
void stopIrrigation()
{
    digitalWrite(pompa2, LOW);
}
void startHumidifier()
{
    digitalWrite(umidificatore1, HIGH);
    digitalWrite(umidificatore2, HIGH);
}
void stopHumidifier()
{
    digitalWrite(umidificatore1, LOW);
    digitalWrite(umidificatore2, LOW);
}
void startFan()
{
    digitalWrite(ventolaSerra, HIGH);
}
void stopFan()
{
    digitalWrite(ventolaSerra, LOW);
}

// ============================================================
// --- LETTURA SENSORI ---
// ============================================================
SensorData readNewSensors()
{
    SensorData data;
    data.valid = false;
    data.temperature = -99.0f;
    data.humidity = -99.0f;
    data.soilMoisture = 0;

    float shtTemp = NAN, shtHum = NAN;
    if (sht45Ok)
    {
        sensors_event_t hev, tev;
        sht45.getEvent(&hev, &tev);
        shtTemp = tev.temperature;
        shtHum = hev.relative_humidity;
    }

    float tDHT = NAN, hDHT = NAN;
    if (millis() >= dhtReadyAt)
    {
        tDHT = dht.readTemperature();
        hDHT = dht.readHumidity();
    }

    bool dhtValido = !isnan(tDHT) && !isnan(hDHT);
    bool shtValido = sht45Ok && !isnan(shtTemp) && !isnan(shtHum);

    if (dhtValido && shtValido)
    {
        data.temperature = tDHT * PESO_DHT + shtTemp * PESO_SHT;
        data.humidity = hDHT * PESO_DHT + shtHum * PESO_SHT;
        data.valid = true;
    }
    else if (shtValido)
    {
        data.temperature = shtTemp;
        data.humidity = shtHum;
        data.valid = true;
    }
    else if (dhtValido)
    {
        data.temperature = tDHT;
        data.humidity = hDHT;
        data.valid = true;
    }

    int v1 = analogRead(sensoreTerreno);
    int v2 = analogRead(sensoreTerreno2);
    int avg = (v1 + v2) / 2;
    data.soilMoisture = constrain(map(avg, 4095, 0, 0, 100), 0, 100);

    return data;
}