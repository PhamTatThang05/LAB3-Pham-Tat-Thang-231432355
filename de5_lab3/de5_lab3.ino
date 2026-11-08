/*
 * Mạch phát hiện té ngã - ESP32 + MPU6050 + MQTT + FreeRTOS
 *
 * Core 0: taskNet   - kết nối WiFi, MQTT, lấy dữ liệu từ dataQueue và publish
 * Core 1: taskRead  - đọc MPU6050, tính gia tốc/góc nghiêng, phát hiện té ngã,
 *                     in Serial, đẩy dữ liệu vào dataQueue và alertQueue
 * Core 1: taskAlert - lấy dữ liệu từ alertQueue, điều khiển LED / còi
 */
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>

// ---------------- Cấu hình ----------------
const char*    WIFI_SSID = "Wokwi-GUEST";   // WiFi mô phỏng của Wokwi
const char*    WIFI_PASS = "";
const char*    MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// ĐỔI chuỗi này thành tên riêng của nhóm (broker công cộng, ai cũng thấy topic)
#define TOPIC_PREFIX "fallgroup01/falldetect"
#define TOPIC_DATA   TOPIC_PREFIX "/data"
#define TOPIC_LED    TOPIC_PREFIX "/cmd/led"
#define TOPIC_BUZZER TOPIC_PREFIX "/cmd/buzzer"

#define PIN_SDA    21
#define PIN_SCL    22
#define PIN_LED    25
#define PIN_BUZZER 26

const float    ACC_TH         = 2.5f;   // g: ngưỡng va chạm (gia tốc đột ngột)
const float    ANGLE_TH       = 45.0f;  // độ: góc nghiêng thay đổi trong ~0.5 s
const uint32_t FALL_WINDOW_MS = 2000;   // va chạm + đổi góc phải xảy ra trong khoảng này
const uint32_t FALL_HOLD_MS   = 8000;   // thời gian giữ cảnh báo
const uint32_t SAMPLE_MS      = 50;     // chu kỳ đọc cảm biến (20 Hz)
const uint8_t  SEND_EVERY     = 4;      // 4 mẫu/lần -> gửi/in mỗi 200 ms
#define HIST_N 10                       // 10 mẫu x 50 ms = 0.5 s

// ---------------- Dữ liệu dùng chung ----------------
struct SensorData {
  float ax, ay, az;   // g
  float acc;          // |a| (g)
  float tilt;         // góc giữa trục Z và phương thẳng đứng (độ)
  bool  fall;
};

QueueHandle_t dataQueue;    // taskRead -> taskNet
QueueHandle_t alertQueue;   // taskRead -> taskAlert

volatile bool manualLed    = false;  // trạng thái từ nút web
volatile bool manualBuzzer = false;
volatile bool ackRequest   = false;  // web bấm "tắt" -> xác nhận, tắt báo động

Adafruit_MPU6050 mpu;
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);

// ---------------- Còi (tương thích core Arduino-ESP32 2.x và 3.x) ----------------
void buzzerInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);
#endif
}
void buzzerSet(bool on) {
  int f = on ? 2000 : 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, f);
#else
  ledcWriteTone(0, f);
#endif
}

// Gửi vào queue, nếu đầy thì bỏ mẫu cũ nhất để luôn giữ dữ liệu mới
void sendLatest(QueueHandle_t q, const SensorData* d) {
  if (xQueueSend(q, d, 0) != pdTRUE) {
    SensorData tmp;
    xQueueReceive(q, &tmp, 0);
    xQueueSend(q, d, 0);
  }
}

