#include "root_ca.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <driver/i2s.h>
#include <esp_websocket_client.h>
#include <opus.h>
#include <esp32-hal-rgb-led.h>

SET_LOOP_TASK_STACK_SIZE(32768);
constexpr int button = 39, micRate = 16000, frame = 320;
constexpr auto port = I2S_NUM_0;
Preferences prefs;
WebServer portal(80);
String device, clientId, endpoint, authHeaders, sessionId, activation;
esp_websocket_client_handle_t socketClient;
OpusEncoder *encoder;
OpusDecoder *decoder;
int outputRate = 24000;
bool primePlayback = true;
unsigned sentFrames = 0;
bool recording, paired, audioInstalled, portalOn;
volatile bool helloNeeded, disconnected;
uint32_t retryAt;
struct Packet {
  uint16_t len;
  uint8_t opcode;
  char data[1537];
};
QueueHandle_t incoming;
int16_t samples[2880];

// The ATOM Echo has one GRB pixel on GPIO27. Values are deliberately dim.
enum class Light : uint32_t {
  Connecting = 0x100700, Setup = 0x121000, Idle = 0x000500,
  Listening = 0x180000, Waiting = 0x100014, Speaking = 0x000c18
};
void showLight(Light state) {
  static uint32_t last = UINT32_MAX;
  uint32_t color = static_cast<uint32_t>(state);
  if (last != color) {
    neopixelWrite(27, color >> 16, color >> 8, color);
    last = color;
  }
}
Light conversationLight = Light::Idle;
uint32_t lightSince;
void setConversationLight(Light state) {
  conversationLight = state;
  lightSince = millis();
}
void updateLight() {
  if (portalOn) showLight(Light::Setup);
  else if (!paired || WiFi.status() != WL_CONNECTED) showLight(Light::Connecting);
  else if (recording) showLight(Light::Listening);
  else {
    // Recover if a turn is silent or its final stop event is missing.
    uint32_t elapsed = millis() - lightSince;
    if (conversationLight == Light::Waiting && elapsed > 20000) {
      Serial.println("Turn timed out; reconnecting");
      disconnected = true;
      conversationLight = Light::Idle;
    }
    if (conversationLight == Light::Speaking && elapsed > 2000)
      conversationLight = Light::Idle;
    showLight(conversationLight);
  }
}

void releaseEncoder() {
  if (encoder) {
    opus_encoder_destroy(encoder);
    encoder = nullptr;
  }
}
void releaseDecoder() {
  if (decoder) {
    opus_decoder_destroy(decoder);
    decoder = nullptr;
  }
}
bool prepareEncoder() {
  int err;
  encoder = opus_encoder_create(micRate, 1, OPUS_APPLICATION_VOIP, &err);
  if (!encoder) {
    Serial.printf("Opus encoder error %d, free=%u\n", err, ESP.getFreeHeap());
    return false;
  }
  opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(1));
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
  return true;
}

void stopAudio() {
  if (audioInstalled)
    i2s_driver_uninstall(port);
  audioInstalled = false;
  pinMode(22, OUTPUT);
  digitalWrite(22, LOW);
}

bool startAudio(bool mic, int rate) {
  stopAudio();
  i2s_config_t cfg{};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER |
                          (mic ? I2S_MODE_RX | I2S_MODE_PDM : I2S_MODE_TX));
  cfg.sample_rate = rate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = mic ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  // Give playback 40 ms of DMA coverage at every negotiated sample rate.
  cfg.dma_buf_count = mic ? 4 : 8;
  cfg.dma_buf_len = mic ? 160 : rate / 200;
  cfg.tx_desc_auto_clear = true;
  i2s_pin_config_t pins{};
  pins.mck_io_num = -1;
  pins.bck_io_num = mic ? -1 : 19;
  pins.ws_io_num = 33;
  pins.data_out_num = mic ? -1 : 22;
  pins.data_in_num = mic ? 23 : -1;
  audioInstalled = i2s_driver_install(port, &cfg, 0, nullptr) == ESP_OK;
  if (!audioInstalled || i2s_set_pin(port, &pins) != ESP_OK) {
    stopAudio();
    return false;
  }
  return true;
}

