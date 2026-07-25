// ============================================================
// SegmaFuckingSirra -67.0 V senza delay bloccanti
// DEBUG VERSION
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
// --- DEBUG CONFIG (metti false per silenziare) ---
// ============================================================
#define DEBUG_ULTRASUONI   true
#define DEBUG_SENSORI      true
#define DEBUG_FIREBASE     true
#define DEBUG_WIFI         true
#define DEBUG_ATTUATORI    true

#define DBG_US(...)   do { if (DEBUG_ULTRASUONI) { Serial.print("[US]  "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_SEN(...)  do { if (DEBUG_SENSORI)    { Serial.print("[SEN] "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_FB(...)   do { if (DEBUG_FIREBASE)   { Serial.print("[FB]  "); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_WIFI(...) do { if (DEBUG_WIFI)       { Serial.print("[WIFI]"); Serial.printf(__VA_ARGS__); } } while(0)
#define DBG_ATT(...)  do { if (DEBUG_ATTUATORI)  { Serial.print("[ATT] "); Serial.printf(__VA_ARGS__); } } while(0)

// --- COSTANTI FISICHE ---
const float VELOCITA_SUONO_CM_US = 0.034f;
const float TANK_HEIGHT          = 30.0f;
const float SOGLIA_ACQUA_MINIMA  = 10.0f;

// --- COSTANTI TIMING (ms) ---
const unsigned long INTERVALLO_SENSORI    = 500;
const unsigned long INTERVALLO_LCD        = 3000;
const unsigned long INTERVALLO_FIREBASE   = 1000;
const unsigned long INTERVALLO_COMANDI    = 500;
const unsigned long INTERVALLO_WIFI_CHECK = 30000;
const unsigned long DHT_WARMUP_MS         = 2000;

// --- COSTANTI ULTRASUONI ---
const int CAMPIONI_ULTRASUONI = 3;

// --- PESI MEDIA PONDERATA ---
const float PESO_DHT = 0.3f;
const float PESO_SHT = 0.7f;

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

unsigned long usAvvii    = 0;
unsigned long usSuccessi = 0;
unsigned long usErrori   = 0;
unsigned long fbInvii    = 0;
unsigned long fbErrori   = 0;

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
// --- TASK FREERTOS: ULTRASUONI SU CORE 0 (pulseIn bloccante) ---
// ============================================================
TaskHandle_t  taskUltrasuoniHandle = NULL;
volatile bool usTaskAbilitato      = false;

