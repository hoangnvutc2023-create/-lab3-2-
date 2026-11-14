// LAB 3 - ĐỀ 2: Giám sát môi trường vườn cây công nghiệp (cà phê, hồ tiêu)
// ESP32 + DHT22 + FreeRTOS (2 core) + MQTT
//
// Core 0: taskTransmit - WiFi + MQTT, đọc dataQueue, publish dữ liệu, nhận lệnh bơm/quạt từ web
// Core 1: taskRead     - đọc DHT22, in Serial, đẩy dataQueue + alertQueue
// Core 1: taskAlert    - đọc alertQueue, điều khiển máy phun sương theo độ ẩm
// Bơm và quạt điều khiển thủ công bằng nút trên web.
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ---------- Chân kết nối ----------
#define DHT_PIN   15   // DHT22 SDA
#define PUMP_PIN  26   // LED xanh dương = Máy bơm (điều khiển từ web)
#define FAN_PIN   27   // LED xanh lá    = Quạt     (điều khiển từ web)
#define MIST_PIN  25   // LED vàng (PWM) = Máy phun sương (tự động theo độ ẩm)
#define DHT_TYPE  DHT22

// ---------- WiFi / MQTT ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

// BASE phải giống hệt BASE trong index.html. Broker công cộng dùng chung cho mọi người,
// nên đặt tên riêng để không bị lẫn dữ liệu/lệnh của người khác.
#define BASE "lab3/de2/nvhoang01"
#define T_DATA     BASE "/data"
#define T_CMD_PUMP BASE "/cmd/pump"
#define T_CMD_FAN  BASE "/cmd/fan"
#define T_ST_PUMP  BASE "/state/pump"
#define T_ST_FAN   BASE "/state/fan"

// ---------- PWM (tương thích core 2.x và 3.x) ----------
#define PWM_FREQ 5000
#define PWM_RES  8
#define PWM_CH   0
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define PWM_BEGIN()   ledcAttach(MIST_PIN, PWM_FREQ, PWM_RES)
  #define PWM_WRITE(d)  ledcWrite(MIST_PIN, d)
#else
  #define PWM_BEGIN()   do { ledcSetup(PWM_CH, PWM_FREQ, PWM_RES); ledcAttachPin(MIST_PIN, PWM_CH); } while (0)
  #define PWM_WRITE(d)  ledcWrite(PWM_CH, d)
#endif

// ---------- Dữ liệu dùng chung ----------
struct SensorData { float temp; float hum; };

QueueHandle_t dataQueue;    // dữ liệu đầy đủ (nhiệt độ + độ ẩm) -> task truyền
QueueHandle_t alertQueue;   // dữ liệu nhiệt độ + độ ẩm -> task điều khiển phun sương

volatile int  mistPower  = 0;       // % công suất phun sương hiện tại
volatile bool pumpOn     = false;
volatile bool fanOn      = false;
volatile bool stateDirty = true;    // báo task truyền cần publish lại trạng thái

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient   espClient;
PubSubClient mqtt(espClient);
String       clientId;              // tạo ngẫu nhiên mỗi lần khởi động

// ---------- Bật/tắt thiết bị ----------
void setPump(bool on) {
  if (pumpOn == on) return;
  pumpOn = on; digitalWrite(PUMP_PIN, on); stateDirty = true;
  Serial.printf("[PUMP] %s\n", on ? "BAT" : "TAT");
}
void setFan(bool on) {
  if (fanOn == on) return;
  fanOn = on; digitalWrite(FAN_PIN, on); stateDirty = true;
  Serial.printf("[FAN] %s\n", on ? "BAT" : "TAT");
}
void publishStates() {                       // chỉ gọi từ task truyền (Core 0)
  mqtt.publish(T_ST_PUMP, pumpOn ? "ON" : "OFF", true);
  mqtt.publish(T_ST_FAN,  fanOn  ? "ON" : "OFF", true);
}

// ---------- Nhận lệnh từ Web ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  String t(topic);
  bool on = msg.equalsIgnoreCase("ON");
  Serial.printf("[MQTT] %s -> %s\n", topic, msg.c_str());

  if (t == T_CMD_PUMP) setPump(on);
  else if (t == T_CMD_FAN) setFan(on);
}

