#include <Arduino.h>

#include <ArduinoJson.h>
#include <LittleFS.h>

// #include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <PubSubClient.h>

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

struct Config
{
  char mqttServer[64] = "192.168.86.3";
  int mqttPort = 1883;
  char mqttUser[64] = "mqtt";
  char mqttPassword[64] = "xxxx";
  char mqttClientID[64] = "testing123";
};

// Configuration file setup
const char *configFile = "/config.json";
Config config;

#define MAX_DEVICES 4

#define CLK_PIN D5  // or SCK
#define DATA_PIN D7 // or MOSI
#define CS_PIN D4   // or SS

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

WiFiManager wifiManager;

const uint8_t MESG_SIZE = 255;
const uint8_t CHAR_SPACING = 1;
uint8_t SCROLL_DELAY = 30;
uint8_t INTENSITY = 1;

char curMessage[MESG_SIZE];
char newMessage[MESG_SIZE];
bool newMessageAvailable = false;

void scrollDataSink(uint8_t dev, MD_MAX72XX::transformType_t t, uint8_t col) {}

uint8_t scrollDataSource(uint8_t dev, MD_MAX72XX::transformType_t t)
{
  static enum { S_IDLE,
                S_NEXT_CHAR,
                S_SHOW_CHAR,
                S_SHOW_SPACE } state = S_IDLE;
  static char *p;
  static uint16_t curLen, showLen;
  static uint8_t cBuf[8];
  uint8_t colData = 0;

  // finite state machine to control what we do on the callback
  switch (state)
  {
  case S_IDLE:
    p = curMessage;
    if (newMessageAvailable)
    {
      strcpy(curMessage, newMessage);
      newMessageAvailable = false;
    }
    state = S_NEXT_CHAR;
    break;

  case S_NEXT_CHAR:
    if (*p == '\0')
      state = S_IDLE;
    else
    {
      showLen = mx.getChar(*p++, sizeof(cBuf) / sizeof(cBuf[0]), cBuf);
      curLen = 0;
      state = S_SHOW_CHAR;
    }
    break;

  case S_SHOW_CHAR:
    colData = cBuf[curLen++];
    if (curLen < showLen)
      break;

    showLen = (*p != '\0' ? CHAR_SPACING : (MAX_DEVICES * COL_SIZE) / 2);
    curLen = 0;
    state = S_SHOW_SPACE;

  case S_SHOW_SPACE:
    curLen++;
    if (curLen == showLen)
      state = S_NEXT_CHAR;
    break;

  default:
    state = S_IDLE;
  }

  return (colData);
}

void scrollText(void)
{
  static uint32_t prevTime = 0;

  // Is it time to scroll the text?
  if (millis() - prevTime >= SCROLL_DELAY)
  {
    mx.transform(MD_MAX72XX::TSL); // scroll along - the callback will load all the data
    prevTime = millis();           // starting point for next time
  }
}

void setMessage(const char *message)
{
  Serial.printf("Displaying message: %s\n", message);
  curMessage[0] = newMessage[0] = '\0';
  sprintf(curMessage, message);
}

void setIntensity(int intensity)
{
  Serial.printf("Setting display intensity to %i\n", intensity);
  mx.control(MD_MAX72XX::INTENSITY, intensity);
}

void setScrollDelay(int delay)
{
  Serial.printf("Setting scroll delay to %i\n", delay);
  SCROLL_DELAY = delay;
}

// Saves the configuration to a file
void saveConfig(const char *filename, const Config &config)
{
  // Delete existing file, otherwise the configuration is appended to the file
  LittleFS.remove(filename);

  // Open file for writing
  File file = LittleFS.open(filename, "w");

  if (!file)
  {
    Serial.println(F("Failed to create file"));
    return;
  }

  // Allocate a temporary JsonDocument
  // Don't forget to change the capacity to match your requirements.
  // Use arduinojson.org/assistant to compute the capacity.
  StaticJsonDocument<512> json;

  // Set the values in the document
  json["mqttServer"] = config.mqttServer;
  json["mqttPort"] = config.mqttPort;
  json["mqttUser"] = config.mqttUser;
  json["mqttPassword"] = config.mqttPassword;
  json["mqttClientID"] = config.mqttClientID;

  // Serialize JSON to file
  if (serializeJson(json, file) == 0)
  {
    Serial.println(F("Failed to write to file"));
  }

  // Close the file
  file.close();
}