// Fill both I2S slots so the amplifier receives the same ordered mono samples
// on either channel. Keep the level supplied by the Opus decoder.
size_t playMono(const int16_t *pcm, size_t count) {
  int16_t stereo[160 * 2];
  size_t played = 0;
  while (played < count) {
    size_t frames = std::min<size_t>(160, count - played);
    for (size_t i = 0; i < frames; ++i)
      stereo[2 * i] = stereo[2 * i + 1] = pcm[played + i];
    size_t bytes = 0;
    if (i2s_write(port, stereo, frames * 4, &bytes, pdMS_TO_TICKS(200)) != ESP_OK || bytes != frames * 4)
      break;
    played += frames;
  }
  return played;
}

String field(cJSON *obj, const char *key) {
  auto item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

void setupPortal() {
  if (portalOn)
    return;
  WiFi.mode(WIFI_AP_STA);
  String pass = prefs.getString("apkey");
  if (!pass.length()) {
    pass = String(esp_random(), HEX) + String(esp_random(), HEX);
    prefs.putString("apkey", pass);
  }
  String name =
      "Xiaozhi-Echo-" + device.substring(12, 14) + device.substring(15);
  WiFi.softAP(name.c_str(), pass.c_str());
  Serial.printf("SETUP %s password=%s http://192.168.4.1\n", name.c_str(),
                pass.c_str());
  portal.on("/", HTTP_GET, [] {
    String html = "<meta charset=utf-8><meta name=viewport "
                  "content='width=device-width'><h2>ATOM Echo 小智</h2>";
    if (activation.length())
      html += "<p>小智设备激活码 " + activation + "</p>";
    html += "<form method=post action=/wifi><label>2.4 GHz Wi-Fi <input "
            "required name=s maxlength=32></label><p><label>密码 <input name=p "
            "type=password "
            "maxlength=63></label></p><button>保存并连接</button></form>";
    portal.send(200, "text/html; charset=utf-8", html);
  });
  portal.on("/wifi", HTTP_POST, [] {
    String ssid = portal.arg("s"), pass = portal.arg("p");
    if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) {
      portal.send(400, "text/plain", "Invalid Wi-Fi settings");
      return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("wifi", pass);
    portal.send(200, "text/plain; charset=utf-8",
                "已保存。请稍后刷新首页查看激活码。");
    WiFi.begin(ssid.c_str(), pass.c_str());
  });
  portal.begin();
  portalOn = true;
}

void socketEvent(void *, esp_event_base_t, int32_t id, void *arg) {
  if (id == WEBSOCKET_EVENT_CONNECTED) {
    helloNeeded = true;
    return;
  }
  if (id == WEBSOCKET_EVENT_DISCONNECTED) {
    disconnected = true;
    return;
  }
  if (id != WEBSOCKET_EVENT_DATA)
    return;
  auto *e = static_cast<esp_websocket_event_data_t *>(arg);
  if ((e->op_code != 1 && e->op_code != 2) || e->data_len <= 0 ||
      e->data_len != e->payload_len || e->data_len > 1536)
    return;
  Packet pkt{};
  pkt.len = e->data_len;
  pkt.opcode = e->op_code;
  memcpy(pkt.data, e->data_ptr, pkt.len);
  if (!xQueueSend(incoming, &pkt, pdMS_TO_TICKS(200)))
    Serial.println("Playback queue full");
}

void connectCloud() {
  WiFiClientSecure tls;
  tls.setCACert(rootCA);
  HTTPClient http;
  if (!http.begin(tls, "https://api.tenclass.net/xiaozhi/ota/"))
    return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Device-Id", device);
  http.addHeader("Client-Id", clientId);
  int status = http.POST("{\"version\":2,\"application\":{\"version\":\"0.1."
                         "0\"},\"board\":{\"type\":\"atom-echo-pico\"}}");
  if (status != 200) {
    Serial.printf("OTA HTTP %d\n", status);
    http.end();
    return;
  }
  auto root = cJSON_Parse(http.getString().c_str());
  http.end();
  tls.stop();
  activation = field(cJSON_GetObjectItem(root, "activation"), "code");
  auto ws = cJSON_GetObjectItem(root, "websocket");
  endpoint = field(ws, "url");
  String token = field(ws, "token");
  cJSON_Delete(root);
  if (activation.length()) {
    Serial.printf("Activation code: %s\n", activation.c_str());
    setupPortal();
    return;
  }
  if (!endpoint.startsWith("wss://") || token.isEmpty())
    return;
  if (!token.startsWith("Bearer "))
    token = "Bearer " + token;
  authHeaders = "Device-Id: " + device + "\r\nClient-Id: " + clientId +
                "\r\nProtocol-Version: 1\r\nAuthorization: " + token + "\r\n";
  esp_websocket_client_config_t cfg{};
  cfg.uri = endpoint.c_str();
  cfg.cert_pem = rootCA;
  cfg.headers = authHeaders.c_str();
  cfg.buffer_size = 2048;
  cfg.task_stack = 8192;
  cfg.disable_auto_reconnect = true;
  socketClient = esp_websocket_client_init(&cfg);
  if (!socketClient)
    return;
  esp_websocket_register_events(socketClient, WEBSOCKET_EVENT_ANY, socketEvent,
                                nullptr);
  if (esp_websocket_client_start(socketClient) != ESP_OK)
    disconnected = true;
}
bool sendText(const String &text) {
  return socketClient && esp_websocket_client_send_text(
                             socketClient, text.c_str(), text.length(),
                             pdMS_TO_TICKS(1000)) == (int)text.length();
}
bool listen(bool on) {
  return sendText("{\"type\":\"listen\",\"session_id\":\"" + sessionId +
           "\",\"mode\":\"manual\",\"state\":\"" + (on ? "start" : "stop") +
           "\"}");
}
void handlePacket(Packet &pkt) {
  if (pkt.opcode == 2) {
    if (recording || !paired)
      return;
    if (!decoder) {
      int err;
      decoder = opus_decoder_create(outputRate, 1, &err);
      if (!decoder) {
        Serial.printf("Opus decoder error %d\n", err);
        return;
      }
    }
    if (primePlayback) {
      // The socket task can collect the next packet while we wait. A short
      // reply or a delayed packet must never keep the first one waiting.
      Packet next;
      xQueuePeek(incoming, &next, pdMS_TO_TICKS(35));
      primePlayback = false;
    }
    if (!audioInstalled && !startAudio(false, outputRate))
      return;
    int n =
        opus_decode(decoder, (uint8_t *)pkt.data, pkt.len, samples, 2880, 0);
    if (n > 0 && playMono(samples, n) == size_t(n)) {
      if (conversationLight != Light::Speaking) Serial.println("Reply audio playing");
      setConversationLight(Light::Speaking);
    } else {
      Serial.printf("Playback failed: decoded=%d\n", n);
      disconnected = true;
    }
    return;
  }
  auto msg = cJSON_Parse(pkt.data);
  String type = field(msg, "type");
  if (type == "hello") {
    sessionId = field(msg, "session_id");
    auto rate = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "audio_params"),
                                    "sample_rate");
    outputRate = cJSON_IsNumber(rate) ? rate->valueint : 24000;
    releaseDecoder();
    paired = sessionId.length() && (outputRate == 16000 ||
                                    outputRate == 24000 || outputRate == 48000);
    if (paired) {
      setConversationLight(Light::Idle);
      Serial.println("Ready: hold button to talk");
      if (portalOn) {
        portal.stop();
        WiFi.softAPdisconnect(true);
        portalOn = false;
      }
    }
  } else if (type == "tts" && field(msg, "state") == "start") {
    primePlayback = true;
    if (decoder) opus_decoder_ctl(decoder, OPUS_RESET_STATE);
  } else if (type == "tts" && field(msg, "state") == "stop" && !recording) {
    setConversationLight(Light::Idle);
  } else if (type == "stt" || type == "llm")
    Serial.println(field(msg, "text"));
  cJSON_Delete(msg);
}

