// ============================================================
// SegmaFuckingSirra -67.0 V senza delay bloccanti
// DEBUG VERSION — fix ultrasuoni: timeout 100ms, core 1, priorità 2
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

// ============================================================
// --- DEBUG CONFIG ---
// ============================================================
#define DEBUG_ULTRASUONI   true
#define DEBUG_SENSORI      true
#define DEBUG_FIREBASE     true
#define DEBUG_WIFI         true
#define DEBUG_ATTUATORI    true
#define DEBUG_LCD          false
#define DEBUG_LOOP_TIMING  false

#define DBG_US(...)   do { if (DEBUG_ULTRASUONI) { Serial.print("[US]   "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_SEN(...)  do { if (DEBUG_SENSORI)    { Serial.print("[SEN]  "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_FB(...)   do { if (DEBUG_FIREBASE)   { Serial.print("[FB]   "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_WIFI(...) do { if (DEBUG_WIFI)       { Serial.print("[WIFI] "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_ATT(...)  do { if (DEBUG_ATTUATORI)  { Serial.print("[ATT]  "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_LCD(...)  do { if (DEBUG_LCD)        { Serial.print("[LCD]  "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_LOOP(...) do { if (DEBUG_LOOP_TIMING){ Serial.print("[LOOP] "); Serial.printf(__VA_ARGS__); } } while(0)

// --- COSTANTI FISICHE ---
const float VELOCITA_SUONO_CM_US = 0.034f;
const float TANK_HEIGHT          = 21.37f; // distanza sensore-fondo (tanica vuota)
const float SOGLIA_ACQUA_MINIMA  = 10.0f;  // % sotto cui la pompa viene bloccata

// --- COSTANTI CALIBRAZIONE ACQUA ---
const float DIST_PIENA  = 4.56f;   // distanza misurata con tanica piena
const float DIST_VUOTA  = 16.37f;  // distanza misurata con 5cm rimasti (= 0% utile)

// --- COSTANTI TIMING (ms) ---
const unsigned long INTERVALLO_SENSORI    = 500;
const unsigned long INTERVALLO_LCD        = 3000;
const unsigned long INTERVALLO_FIREBASE   = 1000;
const unsigned long INTERVALLO_COMANDI    = 500;
const unsigned long INTERVALLO_WIFI_CHECK = 30000;
const unsigned long DHT_WARMUP_MS         = 2000;

// --- COSTANTI ULTRASUONI ---
// TIMEOUT aumentato a 100ms per tollerare interruzioni del WiFi su Core 0
const int           CAMPIONI_ULTRASUONI   = 3;
const unsigned long PAUSA_TRA_PING_MS     = 35;
const unsigned long TIMEOUT_ECHO_MS       = 100;  // era 30, aumentato per fix WiFi

// --- PESI MEDIA PONDERATA ---
const float PESO_DHT = 0.3f;
const float PESO_SHT = 0.7f;

// --- CALIBRAZIONE LUCE ---
const int LUCE_MIN = 2000;  // valore raw con buio/ombra totale
const int LUCE_MAX = 4095;  // valore raw con piena luce

// --- PIN ---
const int luci            = 23;
const int sensoreTerreno  = 35;
const int sensoreTerreno2 = 36;
const int trigPin         = 14;
const int echoPin         = 27;
const int lightPin        = 34;
const int umidificatore1  = 32;
const int umidificatore2  = 33;
const int ventolaSerra    = 25;
const int ventolaScatola  = 26;
const int pompa2          = 13;

// ============================================================
// --- VARIABILI GLOBALI ---
// ============================================================

struct SensorData {
    float temperature;
    float humidity;
    int   soilMoisture;
    bool  valid;
};
SensorData lastSensorData = { -99.0f, -99.0f, 0, false };
float      lastWaterLevel = -1.0f;
int        lastLightPct   = 0;

unsigned long loopCount   = 0;
unsigned long usAvvii     = 0;
unsigned long usSuccessi  = 0;
unsigned long usErrori    = 0;
unsigned long fbInvii     = 0;
unsigned long fbErrori    = 0;

