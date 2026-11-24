/*
  =====================================================================
  SISTEM KENDALI SUHU INKUBATOR TELUR - ESP32 (sesuai wiring REV. 5)
  =====================================================================
  Komponen:
- DHT22            -> GPIO 4           (loop kendali utama)
- MAX6675 (K-Type)  -> SCK 18, CS 5, SO 19   (failsafe over-temperature)
- LCD I2C 16x2      -> SDA 21, SCL 22
- Relay Heater      -> GPIO 26
- Buzzer/LED        -> GPIO 27
- L298N (fan)       -> ENA 32 (PWM), IN1 33, IN2 14

  Library yang perlu diinstal lewat Library Manager:
- DHT sensor library (Adafruit)
- Adafruit Unified Sensor
- MAX6675 library (by Adafruit)
- LiquidCrystal I2C (by Frank de Brabander / Marco Schwartz)
- Firebase ESP32 Client (by Mobizt)  -> search "Firebase ESP32" di Library Manager

  Firebase: pakai Database Secret (legacy token) sesuai yang Anda berikan.
  =====================================================================
*/

#include <WiFi.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <max6675.h>
#include <FirebaseESP32.h>

// ------------------- KONFIGURASI WIFI -------------------
#define WIFI_SSID     "VINKEJU"
#define WIFI_PASSWORD "kejuenak28"

// ------------------- KONFIGURASI FIREBASE -------------------
#define FIREBASE_HOST "project-incubator-e66a2-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "oLOMh7rY9SjMC03w7gmjrcBf7dHxUPs5faTkVd40"

FirebaseData   firebaseData;
FirebaseConfig firebaseConfig;
FirebaseAuth   firebaseAuth;

// ------------------- PIN -------------------
#define PIN_DHT       4
#define PIN_MAX_SCK   18
#define PIN_MAX_CS    5
#define PIN_MAX_SO    19
#define PIN_RELAY     26
#define PIN_BUZZER    27
#define PIN_ENA       32   // PWM kecepatan kipas
#define PIN_IN1       33
#define PIN_IN2       14
#define PIN_SDA_LCD   21   // I2C LCD - SDA
#define PIN_SCL_LCD   22   // I2C LCD - SCL

// ------------------- OBJEK SENSOR / LCD -------------------
DHT dht(PIN_DHT, DHT22);
MAX6675 thermocouple(PIN_MAX_SCK, PIN_MAX_CS, PIN_MAX_SO);
LiquidCrystal_I2C lcd(0x27, 16, 2);   // ganti ke 0x3F jika layar blank

// ------------------- PARAMETER KENDALI -------------------
const float SUHU_AMAN_MAX = 42.0;   // ambang failsafe TETAP (tidak bisa diubah dari web)
const float HISTERESIS    = 0.5;    // agar heater tidak kedip-kedip

float setpointSuhu   = 37.5;  // default, akan ditimpa nilai dari Firebase
int   speedKipasWeb  = 60;    // default persen (0-100), akan ditimpa nilai dari Firebase
const int SPEED_MIN_PERCENT = 30;   // kipas tidak pernah benar-benar berhenti

bool statusHeater = false;
bool statusAlarm  = false;
float gSuhuDHT = 0, gHumDHT = 0, gSuhuMax = 0;   // nilai sensor terakhir, dikirim ke Firebase

int heartbeatCounter = 0;   // penanda ESP32 aktif untuk deteksi online/offline di web

unsigned long lastSensorRead   = 0;
unsigned long lastFirebaseSync = 0;
const unsigned long INTERVAL_SENSOR   = 2000;   // baca sensor tiap 2 detik
const unsigned long INTERVAL_FIREBASE = 3000;   // sinkron Firebase tiap 3 detik

// PWM (ESP32 LEDC) - API baru core ESP32 Arduino v3.x (tanpa channel manual)
const int PWM_FREQ = 1000;
const int PWM_RES  = 8;    // 0-255

void tampilkanLCD(float suhu, float hum, bool alarm);  // prototipe

void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  // arah putar kipas tetap satu arah
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, LOW);

  ledcAttach(PIN_ENA, PWM_FREQ, PWM_RES);

  dht.begin();

  Wire.begin(PIN_SDA_LCD, PIN_SCL_LCD);   // eksplisit set pin I2C LCD: SDA=21, SCL=22
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Inkubator ESP32");
  lcd.setCursor(0, 1);
  lcd.print("Menghubungkan...");

  // ---- WiFi ----
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Terhubung, IP: ");
  Serial.println(WiFi.localIP());

  // ---- Firebase (legacy database secret / token) ----
  firebaseConfig.database_url = FIREBASE_HOST;
  firebaseConfig.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&firebaseConfig, &firebaseAuth);
  Firebase.reconnectWiFi(true);

  // Kirim sinyal awal agar web langsung mendeteksi ESP32 terhubung
  if (Firebase.ready()) {
    heartbeatCounter = 1;
    Firebase.setInt(firebaseData, "/inkubator/heartbeat", heartbeatCounter);
  }

  lcd.clear();
}