void setup() {
  Serial.begin(115200);
  showLight(Light::Connecting);
  pinMode(button, INPUT);
  prefs.begin("xiaozhi", false);
  WiFi.mode(WIFI_STA);
  device = WiFi.macAddress();
  device.toLowerCase();
  clientId = prefs.getString("client");
  if (clientId.isEmpty()) {
    char id[37];
    snprintf(id, sizeof(id), "%08x-%04x-4%03x-8%03x-%04x%08x", esp_random(),
             esp_random() & 65535, esp_random() & 4095, esp_random() & 4095,
             esp_random() & 65535, esp_random());
    clientId = id;
    prefs.putString("client", id);
  }
  incoming = xQueueCreate(7, sizeof(Packet));
  if (!prepareEncoder() || !incoming) {
    Serial.println("Audio allocation failed");
    for (;;)
      delay(1000);
  }

  if (startAudio(true, micRate)) {
    size_t got = 0;
    int peak = 0;
    for (int j = 0; j < 8; j++) {
      i2s_read(port, samples, frame * 2, &got, pdMS_TO_TICKS(100));
      if (j > 2)
        for (size_t i = 0; i < got / 2; i++)
          peak = max(peak, abs((int)samples[i]));
    }
    uint8_t out[256];
    int n = opus_encode(encoder, samples, frame, out, sizeof(out));
    int decErr;
    releaseEncoder();
    auto testDecoder = opus_decoder_create(micRate, 1, &decErr);
    int decoded = n > 0 && testDecoder
                      ? opus_decode(testDecoder, out, n, samples, frame, 0)
                      : 0;
    if (testDecoder)
      opus_decoder_destroy(testDecoder);
    Serial.printf("ATOM Echo mic peak=%d Opus bytes=%d decoded samples=%d\n",
                  peak, n, decoded);
    if (startAudio(false, micRate)) {
      // Brief, low-volume startup chime exercises the speaker path after
      // capture.
      for (int i = 0; i < frame; i++)
        samples[i] = int16_t(500 * sin(2 * PI * 800 * i / micRate));
      size_t played = 0;
      for (int j = 0; j < 5; j++)
        played += playMono(samples, frame);
      delay(45);
      Serial.printf("Speaker stereo frames=%u\n", (unsigned)played);
    }
    stopAudio();
  }
  releaseEncoder();
  String ssid = prefs.getString("ssid");
  if (ssid.length())
    WiFi.begin(ssid.c_str(), prefs.getString("wifi").c_str());
  else
    WiFi.begin();
  configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org");
  if (!digitalRead(button) || !ssid.length())
    setupPortal();
}