unsigned long lastSensorRead    = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSendData      = 0;
unsigned long lastCommandCheck  = 0;
unsigned long lastWifiCheck     = 0;

bool roofOpen = false;

#define DHTPIN  5
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

const char* ssid               = "LAPTOP1234";
const char* password           = "12345678";
const char* firebaseCmdUrl     = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/commands.json";
const char* firebaseSensorsUrl = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/sensors.json";

enum WiFiStato {
    WIFI_OK, WIFI_RECONNECTING, WIFI_FALLBACK_SCAN,
    WIFI_FALLBACK_WAIT_SSID, WIFI_FALLBACK_WAIT_PASS, WIFI_FALLBACK_CONNECTING
};
struct WiFiSM {
    WiFiStato     stato         = WIFI_OK;
    int           tentativi     = 0;
    unsigned long tStato        = 0;
    String        nuovoSSID     = "";
    String        nuovaPassword = "";
} wifiSM;

// ============================================================
// --- MACCHINA A STATI ULTRASUONI ---
// ============================================================
enum UltrasuoniStato {
    US_IDLE, US_TRIGGER_LOW, US_TRIGGER_HIGH,
    US_WAIT_ECHO_START, US_WAIT_ECHO_END, US_PAUSA, US_DONE
};
const char* usStatoNome[] = {
    "IDLE","TRIGGER_LOW","TRIGGER_HIGH",
    "WAIT_ECHO_START","WAIT_ECHO_END","PAUSA","DONE"
};
struct UltrasuoniSM {
    UltrasuoniStato stato     = US_IDLE;
    UltrasuoniStato statoPrec = US_IDLE;
    int             campione  = 0;
    float           somma     = 0.0f;
    int             validi    = 0;
    unsigned long   tStato    = 0;
    float           risultato = -1.0f;
    bool            pronto    = false;
} usSM;

void startUltrasuoni() {
    usSM.stato     = US_TRIGGER_LOW;
    usSM.statoPrec = US_IDLE;
    usSM.campione  = 0;
    usSM.somma     = 0.0f;
    usSM.validi    = 0;
    usSM.pronto    = false;
    usSM.tStato    = micros();
    digitalWrite(trigPin, LOW);
    DBG_US("--- Nuova misura (TANK=%.0fcm, campioni=%d) ---\n", TANK_HEIGHT, CAMPIONI_ULTRASUONI);
}

