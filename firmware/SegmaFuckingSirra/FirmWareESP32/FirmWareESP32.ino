// ============================================================
// SegmaFuckingSirra -67.0 V senza delay bloccanti
// ============================================================

// --- LIBRERIE ---
#include <Arduino.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Adafruit_SHT4x.h>

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
const int           CAMPIONI_ULTRASUONI   = 3;
const unsigned long PAUSA_TRA_PING_MS     = 35;
const unsigned long TIMEOUT_ECHO_MS       = 30;

// --- PESI MEDIA PONDERATA SENSORI T/H ---
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
// --- MACCHINA A STATI: ULTRASUONI NON BLOCCANTE ---
// ============================================================
enum UltrasuoniStato {
    US_IDLE,
    US_TRIGGER_LOW,
    US_TRIGGER_HIGH,
    US_WAIT_ECHO_START,
    US_WAIT_ECHO_END,
    US_PAUSA,
    US_DONE
};

struct UltrasuoniSM {
    UltrasuoniStato stato    = US_IDLE;
    int             campione = 0;
    float           somma    = 0.0f;
    int             validi   = 0;
    unsigned long   tStato   = 0;
    float           risultato= -1.0f;
    bool            pronto   = false;
} usSM;

void startUltrasuoni() {
    usSM.stato    = US_TRIGGER_LOW;
    usSM.campione = 0;
    usSM.somma    = 0.0f;
    usSM.validi   = 0;
    usSM.pronto   = false;
    usSM.tStato   = micros();
    digitalWrite(trigPin, LOW);
}

