// --- LIBRERIE ---
#include <Arduino.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <FirebaseESP32.h>
#include <ArduinoJson.h>
#include <esp32-hal-ledc.h>
#include "driver/ledc.h"

// --- SERVO ---
Servo servoA;
Servo servoB;
const int servoAPin = 4;
const int servoBPin = 16;

// --- VARIABILI GLOBALI PER SIMULAZIONE CLICK ---
bool clickInProgress = false;
unsigned long clickStartTime = 0;

// --- WIFI E FIREBASE ---
const char* ssid = "LAPTOP1234";
const char* password = "12345678";
const char* firebaseCmdUrl = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/commands.json";
const char* firebaseSensorsUrl = "https://serra-d44cc-default-rtdb.europe-west1.firebasedatabase.app/serra/sensors.json";

// --- DHT11 ---
#define DHTPIN 5
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- LCD I2C ---
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// --- SENSORI E PIN ---
const int sensoreTerreno = 35; // Umidità terreno (analogico)
const int trigPin = 14;
const int echoPin = 27;
const int lightPin = 34;
const int led = 2;
const int button = 15; // AGGIUNTO: Pin per il pulsante
const float TANK_HEIGHT = 30.0; // Altezza tanica in cm 
const int LIGHT_THRESHOLD = 2000;

// AGGIUNTO: Pin per attuatori
const int umidificatore1 = 32;
const int umidificatore2 = 33;
const int ventolaSerra = 25;
const int ventolaScatola = 26;
const int pompa1 = 12;
const int pompa2 = 13; 

unsigned long lastDisplayUpdate = 0;
unsigned long lastSendData = 0;
unsigned long lastCommandCheck = 0; // AGGIUNTO: Per evitare spam di comandi
bool roofOpen = false;
int stato = 0;

// --- SETUP ---
void setup() {
    Serial.begin(9600);
    
    // Configurazione pin
    pinMode(led, OUTPUT);
    pinMode(trigPin, OUTPUT);
    pinMode(echoPin, INPUT);
    pinMode(lightPin, INPUT);
    
    // AGGIUNTO: Configurazione pin attuatori
    pinMode(umidificatore1, OUTPUT);
    pinMode(umidificatore2, OUTPUT);
    pinMode(ventolaSerra, OUTPUT);
    pinMode(ventolaScatola, OUTPUT);
    pinMode(pompa1, OUTPUT);
    pinMode(pompa2, OUTPUT);
    
    // Inizializzazione servo
    servoA.attach(servoAPin);
    servoB.attach(servoBPin);
    
    Wire.begin(21, 22);
    dht.begin();
    lcd.init();
    lcd.backlight();

    connectWiFi(); // Connessione WiFi
}

// --- LOOP PRINCIPALE ---
void loop() {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    
    // AGGIUNTO: Controllo validità letture DHT
    if (isnan(t) || isnan(h)) {
        Serial.println("Errore lettura DHT11");
        t = 0.0;
        h = 0.0;
    }
    
    float waterLevel = readWaterLevel();
    int valoreTerreno = analogRead(sensoreTerreno);
    int percentualeTerreno = map(valoreTerreno, 4095, 0, 0, 100);
    percentualeTerreno = constrain(percentualeTerreno, 0, 100); // AGGIUNTO: Vincolo valori
    
    int luceRaw = analogRead(lightPin);
    int percentualeLuce = map(luceRaw, 0, 4095, 0, 100);
    percentualeLuce = constrain(percentualeLuce, 0, 100); // AGGIUNTO: Vincolo valori

    if (stato == 1) {
        moveRoof(!roofOpen);
        roofOpen = !roofOpen;
        stato = 0;
    }

    if (clickInProgress && millis() - clickStartTime >= 200) {
        digitalWrite(ventolaScatola, HIGH);  // Rilascia "pulsante"
        clickInProgress = false;
    }

    // Aggiorna LCD ogni 5 secondi
    if (millis() - lastDisplayUpdate >= 5000) {
        updateLCD(percentualeTerreno, waterLevel, t, h, percentualeLuce);
        lastDisplayUpdate = millis();
    }

    // Invio dati a Firebase ogni 5 secondi
    if (millis() - lastSendData >= 5000) {
        String lightLevelStr = percentualeLuce < 30 ? "low" : percentualeLuce < 70 ? "moderate" : "high";
        String jsonData = String("{") +
            "\"temperature\":" + String(t, 1) + "," + // MIGLIORATO: Limitato decimali
            "\"humidity\":" + String(h, 1) + "," +
            "\"soil\":" + String(percentualeTerreno) + "," +
            "\"remWater\":" + String(waterLevel, 1) + "," +
            "\"light\":\"" + lightLevelStr + "\"}";
        Serial.println("Dati inviati a Firebase: " + jsonData);
        sendToFirebase(jsonData);
        lastSendData = millis();
    }

    // MIGLIORATO: Controlla comandi da Firebase con debouncing
    if (millis() - lastCommandCheck >= 2000) {
        readFirebaseCommands();
        lastCommandCheck = millis();
    }
}