void tickUltrasuoniSM() {
    if (usSM.stato == US_IDLE || usSM.stato == US_DONE) return;

    if (usSM.stato != usSM.statoPrec) {
        DBG_US("Stato: %s -> %s (campione %d/%d)\n",
               usStatoNome[usSM.statoPrec], usStatoNome[usSM.stato],
               usSM.campione, CAMPIONI_ULTRASUONI);
        usSM.statoPrec = usSM.stato;
    }

    switch (usSM.stato) {
        case US_TRIGGER_LOW:
            if (micros() - usSM.tStato >= 2) {
                digitalWrite(trigPin, HIGH);
                usSM.tStato = micros();
                usSM.stato  = US_TRIGGER_HIGH;
            }
            break;

        case US_TRIGGER_HIGH:
            if (micros() - usSM.tStato >= 10) {
                digitalWrite(trigPin, LOW);
                usSM.tStato = millis();
                usSM.stato  = US_WAIT_ECHO_START;
                DBG_US("Trigger inviato, attendo echo su GPIO%d...\n", echoPin);
            }
            break;

        case US_WAIT_ECHO_START:
            if (digitalRead(echoPin) == HIGH) {
                DBG_US("Echo START rilevato! (campione %d)\n", usSM.campione + 1);
                usSM.tStato = micros();
                usSM.stato  = US_WAIT_ECHO_END;
            } else if (millis() - usSM.tStato >= TIMEOUT_ECHO_MS) {
                DBG_US("TIMEOUT echo START campione %d\n", usSM.campione + 1);
                usSM.campione++;
                usSM.tStato = millis();
                usSM.stato  = (usSM.campione < CAMPIONI_ULTRASUONI) ? US_PAUSA : US_DONE;
                if (usSM.stato == US_DONE) {
                    usSM.risultato = (usSM.validi > 0) ? usSM.somma / usSM.validi : -1.0f;
                    usSM.pronto    = true;
                    if (usSM.validi == 0)
                        DBG_US("ERRORE: 0 campioni validi!\n");
                }
            }
            break;

        case US_WAIT_ECHO_END:
            if (digitalRead(echoPin) == LOW) {
                unsigned long duration = micros() - usSM.tStato;
                float distance = duration * VELOCITA_SUONO_CM_US / 2.0f;
                float livello  = constrain(
                    (DIST_VUOTA - distance) / (DIST_VUOTA - DIST_PIENA) * 100.0f,
                    0.0f, 100.0f
                );
                usSM.somma += livello;
                usSM.validi++;
                usSM.campione++;
                DBG_US("Echo END: %luus -> %.1fcm -> livello %.1f%%\n", duration, distance, livello);
                usSM.tStato = millis();
                usSM.stato  = (usSM.campione < CAMPIONI_ULTRASUONI) ? US_PAUSA : US_DONE;
                if (usSM.stato == US_DONE) {
                    usSM.risultato = usSM.somma / usSM.validi;
                    usSM.pronto    = true;
                    DBG_US("DONE: media %.1f%% su %d campioni\n", usSM.risultato, usSM.validi);
                }
            } else if (micros() - usSM.tStato > TIMEOUT_ECHO_MS * 1000UL) {
                DBG_US("TIMEOUT echo END campione %d\n", usSM.campione + 1);
                usSM.campione++;
                usSM.tStato = millis();
                usSM.stato  = (usSM.campione < CAMPIONI_ULTRASUONI) ? US_PAUSA : US_DONE;
                if (usSM.stato == US_DONE) {
                    usSM.risultato = (usSM.validi > 0) ? usSM.somma / usSM.validi : -1.0f;
                    usSM.pronto    = true;
                }
            }
            break;

        case US_PAUSA:
            if (millis() - usSM.tStato >= PAUSA_TRA_PING_MS) {
                digitalWrite(trigPin, LOW);
                usSM.tStato = micros();
                usSM.stato  = US_TRIGGER_LOW;
            }
            break;

        case US_DONE:
        case US_IDLE:
            break;
    }
}

// ============================================================
// --- TASK FREERTOS: ULTRASUONI ---
// FIX: spostato su Core 1 (stesso del loop) con priorità 2
// Il Core 0 è usato intensivamente dal WiFi dell'ESP32 e causava
// interruzioni che facevano perdere l'echo. Core 1 + priorità alta
// garantisce che il task venga schedulato in tempo.
// ============================================================
TaskHandle_t  taskUltrasuoniHandle = NULL;
volatile bool usTaskAbilitato      = false;