// ---------------- Task đọc data (Core 1) ----------------
void taskRead(void*) {
  Wire.begin(PIN_SDA, PIN_SCL);
  while (!mpu.begin(0x68, &Wire)) {
    Serial.println("Khong tim thay MPU6050, kiem tra day noi!");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);

  float    hist[HIST_N];
  uint8_t  idx = 0, cnt = 0;
  bool     histReady = false, hasImpact = false, hasTilt = false;
  uint32_t lastImpact = 0, lastTilt = 0, fallUntil = 0;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);

    SensorData d;
    d.ax  = a.acceleration.x / 9.80665f;
    d.ay  = a.acceleration.y / 9.80665f;
    d.az  = a.acceleration.z / 9.80665f;
    d.acc = sqrtf(d.ax * d.ax + d.ay * d.ay + d.az * d.az);
    d.tilt = (d.acc > 0.05f)
               ? acosf(constrain(d.az / d.acc, -1.0f, 1.0f)) * 57.29578f
               : 0.0f;

    if (!histReady) {                       // lần đầu: điền đầy lịch sử
      for (uint8_t i = 0; i < HIST_N; i++) hist[i] = d.tilt;
      histReady = true;
    }
    float tiltChange = fabsf(d.tilt - hist[idx]);   // so với 0.5 s trước
    hist[idx] = d.tilt;
    idx = (idx + 1) % HIST_N;

    uint32_t now = millis();
    if (d.acc > ACC_TH)         { lastImpact = now; hasImpact = true; }
    if (tiltChange > ANGLE_TH)  { lastTilt   = now; hasTilt   = true; }

    if (hasImpact && hasTilt &&
        (now - lastImpact < FALL_WINDOW_MS) && (now - lastTilt < FALL_WINDOW_MS)) {
      fallUntil = now + FALL_HOLD_MS;
      hasImpact = hasTilt = false;
    }
    d.fall = (now < fallUntil);

    sendLatest(alertQueue, &d);             // mọi mẫu -> task cảnh báo

    if (++cnt >= SEND_EVERY) {              // 200 ms/lần -> Serial + task truyền
      cnt = 0;
      Serial.printf("ax=%.2f ay=%.2f az=%.2f g | |a|=%.2f g | tilt=%.1f deg | %s\n",
                    d.ax, d.ay, d.az, d.acc, d.tilt, d.fall ? "TE NGA!" : "binh thuong");
      sendLatest(dataQueue, &d);
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

// ---------------- Task cảnh báo (Core 1) ----------------
void taskAlert(void*) {
  bool fall = false, muted = false, prevBz = false;
  for (;;) {
    SensorData d;
    if (xQueueReceive(alertQueue, &d, pdMS_TO_TICKS(50)) == pdTRUE) fall = d.fall;

    if (ackRequest) { muted = true; ackRequest = false; }
    if (!fall) muted = false;               // hết té ngã -> cho phép báo lần sau

    bool alarm = fall && !muted;
    bool blink = (millis() / 200) % 2;      // nhấp nháy 2.5 Hz khi báo động
    bool led = alarm ? blink : manualLed;
    bool bz  = alarm ? blink : manualBuzzer;

    digitalWrite(PIN_LED, led);
    if (bz != prevBz) { buzzerSet(bz); prevBz = bz; }
  }
}

// ---------------- Task truyền data (Core 0) ----------------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  bool on = (len >= 2 && payload[0] == 'O' && payload[1] == 'N');
  if (!strcmp(topic, TOPIC_LED))         manualLed = on;
  else if (!strcmp(topic, TOPIC_BUZZER)) manualBuzzer = on;
  if (!on) ackRequest = true;               // nút "tắt" cũng xác nhận báo động
}

void taskNet(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.print("\nWiFi OK, IP: ");
  Serial.println(WiFi.localIP());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  String clientId = "esp32-fall-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!mqtt.connected()) {
      if (mqtt.connect(clientId.c_str())) {
        mqtt.subscribe(TOPIC_LED);
        mqtt.subscribe(TOPIC_BUZZER);
        Serial.println("MQTT OK");
      } else {
        Serial.printf("MQTT loi, rc=%d\n", mqtt.state());
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();

    SensorData d;
    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(50)) == pdTRUE) {
      char buf[160];
      snprintf(buf, sizeof(buf),
               "{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"acc\":%.2f,\"tilt\":%.1f,"
               "\"fall\":%d,\"led\":%d,\"buzzer\":%d}",
               d.ax, d.ay, d.az, d.acc, d.tilt, d.fall, manualLed, manualBuzzer);
      mqtt.publish(TOPIC_DATA, buf);
    }
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  buzzerInit();

  dataQueue  = xQueueCreate(10, sizeof(SensorData));
  alertQueue = xQueueCreate(5,  sizeof(SensorData));

  xTaskCreatePinnedToCore(taskNet,   "TaskTruyenData", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskRead,  "TaskDocData",    6144, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskAlert, "TaskCanhBao",    4096, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);   // toàn bộ việc chạy trong các task
}