// --- CONNESSIONE WIFI ---
void connectWiFi() {
    Serial.print("Connessione al WiFi");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnesso al WiFi!");
    Serial.print("IP locale: ");
    Serial.println(WiFi.localIP());
}

// --- LETTURA LIVELLO ACQUA ---
float readWaterLevel() {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    
    // MIGLIORATO: Timeout ridotto
    long duration = pulseIn(echoPin, HIGH, 30000); // 30ms invece di 100ms
    if (duration <= 0) {
        Serial.println("Errore lettura sensore ultrasonico");
        return -1.0;
    }
    
    float distance = duration * 0.034 / 2;
    float livello = constrain(100 - (distance / TANK_HEIGHT * 100), 0, 100);
    return livello;
}

// --- INVIA DATI A FIREBASE ---
void sendToFirebase(const String& data) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(firebaseSensorsUrl);
        http.addHeader("Content-Type", "application/json");
        int httpResponseCode = http.PUT(data);
        if (httpResponseCode > 0) {
            Serial.print("Risposta Firebase: ");
            Serial.println(httpResponseCode);
        } else {
            Serial.print("Errore invio Firebase: ");
            Serial.println(httpResponseCode);
        }
        http.end();
    } else {
        Serial.println("WiFi non connesso. Impossibile inviare dati.");
    }
}

// --- LCD DISPLAY ---
void updateLCD(float umiditaTerreno, float acqua, float temp, float umidita, int luce) {
    static int displayState = 0;
    lcd.clear();
    switch (displayState) {
        case 0:
            lcd.setCursor(0, 0);
            lcd.print("Umidita' T: "); lcd.print(umiditaTerreno); lcd.print("%");
            lcd.setCursor(0, 1);
            lcd.print("H2O: "); 
            if (acqua >= 0) {
                lcd.print(acqua, 1); lcd.print("%");
            } else {
                lcd.print("ERR");
            }
            break;
        case 1:
            lcd.setCursor(0, 0);
            lcd.print("Temp: "); lcd.print(temp, 1); lcd.print((char)223); lcd.print("C");
            lcd.setCursor(0, 1);
            lcd.print("Umidita': "); lcd.print(umidita, 1); lcd.print(" %");
            break;
        case 2:
            lcd.setCursor(0, 0);
            lcd.print("Luce:");
            lcd.setCursor(0, 1);
            lcd.print(luce); lcd.print(" %");
            break;
    }
    displayState = (displayState + 1) % 3;
}

// --- MOVIMENTO TETTO CON SERVO ---
void moveRoof(bool open) {
  if (open) {
    servoA.write(90);
    servoB.write(0);
    Serial.println("Tetto aperto");
  } else {
    servoA.write(0);
    servoB.write(90);
    Serial.println("Tetto chiuso");
  }
}


// --- COMANDI DA FIREBASE ---
void readFirebaseCommands() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(firebaseCmdUrl);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            Serial.println("Comandi ricevuti da Firebase: " + payload);
            
            // MIGLIORATO: Aumentata dimensione buffer JSON
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, payload);
            
            if (error == DeserializationError::Ok) {
                // Controllo esistenza chiavi prima dell'uso
                if (doc.containsKey("lightOn")) {
                    digitalWrite(led, doc["lightOn"] ? HIGH : LOW);
                }
                if (doc.containsKey("roofOpen") && doc["roofOpen"]) {
                    stato = 1;
                }
                if (doc.containsKey("irrigationOn")) {
                    doc["irrigationOn"] ? startIrrigation() : stopIrrigation();
                }
                if (doc.containsKey("humidifierOn")) {
                    doc["humidifierOn"] ? startHumidifier() : stopHumidifier();
                }
                if (doc.containsKey("fanOn")) {
                    doc["fanOn"] ? startFan() : stopFan();
                }
            } else {
                Serial.print("Errore parsing JSON: ");
                Serial.println(error.c_str());
            }
        } else {
            Serial.print("Errore Firebase GET: ");
            Serial.println(httpCode);
        }
        http.end();
    }
}

// --- COMANDI USCITE ---
void startIrrigation() {
    digitalWrite(pompa1, HIGH);
    digitalWrite(pompa2, HIGH);
    Serial.println("Irrigazione avviata");
}

void stopIrrigation() {
    digitalWrite(pompa1, LOW);
    digitalWrite(pompa2, LOW);  
    Serial.println("Irrigazione fermata");
}

void startHumidifier() {
    digitalWrite(umidificatore1, HIGH);
    digitalWrite(umidificatore2, HIGH);
    Serial.println("Umidificatore avviato");

    // Avvia simulazione click
    digitalWrite(ventolaScatola, LOW);
    clickInProgress = true;
    clickStartTime = millis();
}

void stopHumidifier() {
    digitalWrite(umidificatore1, LOW);
    digitalWrite(umidificatore2, LOW);
    Serial.println("Umidificatore fermato");

    // Avvia simulazione click
    digitalWrite(ventolaScatola, LOW);
    clickInProgress = true;
    clickStartTime = millis();
}

void startFan() {
    digitalWrite(ventolaSerra, HIGH);
    Serial.println("Ventole avviate");
}

void stopFan() {
    digitalWrite(ventolaSerra, LOW);
    Serial.println("Ventole fermate");
}