void taskUltrasuoni(void* param) {
    for (;;) {
        if (usTaskAbilitato) {
            startUltrasuoni();
            usAvvii++;
            unsigned long tStart = millis();
            while (usSM.stato != US_IDLE && usSM.stato != US_DONE) {
                tickUltrasuoniSM();
                delayMicroseconds(10);
                if (millis() - tStart > 500) {
                    DBG_US("TASK TIMEOUT forzato\n");
                    usSM.stato = US_IDLE;
                    break;
                }
            }
            if (usSM.pronto) {
                if (usSM.risultato >= 0) {
                    usSuccessi++;
                    DBG_US("Task OK: %.1f%% (ok=%lu err=%lu)\n",
                           usSM.risultato, usSuccessi, usErrori);
                } else {
                    usErrori++;
                    DBG_US("Task ERRORE (ok=%lu err=%lu)\n", usSuccessi, usErrori);
                }
                lastWaterLevel = usSM.risultato;
                usSM.pronto    = false;
                usSM.stato     = US_IDLE;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ============================================================
// --- PROTOTIPI ---
// ============================================================
void connectWiFiBlocking();
void tickWiFiSM();
void sendToFirebase(const SensorData& s, float waterLevel, int lightPct);
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
void setup() {
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
    Serial.printf("  TIMEOUT_ECHO=%lums\n", TIMEOUT_ECHO_MS);
    Serial.println("============================================\n");

    pinMode(trigPin,  OUTPUT);
    pinMode(echoPin,  INPUT);
    pinMode(lightPin, INPUT);
    digitalWrite(trigPin, LOW);

    int echoStato = digitalRead(echoPin);
    Serial.printf("[INIT] echoPin GPIO%d a riposo: %s %s\n",
                  echoPin, echoStato ? "HIGH" : "LOW",
                  echoStato ? "!! ATTENZIONE !!" : "(OK)");

    pinMode(umidificatore1, OUTPUT); digitalWrite(umidificatore1, LOW);
    pinMode(umidificatore2, OUTPUT); digitalWrite(umidificatore2, LOW);
    pinMode(ventolaSerra,   OUTPUT); digitalWrite(ventolaSerra,   LOW);
    pinMode(ventolaScatola, OUTPUT); digitalWrite(ventolaScatola, LOW);
    pinMode(pompa2,         OUTPUT); digitalWrite(pompa2,         LOW);
    pinMode(luci,           OUTPUT); digitalWrite(luci,           LOW);

    servoA.attach(servoAPin); servoA.write(0);
    servoB.attach(servoBPin); servoB.write(180);
    roofOpen = false;
    Serial.println("[INIT] Servo inizializzati (tetto chiuso)");

    Wire.begin(21, 22);
    Serial.println("[INIT] I2C avviato su SDA=21 SCL=22");
    scanI2C();

    dht.begin();
    dhtReadyAt = millis() + DHT_WARMUP_MS;
    Serial.printf("[INIT] DHT11 avviato, warmup fino a %lums\n", dhtReadyAt);

    if (sht45.begin()) {
        sht45Ok = true;
        sht45.setPrecision(SHT4X_HIGH_PRECISION);
        sht45.setHeater(SHT4X_NO_HEATER);
        Serial.println("[INIT] SHT45: OK");
    } else {
        sht45Ok = false;
        Serial.println("[INIT] SHT45: NON TROVATO");
    }

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0); lcd.print("Serra avviata...");
    Serial.println("[INIT] LCD inizializzato");

    Serial.println("\n[INIT] === TEST ULTRASUONI SINCRONO ===");
    Serial.printf("[INIT] Trig=GPIO%d, Echo=GPIO%d\n", trigPin, echoPin);
    digitalWrite(trigPin, LOW); delayMicroseconds(2);
    digitalWrite(trigPin, HIGH); delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long dur = pulseIn(echoPin, HIGH, 30000);
    if (dur == 0) {
        Serial.println("[INIT] ULTRASUONI: NESSUN ECHO!");
        Serial.println("[INIT] -> Verifica cavi, alimentazione 5V, partitore tensione su ECHO");
    } else {
        float dist = dur * VELOCITA_SUONO_CM_US / 2.0f;
        Serial.printf("[INIT] ULTRASUONI: OK! durata=%ldus distanza=%.1fcm\n", dur, dist);
    }
    Serial.println("[INIT] ======================================\n");

    connectWiFiBlocking();

    // FIX: Core 1 invece di Core 0, priorità 2 invece di 1
    // Core 0 è usato dal WiFi stack interno dell'ESP32 e causava TIMEOUT sull'echo
    xTaskCreatePinnedToCore(taskUltrasuoni, "UltrasuoniTask", 4096, NULL, 2, &taskUltrasuoniHandle, 1);
    usTaskAbilitato = true;
    Serial.println("[INIT] Task ultrasuoni avviato su Core 1 (priorità 2)");
    Serial.println("[INIT] Setup completato. Avvio loop.\n");
}

// ============================================================
// --- LOOP ---
// ============================================================
void loop() {
    unsigned long now = millis();
    loopCount++;

    tickWiFiSM();

    if (now - lastWifiCheck >= INTERVALLO_WIFI_CHECK) {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED && wifiSM.stato == WIFI_OK) {
            DBG_WIFI("Connessione persa!\n");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
            wifiSM.stato     = WIFI_RECONNECTING;
            wifiSM.tentativi = 0;
            wifiSM.tStato    = now;
        } else if (WiFi.status() == WL_CONNECTED) {
            DBG_WIFI("Check OK - IP:%s RSSI:%ddBm heap:%d\n",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
        }
    }

    if (now - lastSensorRead >= INTERVALLO_SENSORI) {
        lastSensorRead = now;
        lastSensorData = readNewSensors();
        int luceRaw = analogRead(lightPin);
        lastLightPct = constrain(
            (int)((float)(luceRaw - LUCE_MIN) / (LUCE_MAX - LUCE_MIN) * 100.0f),
            0, 100
        );
        DBG_SEN("Luce: raw=%d -> %d%%\n", luceRaw, lastLightPct);
    }

    if (now - lastDisplayUpdate >= INTERVALLO_LCD) {
        lastDisplayUpdate = now;
        updateLCD(lastSensorData.soilMoisture, lastWaterLevel,
                  lastSensorData.temperature, lastSensorData.humidity, lastLightPct);
        printDebugSummary();
    }

    if (now - lastSendData >= INTERVALLO_FIREBASE) {
        lastSendData = now;
        sendToFirebase(lastSensorData, lastWaterLevel, lastLightPct);
    }

    if (now - lastCommandCheck >= INTERVALLO_COMANDI) {
        lastCommandCheck = now;
        readFirebaseCommands();
    }

    DBG_LOOP("loop #%lu heap=%d\n", loopCount, ESP.getFreeHeap());
}

// ============================================================
// --- RIEPILOGO DEBUG ---
// ============================================================
void printDebugSummary() {
    Serial.println("\n========== RIEPILOGO ==========");
    Serial.printf("  Uptime: %lus  Loop: %lu  Heap: %d\n",
                  millis()/1000, loopCount, ESP.getFreeHeap());
    Serial.println("  SENSORI:");
    if (lastSensorData.valid) {
        Serial.printf("    T=%.1f°C  H=%.1f%%\n", lastSensorData.temperature, lastSensorData.humidity);
    } else {
        Serial.println("    T/H: ERRORE");
    }
    Serial.printf("    Suolo=%d%%  Luce=%d%%\n", lastSensorData.soilMoisture, lastLightPct);
    if (lastWaterLevel >= 0) {
        Serial.printf("    Acqua=%.1f%%\n", lastWaterLevel);
    } else {
        Serial.println("    Acqua: ERRORE (sensore non risponde)");
    }
    Serial.printf("  ULTRASUONI: avvii=%lu ok=%lu err=%lu (%.0f%%)\n",
                  usAvvii, usSuccessi, usErrori,
                  usAvvii > 0 ? (float)usSuccessi/usAvvii*100.0f : 0.0f);
    Serial.printf("  WIFI: %s  RSSI:%ddBm\n",
                  WiFi.status()==WL_CONNECTED ? "OK" : "DISCONNESSO", WiFi.RSSI());
    Serial.printf("  FIREBASE: invii=%lu err=%lu\n", fbInvii, fbErrori);
    Serial.printf("  ATTUATORI: pompa=%s umid=%s/%s ventola=%s luci=%s tetto=%s\n",
                  digitalRead(pompa2)?"ON":"OFF",
                  digitalRead(umidificatore1)?"ON":"OFF",
                  digitalRead(umidificatore2)?"ON":"OFF",
                  digitalRead(ventolaSerra)?"ON":"OFF",
                  digitalRead(luci)?"ON":"OFF",
                  roofOpen?"APERTO":"CHIUSO");
    Serial.println("================================\n");
}

// ============================================================
// --- SCAN I2C ---
// ============================================================
void scanI2C() {
    Serial.println("[I2C] Scansione dispositivi...");
    int trovati = 0;
    for (byte addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] Dispositivo trovato a 0x%02X%s%s\n", addr,
                          (addr==0x27||addr==0x3F) ? " <- LCD" : "",
                          (addr==0x44||addr==0x45) ? " <- SHT4x" : "");
            trovati++;
        }
    }
    Serial.printf("[I2C] Totale: %d dispositivo/i\n", trovati);
    if (trovati == 0) Serial.println("[I2C] NESSUNO! Verifica SDA=21 SCL=22");
}