void tickUltrasuoniSM() {
    if (usSM.stato == US_IDLE || usSM.stato == US_DONE) return;

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
            }
            break;

        case US_WAIT_ECHO_START:
            if (digitalRead(echoPin) == HIGH) {
                usSM.tStato = micros();
                usSM.stato  = US_WAIT_ECHO_END;
            } else if (millis() - usSM.tStato >= TIMEOUT_ECHO_MS) {
                usSM.campione++;
                usSM.tStato = millis();
                usSM.stato  = (usSM.campione < CAMPIONI_ULTRASUONI) ? US_PAUSA : US_DONE;
                if (usSM.stato == US_DONE) {
                    usSM.risultato = (usSM.validi > 0) ? usSM.somma / usSM.validi : -1.0f;
                    usSM.pronto    = true;
                    if (usSM.validi == 0) Serial.println("Errore: nessuna lettura valida (ultrasuoni).");
                }
            }
            break;

        case US_WAIT_ECHO_END:
            if (digitalRead(echoPin) == LOW) {
                unsigned long duration = micros() - usSM.tStato;
                float distance = duration * VELOCITA_SUONO_CM_US / 2.0f;
                float livello  = constrain(100.0f - (distance / TANK_HEIGHT * 100.0f), 0.0f, 100.0f);
                usSM.somma += livello;
                usSM.validi++;
                usSM.campione++;
                usSM.tStato = millis();
                usSM.stato  = (usSM.campione < CAMPIONI_ULTRASUONI) ? US_PAUSA : US_DONE;
                if (usSM.stato == US_DONE) {
                    usSM.risultato = usSM.somma / usSM.validi;
                    usSM.pronto    = true;
                }
            } else if (micros() - usSM.tStato > TIMEOUT_ECHO_MS * 1000UL) {
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
// --- MACCHINA A STATI: WIFI NON BLOCCANTE ---
// ============================================================
enum WiFiStato {
    WIFI_OK,
    WIFI_RECONNECTING,
    WIFI_FALLBACK_SCAN,
    WIFI_FALLBACK_WAIT_SSID,
    WIFI_FALLBACK_WAIT_PASS,
    WIFI_FALLBACK_CONNECTING
};

struct WiFiSM {
    WiFiStato     stato         = WIFI_OK;
    int           tentativi     = 0;
    unsigned long tStato        = 0;
    String        nuovoSSID     = "";
    String        nuovaPassword = "";
} wifiSM;

// --- WIFI E FIREBASE ---
const char* ssid               = "LAPTOP1234";
const char* password           = "12345678";
const char* firebaseCmdUrl     = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/commands.json";
const char* firebaseSensorsUrl = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/sensors.json";

// --- SENSORE SHT45 ---
Adafruit_SHT4x sht45 = Adafruit_SHT4x();
bool sht45Ok = false;

// --- SERVO ---
Servo servoA;
Servo servoB;
const int servoAPin = 4;
const int servoBPin = 16;

// --- STATO TETTO (solo per LCD) ---
bool roofOpen = false;

// --- DHT11 ---
#define DHTPIN  5
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
unsigned long dhtReadyAt = 0;

// --- LCD I2C ---
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// --- TIMING ---
unsigned long lastSensorRead    = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSendData      = 0;
unsigned long lastCommandCheck  = 0;
unsigned long lastWifiCheck     = 0;

// --- CACHE ULTIMA LETTURA SENSORI ---
struct SensorData {
    float temperature;
    float humidity;
    int   soilMoisture;
    bool  valid;
};

SensorData lastSensorData = { -99.0f, -99.0f, 0, false };
float      lastWaterLevel = -1.0f;
int        lastLightPct   = 0;

// --- PROTOTIPI ---
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

// ============================================================
// --- SETUP ---
// ============================================================
void setup() {
    Serial.begin(9600);

    pinMode(trigPin,  OUTPUT);
    pinMode(echoPin,  INPUT);
    pinMode(lightPin, INPUT);
    digitalWrite(trigPin, LOW);

    pinMode(umidificatore1, OUTPUT);
    pinMode(umidificatore2, OUTPUT);
    pinMode(ventolaSerra,   OUTPUT);
    pinMode(ventolaScatola, OUTPUT);
    pinMode(pompa2,         OUTPUT);
    pinMode(luci,           OUTPUT);

    digitalWrite(umidificatore1, LOW);
    digitalWrite(umidificatore2, LOW);
    digitalWrite(ventolaSerra,   LOW);
    digitalWrite(ventolaScatola, LOW);
    digitalWrite(pompa2,         LOW);
    digitalWrite(luci,           LOW);

    servoA.attach(servoAPin);
    servoB.attach(servoBPin);
    // Boot: tetto chiuso
    servoA.write(0);
    servoB.write(180);
    roofOpen = false;

    Wire.begin(21, 22);

    dht.begin();
    dhtReadyAt = millis() + DHT_WARMUP_MS;

    if (sht45.begin()) {
        sht45Ok = true;
        sht45.setPrecision(SHT4X_HIGH_PRECISION);
        sht45.setHeater(SHT4X_NO_HEATER);
        Serial.println("SHT45 OK");
    } else {
        sht45Ok = false;
        Serial.println("ATTENZIONE: SHT45 non trovato, uso solo DHT11.");
    }

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Serra avviata...");

    connectWiFiBlocking();
}

// ============================================================
// --- LOOP PRINCIPALE ---
// ============================================================
void loop() {
    unsigned long now = millis();

    tickWiFiSM();

    if (now - lastWifiCheck >= INTERVALLO_WIFI_CHECK) {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED && wifiSM.stato == WIFI_OK) {
            Serial.println("WiFi perso, avvio riconnessione...");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
            wifiSM.stato     = WIFI_RECONNECTING;
            wifiSM.tentativi = 0;
            wifiSM.tStato    = now;
        }
    }

    if (now - lastSensorRead >= INTERVALLO_SENSORI) {
        lastSensorRead = now;
        lastSensorData = readNewSensors();
        int luceRaw    = analogRead(lightPin);
        lastLightPct   = constrain(map(luceRaw, 0, 4095, 0, 100), 0, 100);
        startUltrasuoni();
    }

    tickUltrasuoniSM();
    if (usSM.pronto) {
        lastWaterLevel = usSM.risultato;
        usSM.pronto    = false;
        usSM.stato     = US_IDLE;
    }

    if (now - lastDisplayUpdate >= INTERVALLO_LCD) {
        lastDisplayUpdate = now;
        updateLCD(lastSensorData.soilMoisture, lastWaterLevel,
                  lastSensorData.temperature, lastSensorData.humidity,
                  lastLightPct);
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
// --- WIFI: prima connessione al boot ---
// ============================================================
void connectWiFiBlocking() {
    Serial.print("Connessione a "); Serial.println(ssid);
    WiFi.begin(ssid, password);

    unsigned long t0 = millis(), tPoll = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        tickUltrasuoniSM();
        if (millis() - tPoll >= 100) { tPoll = millis(); Serial.print("."); }
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnesso! IP: "); Serial.println(WiFi.localIP());
        wifiSM.stato = WIFI_OK;
    } else {
        Serial.println("\nRete principale non disponibile, avvio fallback seriale...");
        wifiSM.nuovoSSID = ""; wifiSM.nuovaPassword = "";
        wifiSM.tStato = millis();
        wifiSM.stato  = WIFI_FALLBACK_SCAN;
    }
}

// ============================================================
// --- MACCHINA A STATI WIFI ---
// ============================================================
void tickWiFiSM() {
    unsigned long now = millis();
    switch (wifiSM.stato) {
        case WIFI_OK: break;

        case WIFI_RECONNECTING:
            if (now - wifiSM.tStato >= 500) {
                wifiSM.tStato = now; wifiSM.tentativi++; Serial.print(".");
                if (WiFi.status() == WL_CONNECTED) { Serial.println("\nRiconnesso!"); wifiSM.stato = WIFI_OK; }
                else if (wifiSM.tentativi >= 20)   { Serial.println("\nRiconnessione fallita. Riprovo tra 30 s."); wifiSM.stato = WIFI_OK; }
            }
            break;

        case WIFI_FALLBACK_SCAN: {
            Serial.println("\n==============================");
            Serial.println("  CONFIGURAZIONE WIFI MANUALE");
            Serial.println("==============================");
            int numReti = WiFi.scanNetworks();
            for (int i = 0; i < numReti; i++) {
                Serial.print("  "); Serial.print(i+1); Serial.print(") ");
                Serial.print(WiFi.SSID(i));
                Serial.print(" ("); Serial.print(WiFi.RSSI(i)); Serial.print(" dBm)");
                Serial.println(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " [Aperta]" : " [Protetta]");
            }
            Serial.println("\nInserisci SSID e premi Invio:");
            wifiSM.stato = WIFI_FALLBACK_WAIT_SSID; wifiSM.tStato = now;
            break;
        }

        case WIFI_FALLBACK_WAIT_SSID:
            if (Serial.available()) {
                wifiSM.nuovoSSID = Serial.readStringUntil('\n'); wifiSM.nuovoSSID.trim();
                if (wifiSM.nuovoSSID.length() > 0) {
                    Serial.println("Inserisci password (vuoto se aperta) e premi Invio:");
                    wifiSM.nuovaPassword = ""; wifiSM.stato = WIFI_FALLBACK_WAIT_PASS; wifiSM.tStato = now;
                }
            }
            break;

        case WIFI_FALLBACK_WAIT_PASS:
            if (Serial.available()) {
                wifiSM.nuovaPassword = Serial.readStringUntil('\n'); wifiSM.nuovaPassword.trim();
                wifiSM.stato = WIFI_FALLBACK_CONNECTING; wifiSM.tStato = now; wifiSM.tentativi = 0;
                if (wifiSM.nuovaPassword.length() > 0) WiFi.begin(wifiSM.nuovoSSID.c_str(), wifiSM.nuovaPassword.c_str());
                else                                   WiFi.begin(wifiSM.nuovoSSID.c_str());
            } else if (now - wifiSM.tStato >= 30000) {
                wifiSM.stato = WIFI_FALLBACK_CONNECTING; wifiSM.tStato = now; wifiSM.tentativi = 0;
                WiFi.begin(wifiSM.nuovoSSID.c_str());
            }
            break;

        case WIFI_FALLBACK_CONNECTING:
            if (now - wifiSM.tStato >= 500) {
                wifiSM.tStato = now; wifiSM.tentativi++; Serial.print(".");
                if (WiFi.status() == WL_CONNECTED) { Serial.println("\nConnesso!"); wifiSM.stato = WIFI_OK; }
                else if (wifiSM.tentativi >= 30)   { Serial.println("\nImpossibile connettersi. Continuo offline."); wifiSM.stato = WIFI_OK; }
            }
            break;
    }
}

// ============================================================
// --- INVIA DATI A FIREBASE ---
// ============================================================
void sendToFirebase(const SensorData& s, float waterLevel, int lightPct) {
    if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi non connesso. Invio saltato."); return; }

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
    doc["light"] = lightPct < 30 ? "low" : lightPct < 70 ? "moderate" : "high";

    String jsonData;
    serializeJson(doc, jsonData);
    Serial.println("Firebase OUT: " + jsonData);

    HTTPClient http;
    http.begin(firebaseSensorsUrl);
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(jsonData);
    Serial.print("Risposta Firebase: "); Serial.println(code);
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
            lcd.setCursor(0, 0); lcd.print("Umid.T: "); lcd.print(umiditaTerreno, 0); lcd.print("%");
            lcd.setCursor(0, 1); lcd.print("H2O: ");
            if (acqua >= 0) { lcd.print(acqua, 1); lcd.print("%"); } else { lcd.print("ERR"); }
            break;
        case 1:
            lcd.setCursor(0, 0); lcd.print("Temp: "); lcd.print(temp, 1); lcd.print((char)223); lcd.print("C");
            lcd.setCursor(0, 1); lcd.print("Umid: "); lcd.print(umidita, 1); lcd.print("%");
            break;
        case 2:
            lcd.setCursor(0, 0); lcd.print("Luce: "); lcd.print(luce); lcd.print("%");
            lcd.setCursor(0, 1); lcd.print("Tetto: "); lcd.print(roofOpen ? "APERTO" : "CHIUSO");
            break;
    }
    displayState = (displayState + 1) % 3;
}