void loop() {
  updateLight();
  if (portalOn)
    portal.handleClient();
  if (disconnected) {
    disconnected = false;
    paired = false;
    helloNeeded = false;
    recording = false;
    stopAudio();
    releaseEncoder();
    releaseDecoder();
    if (socketClient) {
      esp_websocket_client_destroy(socketClient);
      socketClient = nullptr;
    }
    xQueueReset(incoming);
    retryAt = millis() + 2000;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() > 15000)
      setupPortal();
    delay(5);
    return;
  }
  if (!socketClient && time(nullptr) > 1700000000 &&
      (int32_t)(millis() - retryAt) >= 0) {
    retryAt = millis() + 10000;
    connectCloud();
  }
  if (helloNeeded) {
    helloNeeded = false;
    sendText("{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
             "\"channels\":1,\"frame_duration\":20}}");
  }
  Packet pkt;
  if (xQueueReceive(incoming, &pkt, 0))
    handlePacket(pkt);
  bool pressed = !digitalRead(button);
  if (paired && pressed && !recording) {
    if (conversationLight == Light::Speaking || conversationLight == Light::Waiting)
      sendText("{\"type\":\"abort\",\"session_id\":\"" + sessionId + "\"}");
    releaseDecoder();
    if (startAudio(true, micRate) && prepareEncoder()) {
      sentFrames = 0;
      recording = listen(true);
      if (!recording) disconnected = true;
    }
  }
  if (recording && !pressed) {
    stopAudio();
    releaseEncoder();
    recording = false;
    primePlayback = true;
    bool submitted = listen(false);
    Serial.printf("Turn submitted=%d frames=%u\n", submitted, sentFrames);
    if (!submitted) disconnected = true;
    setConversationLight(submitted && sentFrames ? Light::Waiting : Light::Idle);
  }
  if (recording) {
    size_t got;
    uint8_t out[256];
    if (i2s_read(port, samples, frame * 2, &got, pdMS_TO_TICKS(50)) == ESP_OK &&
        got == frame * 2) {
      int n = opus_encode(encoder, samples, frame, out, sizeof(out));
      if (n > 0) {
        if (esp_websocket_client_send_bin(socketClient, (char *)out, n, pdMS_TO_TICKS(250)) == n)
          ++sentFrames;
        else
          disconnected = true;
      }
    }
  } else
    delay(1);
}