// ============================================================
// --- WIFI BLOCKING ---
// ============================================================
void connectWiFiBlocking() {
    DBG_WIFI("Connessione a '%s'...\n", ssid);
    WiFi.begin(ssid, password);
    unsigned long t0 = millis(), tPoll = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        if (millis() - tPoll >= 500) { tPoll = millis(); Serial.print("."); }
    }
    if (WiFi.status() == WL_CONNECTED) {
        DBG_WIFI("\nConnesso! IP:%s RSSI:%ddBm\n",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
        wifiSM.stato = WIFI_OK;
    } else {
        DBG_WIFI("\nFallito, avvio fallback\n");
        wifiSM.tStato = millis();
        wifiSM.stato  = WIFI_FALLBACK_SCAN;
    }
}

// ============================================================
// --- WIFI SM ---
// ============================================================
void tickWiFiSM() {
    unsigned long now = millis();
    switch (wifiSM.stato) {
        case WIFI_OK: break;
        case WIFI_RECONNECTING:
            if (now - wifiSM.tStato >= 500) {
                wifiSM.tStato = now; wifiSM.tentativi++;
                if (WiFi.status() == WL_CONNECTED) { DBG_WIFI("Riconnesso!\n"); wifiSM.stato = WIFI_OK; }
                else if (wifiSM.tentativi >= 20)   { DBG_WIFI("Riconnessione fallita\n"); wifiSM.stato = WIFI_OK; }
            }
            break;
        case WIFI_FALLBACK_SCAN: {
            Serial.println("\n=== CONFIGURAZIONE WIFI MANUALE ===");
            int n = WiFi.scanNetworks();
            for (int i = 0; i < n; i++)
                Serial.printf("  %d) %s (%ddBm)\n", i+1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
            Serial.println("Inserisci SSID:");
            wifiSM.stato = WIFI_FALLBACK_WAIT_SSID; wifiSM.tStato = now;
            break;
        }
        case WIFI_FALLBACK_WAIT_SSID:
            if (Serial.available()) {
                wifiSM.nuovoSSID = Serial.readStringUntil('\n'); wifiSM.nuovoSSID.trim();
                if (wifiSM.nuovoSSID.length() > 0) {
                    Serial.println("Inserisci password:");
                    wifiSM.stato = WIFI_FALLBACK_WAIT_PASS; wifiSM.tStato = now;
                }
            }
            break;
        case WIFI_FALLBACK_WAIT_PASS:
            if (Serial.available()) {
                wifiSM.nuovaPassword = Serial.readStringUntil('\n'); wifiSM.nuovaPassword.trim();
                wifiSM.stato = WIFI_FALLBACK_CONNECTING; wifiSM.tStato = now; wifiSM.tentativi = 0;
                if (wifiSM.nuovaPassword.length() > 0) WiFi.begin(wifiSM.nuovoSSID.c_str(), wifiSM.nuovaPassword.c_str());
                else WiFi.begin(wifiSM.nuovoSSID.c_str());
            } else if (now - wifiSM.tStato >= 30000) {
                wifiSM.stato = WIFI_FALLBACK_CONNECTING; wifiSM.tStato = now; wifiSM.tentativi = 0;
                WiFi.begin(wifiSM.nuovoSSID.c_str());
            }
            break;
        case WIFI_FALLBACK_CONNECTING:
            if (now - wifiSM.tStato >= 500) {
                wifiSM.tStato = now; wifiSM.tentativi++;
                if (WiFi.status() == WL_CONNECTED) { DBG_WIFI("Connesso!\n"); wifiSM.stato = WIFI_OK; }
                else if (wifiSM.tentativi >= 30)   { DBG_WIFI("Offline\n"); wifiSM.stato = WIFI_OK; }
            }
            break;
    }
}

// ============================================================
// --- FIREBASE ---
// ============================================================
void sendToFirebase(const SensorData& s, float waterLevel, int lightPct) {
    if (WiFi.status() != WL_CONNECTED) { DBG_FB("Saltato: offline\n"); return; }
    JsonDocument doc;
    if (s.valid) {
        doc["temperature"] = serialized(String(s.temperature, 1));
        doc["humidity"]    = serialized(String(s.humidity, 1));
    } else {
        doc["sensorError"] = true;
    }
    doc["soil"] = s.soilMoisture;
    if (waterLevel >= 0) doc["remWater"] = serialized(String(waterLevel, 1));
    else                 doc["waterSensorError"] = true;
    doc["light"] = lightPct;
    String jsonData; serializeJson(doc, jsonData);
    DBG_FB("PUT -> %s\n", jsonData.c_str());
    HTTPClient http;
    http.begin(firebaseSensorsUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(3000);
    unsigned long t0 = millis();
    int code = http.PUT(jsonData);
    fbInvii++;
    if (code == 200) { DBG_FB("OK (HTTP%d %lums)\n", code, millis()-t0); }
    else             { fbErrori++; DBG_FB("ERRORE HTTP%d (%lums)\n", code, millis()-t0); }
    http.end();
}

// ============================================================
// --- LCD ---
// ============================================================
void updateLCD(float umiditaTerreno, float acqua, float temp, float umidita, int luce) {
    static int displayState = 0;
    lcd.clear();
    switch (displayState) {
        case 0:
            lcd.setCursor(0,0); lcd.print("Umid.T: "); lcd.print(umiditaTerreno,0); lcd.print("%");
            lcd.setCursor(0,1); lcd.print("H2O: ");
            if (acqua >= 0) { lcd.print(acqua,1); lcd.print("%"); } else { lcd.print("ERR"); }
            break;
        case 1:
            lcd.setCursor(0,0); lcd.print("Temp: "); lcd.print(temp,1); lcd.print((char)223); lcd.print("C");
            lcd.setCursor(0,1); lcd.print("Umid: "); lcd.print(umidita,1); lcd.print("%");
            break;
        case 2:
            lcd.setCursor(0,0); lcd.print("Luce: "); lcd.print(luce); lcd.print("%");
            lcd.setCursor(0,1); lcd.print("Tetto: "); lcd.print(roofOpen ? "APERTO" : "CHIUSO");
            break;
    }
    displayState = (displayState + 1) % 3;
}

// ============================================================
// --- COMANDI FIREBASE ---
// ============================================================
void readFirebaseCommands() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.begin(firebaseCmdUrl);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DBG_FB("GET: %s\n", payload.c_str());
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            if (doc.containsKey("lightOn"))      { bool on=doc["lightOn"]; digitalWrite(luci, on?HIGH:LOW); DBG_ATT("Luci %s\n",on?"ON":"OFF"); }
            if (doc.containsKey("roofOpen"))     { roofOpen=doc["roofOpen"].as<bool>(); servoA.write(roofOpen?180:0); servoB.write(roofOpen?0:180); DBG_ATT("Tetto %s\n",roofOpen?"APERTO":"CHIUSO"); }
            if (doc.containsKey("irrigationOn")) { doc["irrigationOn"] ? startIrrigation(lastWaterLevel) : stopIrrigation(); }
            if (doc.containsKey("humidifierOn")) { doc["humidifierOn"] ? startHumidifier() : stopHumidifier(); }
            if (doc.containsKey("fanOn"))        { doc["fanOn"] ? startFan() : stopFan(); }
        }
    } else { DBG_FB("GET fallito: HTTP%d\n", httpCode); }
    http.end();
}