// ============================================================
// --- COMANDI DA FIREBASE ---
// ============================================================
void readFirebaseCommands() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(firebaseCmdUrl);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            if (doc.containsKey("lightOn"))
                digitalWrite(luci, doc["lightOn"] ? HIGH : LOW);

            if (doc.containsKey("roofOpen")) {
                roofOpen = doc["roofOpen"].as<bool>();
                if (roofOpen) {
                    servoA.write(180);
                    servoB.write(0);
                    Serial.println("Tetto APERTO");
                } else {
                    servoA.write(0);
                    servoB.write(180);
                    Serial.println("Tetto CHIUSO");
                }
            }

            if (doc.containsKey("irrigationOn"))
                doc["irrigationOn"] ? startIrrigation(lastWaterLevel) : stopIrrigation();
            if (doc.containsKey("humidifierOn"))
                doc["humidifierOn"] ? startHumidifier() : stopHumidifier();
            if (doc.containsKey("fanOn"))
                doc["fanOn"] ? startFan() : stopFan();
        } else {
            Serial.print("Errore parsing JSON comandi: "); Serial.println(error.c_str());
        }
    } else {
        Serial.print("Errore Firebase GET: "); Serial.println(httpCode);
    }
    http.end();
}

// ============================================================
// --- ATTUATORI ---
// ============================================================
void startIrrigation(float waterLevel) {
    if (waterLevel >= 0 && waterLevel < SOGLIA_ACQUA_MINIMA) {
        Serial.println("ATTENZIONE: acqua bassa (" + String(waterLevel, 1) + "%). Irrigazione bloccata.");
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("ACQUA ESAURITA");
        lcd.setCursor(0, 1); lcd.print("Irrigaz. STOP");
        return;
    }
    if (waterLevel < 0) { Serial.println("Sensore acqua in errore. Irrigazione bloccata."); return; }
    digitalWrite(pompa2, HIGH);
    Serial.println("Irrigazione avviata (acqua: " + String(waterLevel, 1) + "%)");
}

