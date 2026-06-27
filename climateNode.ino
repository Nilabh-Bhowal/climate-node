#include <WiFi.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <DHT/dht_nonblocking.h>
#include <DHT/dht_nonblocking.cpp>


// ========================
// SENSOR CONFIG
// ========================
#define DHT_PIN 21
#define DHT_TYPE DHT_TYPE_11

DHT_nonblocking dhtSensor(DHT_PIN, DHT_TYPE);


// ========================
// MOTOR DRIVER PINS
// ========================
#define DIRA 27
#define DIRB 26
#define ENABLE 25


// ========================
// WIFI CREDENTIALS
// ========================

// REPLACE SSID AND PASSWORD WITH YOUR WIFI CREDENTIALS
const char* ssid = "SSID";
const char* password = "PASSWORD";


// ========================
// GLOBAL SENSOR STATE
// ========================
float currTemp = NAN;
float currHumidity = NAN;
unsigned long lastDHTRead = 0;


// ========================
// TIME CONFIG (NTP)
// ========================
const char* ntpServer = "pool.ntp.org";
const long gmtOffsetSec = -21600;
const int daylightOffsetSec = 3600;


// ========================
// WEB SERVER
// ========================
WebServer server(80);


// ======================================================
// HANDLE: /log  → stream CSV file from flash (LittleFS)
// ======================================================
void handleLog() {
  File file = LittleFS.open("/log.csv", FILE_READ);

  if (!file) {
    server.send(404, "text/plain", "No log file");
    return;
  }

  server.streamFile(file, "text/plain");
  file.close();
}


// ======================================================
// HANDLE: /data → JSON sensor output for frontend
// ======================================================
void handleData() {

  String json = "{";
  json += "\"temperature\":";
  json += isnan(currTemp) ? "null" : String(currTemp);
  json += ",";
  json += "\"humidity\":";
  json += isnan(currHumidity) ? "null" : String(currHumidity);
  json += "}";

  server.send(200, "application/json", json);
}


// ======================================================
// HANDLE: / → Web dashboard (HTML + Chart.js frontend)
// ======================================================
void handleRoot() {

  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
      <link rel="icon" type="image/png" href="/icon.png">
      <link rel="apple-touch-icon" type="image/png" href="/icon.png">

      <meta name="apple-mobile-web-app-capable" content="yes">
      <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
      <meta name="apple-mobile-web-app-title" content="Climate Node">
      <title>Climate Node</title>

      <style>
        html {
          font-family: Helvetica;
          text-align: center;
          margin: 0 auto;
          color: white;
          background: #121212;
        }
      </style>
    </head>

    <body>
      <h2>Climate Node</h2>

      <h3>Temperature: <span id="temp">--</span>&deg;C</h3>
      <h3>Humidity: <span id="hum">--</span>%</h3>

      <canvas id="tempChart" width="400" height="200"></canvas>
      <canvas id="humChart" width="400" height="200"></canvas>

      <script>
        let tempData = [];
        let humData = [];
        let labels = [];

        const tempCtx = document.getElementById("tempChart").getContext("2d");
        const humCtx = document.getElementById("humChart").getContext("2d");

        const tempChart = new Chart(tempCtx, {
          type: "line",
          data: {
            labels: labels,
            datasets: [{ 
              label: "Temperature (\u00B0C)",
              data: tempData,
              borderColor: "red",
              fill: false
            }]
          },
          options: {
            scales: {
              x: {
                ticks: { color: "#fff" },
                grid: { color: "#333" }
              },
              y: {
                ticks: { color: "#fff" },
                grid: { color: "#333" }
              }
            },
            plugins: {
              legend: {
                labels: { color: "#fff" }
              }
            }
          }
        });

        const humChart = new Chart(humCtx, {
          type: "line",
          data: {
            labels: labels,
            datasets: [{ 
              label: "Humidity (%)",
              data: humData,
              borderColor: "blue",
              fill: false
            }]
          },
          options: {
            scales: {
              x: {
                ticks: { color: "#fff" },
                grid: { color: "#333" }
              },
              y: {
                ticks: { color: "#fff" },
                grid: { color: "#333" }
              }
            },
            plugins: {
              legend: {
                labels: { color: "#fff" }
              }
            }
          }
        });

        async function updateData() {
          const res = await fetch("/data");
          const data = await res.json();

          document.getElementById("temp").textContent = data.temperature ?? "...";
          document.getElementById("hum").textContent = data.humidity ?? "...";

          let now = new Date().toLocaleTimeString();

          labels.push(now);
          tempData.push(data.temperature);
          humData.push(data.humidity);

          if (labels.length > 20) {
            labels.shift();
            tempData.shift();
            humData.shift();
          }

          tempChart.update();
          humChart.update();
        }

        updateData();
        setInterval(updateData, 3000);
      </script>
    </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}


// ======================================================
// LOGGING: append timestamped readings to CSV
// ======================================================
void logData(float temp, float humidity) {

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }

  char timeStr[32];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

  File file = LittleFS.open("/log.csv", FILE_APPEND);

  if (!file) {
    Serial.println("Failed to open log file");
    return;
  }

  file.printf("%s, %d, %d\n", timeStr, (int)temp, (int)humidity);

  file.close();
}


// ======================================================
// SENSOR MEASUREMENT (non-blocking, every 3s)
// ======================================================
static bool measure(float *temp, float *humidity) {

  static unsigned long measurementTimestamp = millis();

  if (millis() - measurementTimestamp > 3000ul) {

    if (dhtSensor.measure(temp, humidity) == true) {

      measurementTimestamp = millis();

      logData(*temp, *humidity);

      return true;
    }
  }

  return false;
}


// ======================================================
// SETUP
// ======================================================
void setup() {

  Serial.begin(115200);

  // mount flash filesystem
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed;");
    return;
  }

  File f = LittleFS.open("/icon.png", "r");
if (!f) {
  Serial.println("ICON MISSING IN LITTLEFS");
} else {
  Serial.println("ICON FOUND");
  f.close();
}

  // connect WiFi
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // time sync for logging timestamps
  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);

  // web routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/log", handleLog);
  server.serveStatic("/icon.png", LittleFS, "/icon.png");

  server.begin();

  // motor setup
  pinMode(DIRA, OUTPUT);
  pinMode(DIRB, OUTPUT);

  // PWM init
  ledcAttach(ENABLE, 2000, 8);
}


// ======================================================
// LOOP
// ======================================================
void loop() {

  // update sensor (non-blocking)
  if (measure(&currTemp, &currHumidity)) {
    lastDHTRead = millis();
  }

  // handle HTTP requests
  server.handleClient();

  // motor direction spinning fan out
  digitalWrite(DIRB, HIGH);
  digitalWrite(DIRA, LOW);

  // fan control logic
  int speed = 0;

  if (currTemp < 24) {
    speed = 0;
  }
  else if (currTemp > 30) {
    speed = 255;
  }
  else {
    speed = map(currTemp, 26, 30, 200, 255);
  }

  // apply PWM
  ledcWrite(ENABLE, speed);
}