// ============================================================
// --- ATTUATORI ---
// ============================================================
void startIrrigation(float waterLevel) {
    if (waterLevel >= 0 && waterLevel < SOGLIA_ACQUA_MINIMA) {
        DBG_ATT("Irrigazione BLOCCATA: %.1f%% < soglia\n", waterLevel);
        lcd.clear(); lcd.setCursor(0,0); lcd.print("ACQUA ESAURITA");
        lcd.setCursor(0,1); lcd.print("Irrigaz. STOP");
        return;
    }
    if (waterLevel < 0) { DBG_ATT("Irrigazione BLOCCATA: sensore errore\n"); return; }
    digitalWrite(pompa2, HIGH);
    DBG_ATT("Pompa ON (acqua: %.1f%%)\n", waterLevel);
}
void stopIrrigation()  { digitalWrite(pompa2, LOW);  DBG_ATT("Pompa OFF\n"); }
void startHumidifier() { digitalWrite(umidificatore1, HIGH); digitalWrite(umidificatore2, HIGH); DBG_ATT("Umidificatori ON\n"); }
void stopHumidifier()  { digitalWrite(umidificatore1, LOW);  digitalWrite(umidificatore2, LOW);  DBG_ATT("Umidificatori OFF\n"); }
void startFan()        { digitalWrite(ventolaSerra, HIGH); DBG_ATT("Ventola ON\n"); }
void stopFan()         { digitalWrite(ventolaSerra, LOW);  DBG_ATT("Ventola OFF\n"); }