void taskUltrasuoni(void* param) {
    for (;;) {
        if (usTaskAbilitato) {
            usAvvii++;
            float somma = 0.0f;
            int   validi = 0;

            for (int i = 0; i < CAMPIONI_ULTRASUONI; i++) {
                digitalWrite(trigPin, LOW);  delayMicroseconds(2);
                digitalWrite(trigPin, HIGH); delayMicroseconds(10);
                digitalWrite(trigPin, LOW);

                long dur = pulseIn(echoPin, HIGH, 30000); // timeout 30ms
                if (dur > 0) {
                    float dist   = dur * VELOCITA_SUONO_CM_US / 2.0f;
                    float livello = constrain(100.0f - (dist / TANK_HEIGHT * 100.0f), 0.0f, 100.0f);
                    somma += livello;
                    validi++;
                    DBG_US("C%d: %ldus -> %.1fcm -> %.1f%%\n", i+1, dur, dist, livello);
                } else {
                    DBG_US("C%d: TIMEOUT\n", i+1);
                }
                delay(35);
            }

            if (validi > 0) {
                lastWaterLevel = somma / validi;
                usSuccessi++;
                DBG_US("OK: %.1f%% (%d/%d validi)\n", lastWaterLevel, validi, CAMPIONI_ULTRASUONI);
            } else {
                lastWaterLevel = -1.0f;
                usErrori++;
                DBG_US("ERRORE: nessun campione valido\n");
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

    Serial.println("\n============================================");
    Serial.println("  SegmaFuckingSirra - DEBUG MODE");
    Serial.printf ("  Chip: %s  CPU: %dMHz  Heap: %d\n",
                   ESP.getChipModel(), ESP.getCpuFreqMHz(), ESP.getFreeHeap());
    Serial.printf ("  TRIG=GPIO%d  ECHO=GPIO%d  DHT=GPIO%d\n", trigPin, echoPin, DHTPIN);
    Serial.println("============================================\n");

    pinMode(trigPin,  OUTPUT); digitalWrite(trigPin, LOW);
    pinMode(echoPin,  INPUT);
    pinMode(lightPin, INPUT);

    pinMode(umidificatore1, OUTPUT); digitalWrite(umidificatore1, LOW);
    pinMode(umidificatore2, OUTPUT); digitalWrite(umidificatore2, LOW);
    pinMode(ventolaSerra,   OUTPUT); digitalWrite(ventolaSerra,   LOW);
    pinMode(ventolaScatola, OUTPUT); digitalWrite(ventolaScatola, LOW);
    pinMode(pompa2,         OUTPUT); digitalWrite(pompa2,         LOW);
    pinMode(luci,           OUTPUT); digitalWrite(luci,           LOW);

    servoA.attach(servoAPin); servoA.write(0);
    servoB.attach(servoBPin); servoB.write(180);
    roofOpen = false;

    Wire.begin(21, 22);
    scanI2C();

    dht.begin();
    dhtReadyAt = millis() + DHT_WARMUP_MS;

    if (sht45.begin()) {
        sht45Ok = true;
        sht45.setPrecision(SHT4X_HIGH_PRECISION);
        sht45.setHeater(SHT4X_NO_HEATER);
        Serial.println("[INIT] SHT45: OK");
    } else {
        sht45Ok = false;
        Serial.println("[INIT] SHT45: NON TROVATO");
    }

    lcd.init(); lcd.backlight();
    lcd.setCursor(0,0); lcd.print("Serra avviata...");

    // Test ultrasuoni sincrono al boot
    digitalWrite(trigPin, LOW);  delayMicroseconds(2);
    digitalWrite(trigPin, HIGH); delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long dur = pulseIn(echoPin, HIGH, 30000);
    if (dur == 0) {
        Serial.println("[INIT] ULTRASUONI: NESSUN ECHO - verifica cavi e 5V");
    } else {
        Serial.printf("[INIT] ULTRASUONI: OK -> %ldus -> %.1fcm\n",
                      dur, dur * VELOCITA_SUONO_CM_US / 2.0f);
    }

    connectWiFiBlocking();

    xTaskCreatePinnedToCore(taskUltrasuoni, "US_Task", 4096, NULL, 1, &taskUltrasuoniHandle, 0);
    usTaskAbilitato = true;
    Serial.println("[INIT] Task ultrasuoni su Core 0. Setup OK.\n");
}

// ============================================================
// --- LOOP ---
// ============================================================
void loop() {
    unsigned long now = millis();

    tickWiFiSM();

    if (now - lastWifiCheck >= INTERVALLO_WIFI_CHECK) {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED && wifiSM.stato == WIFI_OK) {
            DBG_WIFI(" Connessione persa, riconnetto...\n");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
            wifiSM.stato = WIFI_RECONNECTING;
            wifiSM.tentativi = 0;
            wifiSM.tStato = now;
        }
    }

    if (now - lastSensorRead >= INTERVALLO_SENSORI) {
        lastSensorRead = now;
        lastSensorData = readNewSensors();
        int luceRaw = analogRead(lightPin);
        lastLightPct = constrain(map(luceRaw, 0, 4095, 0, 100), 0, 100);
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
}

// ============================================================
// --- RIEPILOGO DEBUG ---
// ============================================================
void printDebugSummary() {
    Serial.println("\n===== RIEPILOGO =====");
    if (lastSensorData.valid)
        Serial.printf("  T=%.1f°C  H=%.1f%%  Suolo=%d%%  Luce=%d%%\n",
                      lastSensorData.temperature, lastSensorData.humidity,
                      lastSensorData.soilMoisture, lastLightPct);
    else
        Serial.println("  T/H: ERRORE sensori");

    if (lastWaterLevel >= 0)
        Serial.printf("  Acqua=%.1f%%\n", lastWaterLevel);
    else
        Serial.println("  Acqua: ERRORE");

    Serial.printf("  US: ok=%lu err=%lu  FB: inv=%lu err=%lu\n",
                  usSuccessi, usErrori, fbInvii, fbErrori);
    Serial.printf("  WiFi:%s  RSSI:%ddBm  Heap:%d\n",
                  WiFi.status()==WL_CONNECTED?"OK":"KO", WiFi.RSSI(), ESP.getFreeHeap());
    Serial.printf("  Pompa:%s Umid:%s/%s Ventola:%s Luci:%s Tetto:%s\n",
                  digitalRead(pompa2)?"ON":"OFF",
                  digitalRead(umidificatore1)?"ON":"OFF",
                  digitalRead(umidificatore2)?"ON":"OFF",
                  digitalRead(ventolaSerra)?"ON":"OFF",
                  digitalRead(luci)?"ON":"OFF",
                  roofOpen?"APERTO":"CHIUSO");
    Serial.println("=====================\n");
}

// ============================================================
// --- SCAN I2C ---
// ============================================================
void scanI2C() {
    int trovati = 0;
    for (byte addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] 0x%02X%s%s\n", addr,
                          (addr==0x27||addr==0x3F) ? " <- LCD"  : "",
                          (addr==0x44||addr==0x45) ? " <- SHT4x": "");
            trovati++;
        }
    }
    if (trovati == 0) Serial.println("[I2C] NESSUN dispositivo trovato!");
}