void stopIrrigation()  { digitalWrite(pompa2, LOW);                                              Serial.println("Irrigazione fermata"); }
void startHumidifier() { digitalWrite(umidificatore1, HIGH); digitalWrite(umidificatore2, HIGH); Serial.println("Umidificatori avviati"); }
void stopHumidifier()  { digitalWrite(umidificatore1, LOW);  digitalWrite(umidificatore2, LOW);  Serial.println("Umidificatori fermati"); }
void startFan()        { digitalWrite(ventolaSerra, HIGH);                                       Serial.println("Ventola serra avviata"); }
void stopFan()         { digitalWrite(ventolaSerra, LOW);                                        Serial.println("Ventola serra fermata"); }

// ============================================================
// --- LETTURA SENSORI ---
// ============================================================
SensorData readNewSensors() {
    SensorData data;
    data.valid = false;

    float shtTemp = NAN, shtHum = NAN;
    if (sht45Ok) {
        sensors_event_t hev, tev;
        sht45.getEvent(&hev, &tev);
        shtTemp = tev.temperature;
        shtHum  = hev.relative_humidity;
        Serial.print("SHT45 -> T:"); Serial.print(shtTemp);
        Serial.print(" H:"); Serial.println(shtHum);
    }

    float tDHT = NAN, hDHT = NAN;
    if (millis() >= dhtReadyAt) {
        tDHT = dht.readTemperature();
        hDHT = dht.readHumidity();
        Serial.print("DHT11 -> T:"); Serial.print(tDHT);
        Serial.print(" H:"); Serial.println(hDHT);
    } else {
        Serial.println("DHT11: warmup in corso...");
    }

    bool dhtValido = !isnan(tDHT) && !isnan(hDHT);
    bool shtValido = sht45Ok && !isnan(shtTemp) && !isnan(shtHum);

    if (dhtValido && shtValido) {
        data.temperature = tDHT * PESO_DHT + shtTemp * PESO_SHT;
        data.humidity    = hDHT * PESO_DHT + shtHum  * PESO_SHT;
        data.valid = true;
        Serial.println("Fonte: media ponderata DHT+SHT");
    } else if (shtValido) {
        data.temperature = shtTemp; data.humidity = shtHum; data.valid = true;
        Serial.println("Fonte: solo SHT45");
    } else if (dhtValido) {
        data.temperature = tDHT; data.humidity = hDHT; data.valid = true;
        Serial.println("Fonte: solo DHT11");
    } else {
        data.temperature = -99.0f; data.humidity = -99.0f; data.valid = false;
        Serial.println("ERRORE: nessun sensore T/H disponibile!");
    }

    int valSoil1 = analogRead(sensoreTerreno);
    int valSoil2 = analogRead(sensoreTerreno2);
    int avgSoil  = (valSoil1 + valSoil2) / 2;
    avgSoil = map(avgSoil, 4095, 0, 0, 100);
    data.soilMoisture = constrain(avgSoil, 0, 100);

    return data;
}