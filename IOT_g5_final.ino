#include <Wire.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Sensor Pins
#define DHTPIN 4
#define SOIL_PIN 32
#define WATER_PIN 34
#define TEMT6000_PIN 33
#define MH_LIGHT_PIN 36
#define PIR_PIN 23
#define BUZZER_PIN 17
#define RGB_RED_PIN 25
#define RGB_GREEN_PIN 26
#define RGB_BLUE_PIN 14
#define DHTTYPE DHT11

// APDS-9930
#define APDS_ADDR 0x39
DHT dht(DHTPIN, DHTTYPE);

// WiFi credentials
const char* ssid = "Cease Fire"; // Replace with your WiFi SSID
const char* password = "@abcd1234"; // Replace with your WiFi password

// Server details
const char* serverUrl = "http://192.168.1.233:5000/api/sensor-data"; // Replace with actual server IP
const char* apiKey = "IOTg5_key"; // Matches server API key
const char* deviceId = "ESP32_01";

// Retry settings
const int MAX_RETRIES = 3;
const int RETRY_DELAY = 5000; // 5 seconds
const int SERVER_ERROR_DELAY = 30000; // 30 seconds for HTTP 500

// Soil moisture calibration
const int SOIL_DRY = 2061; // ADC value in air (dry)
const int SOIL_WET = 0;    // ADC value in water (wet)

// I2C timeout
const unsigned long I2C_TIMEOUT = 1000; // 1 second

// Timing for HTTP requests
const unsigned long HTTP_INTERVAL = 30000; // 30 seconds
unsigned long lastHttpTime = 0;

bool apdsInitialized = false;

// Motion tracking
bool motionHistory[10] = {false};
int motionIndex = 0;
int motionCount = 0;

// Alert states
bool isLowSoilMoistureActive = false;
bool isFloodingActive = false;
bool isHighTemperatureActive = false;
bool isLowWaterActive = false;
bool isPersistentMotionActive = false;

// Buzzer state
unsigned long lastBuzzerUpdate = 0;
bool buzzerState = false;
int buzzerCycle = 0;
unsigned long buzzerInterval = 0;
int buzzerMaxCycles = 0;
const char* activeAlert = "";