void connectWiFi() {
  Serial.print("[WiFi] Dang ket noi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " that bai");
}

void connectMQTT() {
  Serial.printf("[MQTT] Dang ket noi %s (id=%s)\n", MQTT_HOST, clientId.c_str());
  if (mqtt.connect(clientId.c_str())) {
    mqtt.subscribe(T_CMD_PUMP);
    mqtt.subscribe(T_CMD_FAN);
    publishStates();
    Serial.println("[MQTT] Da ket noi broker");
  } else {
    Serial.printf("[MQTT] Loi, rc=%d\n", mqtt.state());
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ======================= TASK 1: TRUYỀN DATA (Core 0) =======================
void taskTransmit(void* pv) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  SensorData d;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    if (WiFi.status() == WL_CONNECTED) {            // WiFi thành công mới làm tiếp
      if (!mqtt.connected()) connectMQTT();
      mqtt.loop();
      if (stateDirty && mqtt.connected()) { stateDirty = false; publishStates(); }
      if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(100)) == pdTRUE && mqtt.connected()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"temp\":%.1f,\"hum\":%.1f,\"mist\":%d,\"pump\":%d,\"fan\":%d}",
                 d.temp, d.hum, (int)mistPower, (int)pumpOn, (int)fanOn);
        if (!mqtt.publish(T_DATA, buf)) Serial.println("[MQTT] Publish that bai");
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

// ======================= TASK 2: ĐỌC DATA (Core 1) ==========================
void taskRead(void* pv) {
  dht.begin();
  for (;;) {
    SensorData d;
    d.hum  = dht.readHumidity();
    d.temp = dht.readTemperature();
    if (isnan(d.hum) || isnan(d.temp)) {
      Serial.println("[DHT22] Loi doc cam bien");
    } else {
      Serial.printf("[DHT22] Nhiet do: %.1f C | Do am: %.1f %% | Phun suong: %d%%\n",
                    d.temp, d.hum, (int)mistPower);
      xQueueSend(dataQueue,  &d, 0);                // dữ liệu đầy đủ -> task truyền
      xQueueSend(alertQueue, &d, 0);                // dữ liệu điều khiển -> task phun sương
    }
    vTaskDelay(pdMS_TO_TICKS(2000));                // DHT22 tối thiểu 2s/lần
  }
}

// ======================= TASK 3: ĐIỀU KHIỂN PHUN SƯƠNG (Core 1) =============
void setMist(int percent) {
  mistPower = percent;
  PWM_WRITE(percent * 255 / 100);
  Serial.printf("[MIST] Cong suat = %d%%\n", percent);
}

void taskAlert(void* pv) {
  int        level = 0;     // công suất phun sương đang chạy
  uint32_t   endMs = 0;     // thời điểm kết thúc chu kỳ phun
  SensorData d;

  for (;;) {
    if (xQueueReceive(alertQueue, &d, pdMS_TO_TICKS(100)) == pdTRUE) {
      int want = 0; uint32_t dur = 0;
      if      (d.hum < 30) { want = 100; dur = 30000; }   // <30%   : 100% / 30s
      else if (d.hum < 35) { want = 50;  dur = 20000; }   // 30-35% : 50%  / 20s
      else if (d.hum < 40) { want = 25;  dur = 10000; }   // 35-40% : 25%  / 10s

      if (want > level) {                  // mức mới nặng hơn -> chạy/leo thang
        level = want;
        endMs = millis() + dur;
        setMist(level);
      } else if (d.hum >= 40 && level > 0) { // độ ẩm về vùng ổn định -> dừng sớm
        level = 0;
        setMist(0);
      }
    }
    if (level > 0 && (int32_t)(millis() - endMs) >= 0) {  // hết thời gian phun
      level = 0;
      setMist(0);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN,  OUTPUT);
  PWM_BEGIN();
  PWM_WRITE(0);

  // Trong Wokwi mọi ESP32 có cùng MAC, nên không dùng MAC làm clientId (sẽ trùng trên broker công cộng)
  clientId = "esp32-garden-" + String(esp_random(), HEX);

  dataQueue  = xQueueCreate(10, sizeof(SensorData));
  alertQueue = xQueueCreate(10, sizeof(SensorData));

  //                    hàm          tên         stack  tham số prio handle core
  xTaskCreatePinnedToCore(taskTransmit, "Transmit", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskRead,     "Read",     4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskAlert,    "Alert",    4096, NULL, 2, NULL, 1);
}

void loop() { vTaskDelete(NULL); }   // mọi việc do FreeRTOS task đảm nhiệm