void loop() {
  unsigned long now = millis();

  // =========================================================
  // 1) BACA SENSOR + LOGIKA KENDALI (jalan lokal, tidak menunggu Firebase)
  // =========================================================
  if (now - lastSensorRead >= INTERVAL_SENSOR) {
    lastSensorRead = now;

    float suhuDHT = dht.readTemperature();
    float humDHT  = dht.readHumidity();
    float suhuMax = thermocouple.readCelsius();

    // ---- CEK FAILSAFE DULU (paling atas, prioritas tertinggi) ----
    if (!isnan(suhuMax) && suhuMax > SUHU_AMAN_MAX) {
      // Over-temperature terdeteksi oleh thermocouple -> heater WAJIB mati
      digitalWrite(PIN_RELAY, LOW);
      statusHeater = false;
      statusAlarm  = true;
      ledcWrite(PIN_ENA, 255);   // kipas kecepatan maksimum
      digitalWrite(PIN_BUZZER, HIGH);

      tampilkanLCD(suhuDHT, humDHT, true);
      Serial.println("!!! ALARM: Suhu thermocouple melebihi batas aman !!!");
    }
    else {
      // Kondisi normal -> kendali utama berbasis DHT22 + setpoint
      statusAlarm = false;
      digitalWrite(PIN_BUZZER, LOW);

      if (!isnan(suhuDHT)) {
        if (suhuDHT < (setpointSuhu - HISTERESIS)) {
          digitalWrite(PIN_RELAY, HIGH);
          statusHeater = true;
        } else if (suhuDHT >= (setpointSuhu + HISTERESIS)) {
          digitalWrite(PIN_RELAY, LOW);
          statusHeater = false;
        }
        // di antara histeresis: biarkan status terakhir (tidak diubah)
      }

      // kipas mengikuti nilai dari web, tapi tidak pernah di bawah SPEED_MIN_PERCENT
      int speedPakai = max(speedKipasWeb, SPEED_MIN_PERCENT);
      int pwmValue = map(speedPakai, 0, 100, 0, 255);
      ledcWrite(PIN_ENA, pwmValue);

      tampilkanLCD(suhuDHT, humDHT, false);
    }

    Serial.printf("DHT22: %.1fC / %.1f%%RH | MAX6675: %.1fC | Heater: %s | Alarm: %s\n",
                  suhuDHT, humDHT, suhuMax,
                  statusHeater ? "ON" : "OFF",
                  statusAlarm ? "YA" : "tidak");

    // Simpan nilai sensor terakhir untuk dikirim ke Firebase di bagian bawah
    gSuhuDHT = suhuDHT;
    gHumDHT  = humDHT;
    gSuhuMax = suhuMax;
  }

  // =========================================================
  // 2) SINKRON FIREBASE (baca setpoint & kecepatan kipas dari web,
  //    kirim status/sensor & heartbeat terakhir ke web)
  // =========================================================
  if (now - lastFirebaseSync >= INTERVAL_FIREBASE) {
    lastFirebaseSync = now;

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      // ---- baca dari web ----
      if (Firebase.getFloat(firebaseData, "/inkubator/setpoint_suhu")) {
        setpointSuhu = firebaseData.floatData();
      }
      if (Firebase.getInt(firebaseData, "/inkubator/speed_kipas")) {
        speedKipasWeb = firebaseData.intData();
      }

      // ---- kirim sensor ke web (cegah kirim NaN jika sensor sempat error) ----
      if (!isnan(gSuhuDHT)) {
        Firebase.setFloat(firebaseData, "/inkubator/suhu_dht", gSuhuDHT);
      }
      if (!isnan(gHumDHT)) {
        Firebase.setFloat(firebaseData, "/inkubator/kelembapan", gHumDHT);
      }
      if (!isnan(gSuhuMax)) {
        Firebase.setFloat(firebaseData, "/inkubator/suhu_thermocouple", gSuhuMax);
      }
      Firebase.setBool(firebaseData, "/inkubator/status_heater", statusHeater);
      Firebase.setBool(firebaseData, "/inkubator/alarm", statusAlarm);

      // ---- kirim heartbeat agar website tahu ESP32 aktif ----
      heartbeatCounter = (heartbeatCounter + 1) % 1000000;
      Firebase.setInt(firebaseData, "/inkubator/heartbeat", heartbeatCounter);
    } else {
      Serial.println("Firebase/WiFi belum siap (cek koneksi/token).");
    }
  }
}

// ------------------- FUNGSI TAMPIL LCD -------------------
void tampilkanLCD(float suhu, float hum, bool alarm) {
  lcd.setCursor(0, 0);
  if (alarm) {
    lcd.print("!! OVERHEAT !!  ");
  } else {
    lcd.print("T:");
    lcd.print(isnan(suhu) ? 0 : suhu, 1);
    lcd.print("C H:");
    lcd.print(isnan(hum) ? 0 : hum, 0);
    lcd.print("%   ");
  }

  lcd.setCursor(0, 1);
  lcd.print("Htr:");
  lcd.print(statusHeater ? "ON " : "OFF");
  lcd.print(" SP:");
  lcd.print(setpointSuhu, 1);
  lcd.print(" ");
}