// ============================================================
// --- LETTURA SENSORI ---
// ============================================================
SensorData readNewSensors() {
    SensorData data;
    data.valid = false;
    data.temperature = -99.0f;
    data.humidity    = -99.0f;
    data.soilMoisture = 0;

    float shtTemp = NAN, shtHum = NAN;
    if (sht45Ok) {
        sensors_event_t hev, tev;
        sht45.getEvent(&hev, &tev);
        shtTemp = tev.temperature;
        shtHum  = hev.relative_humidity;
        if (!isnan(shtTemp) && !isnan(shtHum)) {
            DBG_SEN("SHT45: T=%.2f°C H=%.2f%%\n", shtTemp, shtHum);
        } else {
            DBG_SEN("SHT45: NaN!\n");
        }
    } else {
        DBG_SEN("SHT45: non disponibile\n");
    }

    float tDHT = NAN, hDHT = NAN;
    if (millis() >= dhtReadyAt) {
        tDHT = dht.readTemperature();
        hDHT = dht.readHumidity();
        if (!isnan(tDHT) && !isnan(hDHT)) {
            DBG_SEN("DHT11: T=%.1f°C H=%.1f%%\n", tDHT, hDHT);
        } else {
            DBG_SEN("DHT11: NaN! (cavo?)\n");
        }
    } else {
        DBG_SEN("DHT11: warmup %lums\n", dhtReadyAt - millis());
    }

    bool dhtValido = !isnan(tDHT) && !isnan(hDHT);
    bool shtValido = sht45Ok && !isnan(shtTemp) && !isnan(shtHum);

    if (dhtValido && shtValido) {
        data.temperature = tDHT * PESO_DHT + shtTemp * PESO_SHT;
        data.humidity    = hDHT * PESO_DHT + shtHum  * PESO_SHT;
        data.valid = true;
        DBG_SEN("Fonte: media ponderata DHT+SHT -> T=%.1f H=%.1f\n", data.temperature, data.humidity);
    } else if (shtValido) {
        data.temperature = shtTemp;
        data.humidity    = shtHum;
        data.valid = true;
        DBG_SEN("Fonte: solo SHT45\n");
    } else if (dhtValido) {
        data.temperature = tDHT;
        data.humidity    = hDHT;
        data.valid = true;
        DBG_SEN("Fonte: solo DHT11\n");
    } else {
        DBG_SEN("ERRORE: nessun sensore valido!\n");
    }

    int v1  = analogRead(sensoreTerreno);
    int v2  = analogRead(sensoreTerreno2);
    int avg = (v1 + v2) / 2;
    int mapped = constrain(map(avg, 4095, 0, 0, 100), 0, 100);
    data.soilMoisture = mapped;
    DBG_SEN("Suolo: raw1=%d raw2=%d avg=%d -> %d%%\n", v1, v2, avg, mapped);

    return data;
}