// ============================================================
// --- WIFI ---
// ============================================================
void connectWiFiBlocking() {
    DBG_WIFI(" Connessione a '%s'...", ssid);
    WiFi.begin(ssid, password);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connesso! IP:%s RSSI:%ddBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        wifiSM.stato = WIFI_OK;
    } else {
        Serial.println("\n[WIFI] Fallito, avvio fallback");
        wifiSM.tStato = millis();
        wifiSM.stato  = WIFI_FALLBACK_SCAN;
    }
}

void tickWiFiSM() {
    unsigned long now = millis();
    switch (wifiSM.stato) {
        case WIFI_OK: break;
        case WIFI_RECONNECTING:
            if (now - wifiSM.tStato >= 500) {
                wifiSM.tStato = now; wifiSM.tentativi++;
                if (WiFi.status() == WL_CONNECTED) { DBG_WIFI(" Riconnesso!\n"); wifiSM.stato = WIFI_OK; }
                else if (wifiSM.tentativi >= 20)   { DBG_WIFI(" Riconnessione fallita\n"); wifiSM.stato = WIFI_OK; }
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
                if (wifiSM.nuovoSSID.length() > 0) { Serial.println("Inserisci password:"); wifiSM.stato = WIFI_FALLBACK_WAIT_PASS; wifiSM.tStato = now; }
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
                if (WiFi.status() == WL_CONNECTED) { DBG_WIFI(" Connesso!\n"); wifiSM.stato = WIFI_OK; }
                else if (wifiSM.tentativi >= 30)   { DBG_WIFI(" Offline\n"); wifiSM.stato = WIFI_OK; }
            }
            break;
    }
}

// ============================================================
// --- FIREBASE ---
// ============================================================
void sendToFirebase(const SensorData& s, float waterLevel, int lightPct) {
    if (WiFi.status() != WL_CONNECTED) { DBG_FB(" Saltato: offline\n"); return; }
    JsonDocument doc;
    if (s.valid) {
        doc["temperature"] = serialized(String(s.temperature, 1));
        doc["humidity"]    = serialized(String(s.humidity, 1));
    } else { doc["sensorError"] = true; }
    doc["soil"] = s.soilMoisture;
    if (waterLevel >= 0) doc["remWater"] = serialized(String(waterLevel, 1));
    else                 doc["waterSensorError"] = true;
    doc["light"] = lightPct < 30 ? "low" : lightPct < 70 ? "moderate" : "high";
    String jsonData; serializeJson(doc, jsonData);
    DBG_FB(" PUT -> %s\n", jsonData.c_str());
    HTTPClient http;
    http.begin(firebaseSensorsUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(3000);
    unsigned long t0 = millis();
    int code = http.PUT(jsonData);
    fbInvii++;
    if (code == 200) { DBG_FB(" OK (%lums)\n", millis()-t0); }
    else             { fbErrori++; DBG_FB(" ERRORE HTTP%d (%lums)\n", code, millis()-t0); }
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
            if (acqua >= 0) { lcd.print(acqua,1); lcd.print("%"); } else lcd.print("ERR");
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
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            if (doc.containsKey("lightOn"))      { bool on=doc["lightOn"]; digitalWrite(luci, on?HIGH:LOW); DBG_ATT(" Luci %s\n", on?"ON":"OFF"); }
            if (doc.containsKey("roofOpen"))     { roofOpen=doc["roofOpen"].as<bool>(); servoA.write(roofOpen?180:0); servoB.write(roofOpen?0:180); DBG_ATT(" Tetto %s\n", roofOpen?"APERTO":"CHIUSO"); }
            if (doc.containsKey("irrigationOn")) { doc["irrigationOn"] ? startIrrigation(lastWaterLevel) : stopIrrigation(); }
            if (doc.containsKey("humidifierOn")) { doc["humidifierOn"] ? startHumidifier() : stopHumidifier(); }
            if (doc.containsKey("fanOn"))        { doc["fanOn"] ? startFan() : stopFan(); }
        }
    } else { DBG_FB(" GET fallito: HTTP%d\n", httpCode); }
    http.end();
}