// For persistent motion pattern
int motionBeepCount = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("Serial Initialized"); // Confirm Serial setup
  Wire.begin(21, 22); // SDA=21, SCL=22
  apdsInitialized = initAPDS();
  dht.begin();
  pinMode(PIR_PIN, INPUT);
  pinMode(SOIL_PIN, INPUT);
  pinMode(WATER_PIN, INPUT);
  pinMode(TEMT6000_PIN, INPUT);
  pinMode(MH_LIGHT_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RGB_RED_PIN, OUTPUT);
  pinMode(RGB_GREEN_PIN, OUTPUT);
  pinMode(RGB_BLUE_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  // Initialize RGB LED (Common Cathode: LOW = OFF)
  digitalWrite(RGB_RED_PIN, LOW);
  digitalWrite(RGB_GREEN_PIN, LOW);
  digitalWrite(RGB_BLUE_PIN, LOW);
  
  // Test RGB LED: Set to red for 5 seconds
  setRGBColor(255, 0, 0); // Red
  delay(5000);
  setRGBColor(0, 0, 0); // Off
  
  // Connect to WiFi
  Serial.println("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // Read sensor values
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int soilRaw = analogRead(SOIL_PIN);
  int waterLevel = analogRead(WATER_PIN);
  int lightTEMT = analogRead(TEMT6000_PIN);
  int lightMH = analogRead(MH_LIGHT_PIN);
  int ambientLight = apdsInitialized ? readAmbientLight() : 0;
  int proximity = apdsInitialized ? readProximity() : 0;
  bool motion = digitalRead(PIR_PIN);

  // Validate sensor readings
  bool sensorsValid = true;
  if (isnan(temperature) || temperature < -40 || temperature > 80) {
    Serial.println("Invalid temperature reading");
    sensorsValid = false;
  }
  if (isnan(humidity) || humidity < 0 || humidity > 100) {
    Serial.println("Invalid humidity reading");
    sensorsValid = false;
  }
  // Map soil moisture (0=dry, 100=wet)
  int soilMoisture = map(soilRaw, SOIL_WET, SOIL_DRY, 0, 100);
  soilMoisture = constrain(soilMoisture, 0, 100);
  if (waterLevel <= 0 || waterLevel > 4095) {
    waterLevel = 0;
  }
  if (lightTEMT <= 0 || lightTEMT > 4095) {
    lightTEMT = 0;
  }
  if (lightMH <= 0 || lightMH > 4095) {
    lightMH = 0;
  }

  // Track motion history
  motionHistory[motionIndex] = motion;
  motionIndex = (motionIndex + 1) % 10;
  motionCount = 0;
  for (int i = 0; i < 10; i++) {
    if (motionHistory[i]) motionCount++;
  }
  bool persistentMotion = (motionCount >= 10);

  // Update alert states
  bool prevLowSoilMoisture = isLowSoilMoistureActive;
  bool prevFlooding = isFloodingActive;
  bool prevHighTemperature = isHighTemperatureActive;
  bool prevLowWater = isLowWaterActive;
  bool prevPersistentMotion = isPersistentMotionActive;

  isLowSoilMoistureActive = (soilMoisture < 10);
  isFloodingActive = (soilMoisture > 90);
  isHighTemperatureActive = (temperature > 30);
  isLowWaterActive = (waterLevel < 1000); // Adjusted threshold
  isPersistentMotionActive = persistentMotion;



  // Manage continuous buzzer sound
  if (sensorsValid) {
    // Prioritize alerts: Low Soil Moisture > Flooding > High Temperature > Low Water > Persistent Motion
    if (isLowSoilMoistureActive) {
      soundBuzzer("low_soil_moisture");
    } else if (isFloodingActive) {
      soundBuzzer("flooding");
    } else if (isHighTemperatureActive) {
      soundBuzzer("high_temperature");
    } else if (isLowWaterActive) {
      soundBuzzer("low_water");
    } else if (isPersistentMotionActive) {
      soundBuzzer("persistent_motion");
    } else {
      // No active alerts, stop buzzer
      activeAlert = "";
      noTone(BUZZER_PIN);
    }
  } else {
    // Invalid sensors, stop buzzer
    activeAlert = "";
    noTone(BUZZER_PIN);
  }

  // Update RGB LED based on active alert (Common Cathode)
  if (sensorsValid) {
    // Prioritize alerts
    if (isLowSoilMoistureActive) {
      setRGBColor(255, 0, 0); // Red
    } else if (isFloodingActive) {
      setRGBColor(0, 0, 255); // Blue
    } else if (isHighTemperatureActive) {
      setRGBColor(255, 165, 0); // Orange
    } else if (isLowWaterActive) {
      setRGBColor(0, 255, 255); // Cyan
    } else if (isPersistentMotionActive) {
      setRGBColor(128, 0, 128); // Purple
    } else {
      setRGBColor(0, 255, 0); // Green (Normal)
    }
  } else {
    setRGBColor(0, 255, 0); // Green (Default for invalid sensors)
  }

  // Send data to server every 30 seconds
  unsigned long currentTime = millis();
  if (currentTime - lastHttpTime >= HTTP_INTERVAL) {
    // Print sensor data
    Serial.printf("Temp: %.1f°C, Hum: %.1f%%, Soil: %d%%, Water: %d, TEMT: %d, MH: %d, Amb: %d, Prox: %d, Motion: %s\n",
                  temperature, humidity, soilMoisture, waterLevel, lightTEMT, lightMH, ambientLight, proximity,
                  motion ? "YES" : "NO");

    // Skip sending if critical sensors failed
    if (sensorsValid) {
      // Create JSON payload
      DynamicJsonDocument doc(1024);
      doc["device_id"] = deviceId;
      doc["temperature"] = temperature;
      doc["humidity"] = humidity;
      doc["soil_moisture"] = soilMoisture;
      doc["water_level"] = waterLevel;
      doc["light_temt"] = lightTEMT;
      doc["light_mh"] = lightMH;
      doc["ambient_light"] = ambientLight;
      doc["proximity"] = proximity;
      doc["motion"] = motion;

      String payload;
      serializeJson(doc, payload);

      // Send to server with retries
      int attempts = 0;
      bool success = false;
      while (attempts < MAX_RETRIES && !success) {
        if (WiFi.status() == WL_CONNECTED) {
          HTTPClient http;
          http.begin(serverUrl);
          http.addHeader("Content-Type", "application/json");
          http.addHeader("X-API-Key", apiKey);
          
          int httpResponseCode = http.POST(payload);
          
          if (httpResponseCode == 200) {
            Serial.printf("HTTP Response code: %d\n", httpResponseCode);
            String response = http.getString();
            Serial.println(response);
            success = true;
          } else {
            Serial.printf("HTTP Error: %d\n", httpResponseCode);
            String response = http.getString();
            Serial.println(response);
            attempts++;
            if (httpResponseCode == 500) {
              delay(SERVER_ERROR_DELAY);
            } else {
              delay(RETRY_DELAY);
            }
          }
          http.end();
        } else {
          WiFi.disconnect();
          WiFi.begin(ssid, password);
          delay(5000);
          attempts++;
        }
      }
    }
    
    lastHttpTime = currentTime;
  }
}

// APDS-9930 Functions
bool initAPDS() {
  Wire.beginTransmission(APDS_ADDR);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  writeReg(0x80, 0x00); // Disable all
  delay(10);
  writeReg(0x81, 0xFF); // Enable proximity and ALS
  writeReg(0x8F, 0x40); // Set gain
  writeReg(0x80, 0x0F); // Enable proximity and ALS
  
  Wire.beginTransmission(APDS_ADDR);
  Wire.write(0x92 | 0x80);
  Wire.endTransmission(false);
  Wire.requestFrom(APDS_ADDR, 1);
  if (Wire.available()) {
    byte id = Wire.read();
    if (id == 0x39 || id == 0xAB) {
      return true;
    }
  }
  return false;
}

uint16_t readProximity() {
  return readReg16(0x18);
}

uint16_t readAmbientLight() {
  return readReg16(0x14);
}

void writeReg(byte reg, byte val) {
  Wire.beginTransmission(APDS_ADDR);
  Wire.write(reg | 0x80);
  Wire.write(val);
  Wire.endTransmission();
}

uint16_t readReg16(byte reg) {
  Wire.beginTransmission(APDS_ADDR);
  Wire.write(reg | 0x80);
  if (Wire.endTransmission(false) != 0) {
    return 0;
  }
  
  Wire.requestFrom(APDS_ADDR, 2);
  unsigned long start = millis();
  while (Wire.available() < 2) {
    if (millis() - start > I2C_TIMEOUT) {
      return 0;
    }
  }
  
  return Wire.read() | (Wire.read() << 8);
}

// Alert Functions
void soundBuzzer(const char* alertType) {
  // Only update if the alert type has changed or we're continuing the same alert
  if (strcmp(activeAlert, alertType) != 0) {
    activeAlert = alertType;
    buzzerCycle = 0;
    buzzerState = false;
    lastBuzzerUpdate = 0;
    motionBeepCount = 0;
    noTone(BUZZER_PIN);
  }

  unsigned long currentTime = millis();
  if (currentTime - lastBuzzerUpdate < buzzerInterval) {
    return; // Not time to update buzzer yet
  }

  // Configure pattern based on alert type
  if (strcmp(alertType, "low_soil_moisture") == 0) {
    // Short beeps: 100ms ON, 100ms OFF, 800Hz
    buzzerInterval = buzzerState ? 100 : 100;
    buzzerMaxCycles = -1; // Continuous
    buzzerState = !buzzerState;
    if (buzzerState) {
      tone(BUZZER_PIN, 800, 100);
    } else {
      noTone(BUZZER_PIN);
    }
  } else if (strcmp(alertType, "flooding") == 0) {
    // Medium beeps: 150ms ON, 150ms OFF, 1000Hz
    buzzerInterval = buzzerState ? 150 : 150;
    buzzerMaxCycles = -1; // Continuous
    buzzerState = !buzzerState;
    if (buzzerState) {
      tone(BUZZER_PIN, 1000, 150);
    } else {
      noTone(BUZZER_PIN);
    }
  } else if (strcmp(alertType, "high_temperature") == 0) {
    // Very short beeps: 80ms ON, 80ms OFF, 600Hz
    buzzerInterval = buzzerState ? 80 : 80;
    buzzerMaxCycles = -1; // Continuous
    buzzerState = !buzzerState;
    if (buzzerState) {
      tone(BUZZER_PIN, 600, 80);
    } else {
      noTone(BUZZER_PIN);
    }
  } else if (strcmp(alertType, "low_water") == 0) {
    // Medium beeps: 200ms ON, 200ms OFF, 1200Hz
    buzzerInterval = buzzerState ? 200 : 200;
    buzzerMaxCycles = -1; // Continuous
    buzzerState = !buzzerState;
    if (buzzerState) {
      tone(BUZZER_PIN, 1200, 200);
    } else {
      noTone(BUZZER_PIN);
    }
  } else if (strcmp(alertType, "persistent_motion") == 0) {
    // Staccato: 30ms ON, 30ms OFF, 5 beeps, 500ms pause, 1500Hz
    if (motionBeepCount < 5) {
      buzzerInterval = buzzerState ? 30 : 30;
      buzzerState = !buzzerState;
      if (buzzerState) {
        tone(BUZZER_PIN, 1500, 30);
      } else {
        noTone(BUZZER_PIN);
      }
      if (!buzzerState) motionBeepCount++;
    } else {
      buzzerInterval = 500; // Pause
      buzzerState = false;
      noTone(BUZZER_PIN);
      motionBeepCount = 0;
    }
    buzzerMaxCycles = -1; // Continuous
  }

  lastBuzzerUpdate = currentTime;
}

void setRGBColor(int r, int g, int b) {
  // Common Cathode: Non-inverted PWM, 50% brightness
  analogWrite(RGB_RED_PIN, r / 2);
  analogWrite(RGB_GREEN_PIN, g / 2);
  analogWrite(RGB_BLUE_PIN, b / 2);


}

