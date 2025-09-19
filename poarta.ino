#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// --- WiFi ---
const char* ssid = "Guest";
const char* pass = "kablemguest";

// --- MQTT ---
const char* mqtt_host = "broker.emqx.io";
const int   mqtt_port = 1883;  // pentru TLS: 8883 + WiFiClientSecure
const char* client_id = "esp_gate_1";
const char* cmdTopic = "kablem/gates/1/cmd";
const char* statusTopic = "kablem/gates/1/status";
const char* progressTopic = "kablem/gates/1/progress";

WiFiClient espClient;
PubSubClient mqtt(espClient);

// Starea porții
String state = "STOP";
int progress = 0;

void publishStatus(const char* s) {
  mqtt.publish(statusTopic, s, true);
  Serial.print("Publish status → "); Serial.println(s);
}
void publishProgress(int p) {
  char buf[8]; snprintf(buf, sizeof(buf), "%d", p);
  mqtt.publish(progressTopic, buf, false);
  Serial.print("Publish progres → "); Serial.println(buf);
}

void handleCmd(String cmd) {
  cmd.trim();
  Serial.print("Comandă primită: "); Serial.println(cmd);

  if (cmd == "OPEN") {
    state = "OPENING";
    publishStatus(state.c_str());
    // TODO: pornește motor/deschidere
  } else if (cmd == "CLOSE") {
    state = "CLOSING";
    publishStatus(state.c_str());
    // TODO: pornește motor/închidere
  } else if (cmd == "STOP") {
    state = "STOP";
    publishStatus(state.c_str());
    // TODO: oprește motor
  } else if (cmd == "STATUS?") {
    publishStatus(state.c_str());
    publishProgress(progress);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg; msg.reserve(len);
  for (unsigned int i=0;i<len;i++) msg += (char)payload[i];

  Serial.print("MQTT mesaj de pe topicul [");
  Serial.print(topic); Serial.print("] → ");
  Serial.println(msg);

  if (String(topic) == cmdTopic) {
    handleCmd(msg);
  }
}

void reconnect() {
  while (!mqtt.connected()) {
    Serial.print("Conectare MQTT...");
    if (mqtt.connect(client_id)) {
      Serial.println("OK");
      mqtt.subscribe(cmdTopic, 1);
      Serial.print("Subscribe la "); Serial.println(cmdTopic);
      publishStatus("ONLINE");
    } else {
      Serial.print("Eșec, rc=");
      Serial.print(mqtt.state());
      Serial.println(" → reîncercare în 2s");
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, pass);
  Serial.print("Conectare WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(" OK");

  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setCallback(mqttCallback);
  reconnect();
}

void loop() {
  if (!mqtt.connected()) {
    reconnect();
  }
  mqtt.loop();

  // Exemplu dummy de progres (simulare)
  static unsigned long last = 0;
  if (millis() - last > 200) {
    last = millis();
    if (state == "OPENING" && progress < 100) {
      progress += 1;
      publishProgress(progress);
      if (progress >= 100) { state = "OPEN"; publishStatus("OPEN"); }
    }
    if (state == "CLOSING" && progress > 0) {
      progress -= 1;
      publishProgress(progress);
      if (progress <= 0) { state = "CLOSED"; publishStatus("CLOSED"); }
    }
  }
}