// ============================================================
// --- ATTUATORI ---
// ============================================================
void startIrrigation(float waterLevel) {
    if (waterLevel >= 0 && waterLevel < SOGLIA_ACQUA_MINIMA) {
        DBG_ATT(" Irrigazione BLOCCATA: %.1f%% < soglia\n", waterLevel);
        lcd.clear(); lcd.setCursor(0,0); lcd.print("ACQUA ESAURITA");
        lcd.setCursor(0,1); lcd.print("Irrigaz. STOP"); return;
    }
    if (waterLevel < 0) { DBG_ATT(" Irrigazione BLOCCATA: sensore errore\n"); return; }
    digitalWrite(pompa2, HIGH); DBG_ATT(" Pompa ON (acqua:%.1f%%)\n", waterLevel);
}
void stopIrrigation()  { digitalWrite(pompa2, LOW);  DBG_ATT(" Pompa OFF\n"); }
void startHumidifier() { digitalWrite(umidificatore1, HIGH); digitalWrite(umidificatore2, HIGH); DBG_ATT(" Umidificatori ON\n"); }
void stopHumidifier()  { digitalWrite(umidificatore1, LOW);  digitalWrite(umidificatore2, LOW);  DBG_ATT(" Umidificatori OFF\n"); }
void startFan()        { digitalWrite(ventolaSerra, HIGH); DBG_ATT(" Ventola ON\n"); }
void stopFan()         { digitalWrite(ventolaSerra, LOW);  DBG_ATT(" Ventola OFF\n"); }

// ============================================================
// --- LETTURA SENSORI ---
// ============================================================
SensorData readNewSensors() {
    SensorData data = { -99.0f, -99.0f, 0, false };

    float shtTemp = NAN, shtHum = NAN;
    if (sht45Ok) {
        sensors_event_t hev, tev;
        sht45.getEvent(&hev, &tev);
        shtTemp = tev.temperature; shtHum = hev.relative_humidity;
        if (!isnan(shtTemp) && !isnan(shtHum)) DBG_SEN(" SHT45: T=%.1f H=%.1f\n", shtTemp, shtHum);
        else                                   DBG_SEN(" SHT45: NaN!\n");
    }

    float tDHT = NAN, hDHT = NAN;
    if (millis() >= dhtReadyAt) {
        tDHT = dht.readTemperature(); hDHT = dht.readHumidity();
        if (!isnan(tDHT) && !isnan(hDHT)) DBG_SEN(" DHT11: T=%.1f H=%.1f\n", tDHT, hDHT);
        else                              DBG_SEN(" DHT11: NaN!\n");
    }

    bool dhtValido = !isnan(tDHT) && !isnan(hDHT);
    bool shtValido = sht45Ok && !isnan(shtTemp) && !isnan(shtHum);

    if (dhtValido && shtValido) {
        data.temperature = tDHT*PESO_DHT + shtTemp*PESO_SHT;
        data.humidity    = hDHT*PESO_DHT + shtHum*PESO_SHT;
        data.valid = true;
        DBG_SEN(" Fonte: media -> T=%.1f H=%.1f\n", data.temperature, data.humidity);
    } else if (shtValido) {
        data.temperature = shtTemp; data.humidity = shtHum; data.valid = true;
        DBG_SEN(" Fonte: solo SHT45\n");
    } else if (dhtValido) {
        data.temperature = tDHT; data.humidity = hDHT; data.valid = true;
        DBG_SEN(" Fonte: solo DHT11\n");
    } else {
        DBG_SEN(" ERRORE: nessun sensore valido!\n");
    }

    int v1 = analogRead(sensoreTerreno), v2 = analogRead(sensoreTerreno2);
    data.soilMoisture = constrain(map((v1+v2)/2, 4095, 0, 0, 100), 0, 100);
    DBG_SEN(" Suolo: %d%%  Luce: %d%%\n", data.soilMoisture, lastLightPct);

    return data;
}