// Loads the configuration from a file
void loadConfig(const char *filename, Config &config)
{
  // Open file for reading
  File file = LittleFS.open(filename, "r");

  // Allocate a temporary JsonDocument
  // Don't forget to change the capacity to match your requirements.
  // Use arduinojson.org/v6/assistant to compute the capacity.
  StaticJsonDocument<512> json;

  // Deserialize the JSON document
  DeserializationError error = deserializeJson(json, file);

  if (error)
    Serial.println(F("Failed to read file, using default configuration"));

  // strcpy(config.mqttServer, json["mqttServer"] | "mtqq.local");
  // strcpy(config.mqttPort, json["mqttPort"] | "1883");
  // strcpy(config.mqttUser, json["mqttUser"] | "xxxx");
  // strcpy(config.mqttPassword, json["mqttPassword"] | "xxxx");
  // strcpy(config.mqttClientID, json["mqttClientID"] | "1234");
  strcpy(config.mqttServer, "192.168.86.3");
  config.mqttPort = 1883;
  strcpy(config.mqttUser, "mqtt");
  strcpy(config.mqttPassword, "xxxx");
  strcpy(config.mqttClientID, "1234");

  // saveConfig(configFile, config);

  // Close the file
  file.close();
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  char messageBuffer[length + 1];

  for (unsigned int i = 0; i < length; i++)
  {
    messageBuffer[i] = (char)payload[i];
  }

  messageBuffer[length] = '\0';

  if (strcmp(topic, "leddisplay/message") == 0)
  {
    setMessage(messageBuffer);
  }

  if (strcmp(topic, "leddisplay/intensity") == 0)
  {
    setIntensity(atoi(messageBuffer));
  }

  if (strcmp(topic, "leddisplay/delay") == 0)
  {
    setScrollDelay(atoi(messageBuffer));
  }
}

void mqttReconnect()
{
  while (!mqttClient.connected())
  {
    Serial.printf("Attempting MQTT connection to %s:%i...", config.mqttServer, config.mqttPort);

    mqttClient.setServer(config.mqttServer, config.mqttPort);
    mqttClient.setCallback(mqttCallback);

    if (mqttClient.connect(config.mqttClientID, config.mqttUser, config.mqttPassword))
    {
      Serial.println("connected");
      mqttClient.subscribe("leddisplay/message");
      mqttClient.subscribe("leddisplay/delay");
      mqttClient.subscribe("leddisplay/intensity");
    }
    else
    {
      Serial.printf("failed, rc=%i trying again in 5 seconds\n", mqttClient.state());
      delay(5000);
    }
  }
}

void configModeCallback(WiFiManager *wifiManager)
{
  char portalWelcome[64];
  sprintf(portalWelcome, "Configuration portal started, connect to %s", wifiManager->getConfigPortalSSID().c_str());
  setMessage(portalWelcome);
}

void saveParamsCallback()
{
  Serial.println("Saving configuration...");
  saveConfig(configFile, config);
}

void setupWiFiManager()
{
  WiFi.mode(WIFI_STA);

  // // Used for debugging
  // wifiManager.resetSettings();

  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveParamsCallback(saveParamsCallback);
  wifiManager.setConfigPortalBlocking(true);

  // Allow MQTT settings to be configured from portal
  WiFiManagerParameter customMqttServer("server", "MQTT server", config.mqttServer, 64);

  char mqttPort[6];
  WiFiManagerParameter customMqttPort("port", "MQTT port", itoa(config.mqttPort, mqttPort, 10), 6);

  WiFiManagerParameter customMqttUser("user", "MQTT user", config.mqttUser, 64);
  WiFiManagerParameter customMqttPassword("password", "MQTT password", config.mqttPassword, 64);
  WiFiManagerParameter customMqttClientID("id", "MQTT Client ID", config.mqttClientID, 64);

  wifiManager.addParameter(&customMqttServer);
  wifiManager.addParameter(&customMqttPort);
  wifiManager.addParameter(&customMqttUser);
  wifiManager.addParameter(&customMqttPassword);
  wifiManager.addParameter(&customMqttClientID);

  setMessage("Connecting to WiFi...");

  wifiManager.setConfigPortalTimeout(60);

  // // Set debug to false on release
  // wifiManager.setDebugOutput(false);

  if (wifiManager.autoConnect())
  {
    setMessage("Connected");
  }
  else
  {
    setMessage("Config portal running");
  }

  // Load configured settings for MQTT
  strcpy(config.mqttServer, customMqttServer.getValue());
  config.mqttPort = atoi(customMqttPort.getValue());
  strcpy(config.mqttUser, customMqttUser.getValue());
  strcpy(config.mqttPassword, customMqttPassword.getValue());
  strcpy(config.mqttClientID, customMqttClientID.getValue());
}

void setupConfig()
{
  while (!LittleFS.begin())
  {
    Serial.println("Failed to init FS");
    delay(1000);
  }

  // Should load default config if run for the first time
  Serial.println(F("Loading configuration..."));
  loadConfig(configFile, config);

  // // Create configuration file
  // Serial.println(F("Saving configuration..."));
  // saveConfig(configFile, config);
}

void setup()
{
  // Reset for debugging
  // ESP.eraseConfig();
  // ESP.reset();

  Serial.begin(115200);

  // setupConfig();
  setupWiFiManager();

  mx.begin();
  setIntensity(1);
  mx.setShiftDataInCallback(scrollDataSource);
  mx.setShiftDataOutCallback(scrollDataSink);

  char welcomeMessage[64];
  sprintf(welcomeMessage, "Connected, listening on: %d.%d.%d.%d", WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3]);

  setMessage(welcomeMessage);
}

void loop()
{
  scrollText();
  wifiManager.process();
  mqttClient.loop();

  if (!mqttClient.connected())
  {
    mqttReconnect();
  }
}
