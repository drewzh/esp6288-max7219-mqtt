#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <MD_Parola.h>
#include <MD_MAX72XX.h>
#include <SPI.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// Define the connections for the LED matrix
#define CLK_PIN D5  // or SCK
#define DATA_PIN D7 // or MOSI
#define CS_PIN D4   // or SS
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

// Button configuration
#define RESET_BUTTON_PIN D3
#define RESET_HOLD_TIME 5000 // Hold button for 5 seconds to reset

// Configuration file path
const char *configFilePath = "/config.json";

// Initialize the display
MD_Parola display = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// MQTT settings
char mqtt_server[40] = "";
char mqtt_port[6] = "1883";
char mqtt_user[20] = "";
char mqtt_pass[20] = "";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Available effects
const char *effects[] = {
    "PA_PRINT", "PA_SCROLL_LEFT", "PA_SCROLL_RIGHT", "PA_SCROLL_UP", "PA_SCROLL_DOWN",
    "PA_SLICE", "PA_MESH", "PA_FADE", "PA_WIPE", "PA_WIPE_CURSOR", "PA_BLINDS", "PA_DISSOLVE",
    "PA_RANDOM", "PA_OPENING", "PA_OPENING_CURSOR", "PA_CLOSING", "PA_CLOSING_CURSOR",
    "PA_SCROLL_UP_LEFT", "PA_SCROLL_UP_RIGHT", "PA_SCROLL_DOWN_LEFT", "PA_SCROLL_DOWN_RIGHT",
    "PA_SCAN_HORIZ", "PA_SCAN_VERT", "PA_GROW_UP", "PA_GROW_DOWN"};

// Function declarations
void saveConfig();
void loadConfig();
void connectToMqtt();
void setMessage(const char *message, textEffect_t effect = PA_SCROLL_LEFT);
void setIntensity(int intensity);
void setScrollDelay(int delay);
void mqttCallback(char *topic, byte *payload, unsigned int length);
textEffect_t getEffectFromString(const String &effectStr);
void checkResetButton();
bool validateMQTTConfig();

void setup()
{
    Serial.begin(115200);

    // Initialize LittleFS
    if (!LittleFS.begin())
    {
        Serial.println("Failed to mount file system");
        setMessage("FS Mount Fail");
        return;
    }

    // Load configuration from LittleFS
    loadConfig();

    // Debug prints to verify values after loading config
    Serial.println("After loading config:");
    Serial.print("MQTT Server: ");
    Serial.println(mqtt_server);
    Serial.print("MQTT Port: ");
    Serial.println(mqtt_port);
    Serial.print("MQTT User: ");
    Serial.println(mqtt_user);
    Serial.print("MQTT Pass: ");
    Serial.println(mqtt_pass);

    // Initialize the display
    if (!display.begin())
    {
        Serial.println("Failed to initialize display");
        setMessage("Display Init Fail");
        while (true)
            ; // Halt execution if display fails to initialize
    }
    display.setIntensity(1);
    display.displayClear();

    // WiFi Manager
    WiFiManager wifiManager;
    wifiManager.setDarkMode(true);

    WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
    WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
    WiFiManagerParameter custom_mqtt_user("user", "MQTT User", mqtt_user, 20);
    WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", mqtt_pass, 20);

    // Adding custom parameters for MQTT server details
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);

    // Auto connect to WiFi
    setMessage("Connecting to WiFi", PA_SCROLL_LEFT);
    if (!wifiManager.autoConnect("ESP8266_Config_AP"))
    {
        Serial.println("Failed to connect and hit timeout");
        setMessage("WiFi Conn Fail");
        delay(3000);
        ESP.restart();
    }

    setMessage("WiFi Connected");
    Serial.println("WiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    String ipAddress = "IP: " + WiFi.localIP().toString();
    setMessage(ipAddress.c_str(), PA_SCROLL_LEFT);

    // Store custom parameters
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());

    // Save the configuration to LittleFS
    saveConfig();

    // Debug prints to verify values before MQTT setup
    Serial.println("Before MQTT setup:");
    Serial.print("MQTT Server: ");
    Serial.println(mqtt_server);
    Serial.print("MQTT Port: ");
    Serial.println(mqtt_port);
    Serial.print("MQTT User: ");
    Serial.println(mqtt_user);
    Serial.print("MQTT Pass: ");
    Serial.println(mqtt_pass);

    // Setup MQTT
    mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    mqttClient.setCallback(mqttCallback);

    // Validate MQTT configuration and connect to MQTT
    if (!validateMQTTConfig())
    {
        setMessage("MQTT Config Invalid");
        Serial.println("MQTT Config Invalid, opening AP for reconfiguration...");
        wifiManager.startConfigPortal("ESP8266_Config_AP");

        // Store new parameters after AP configuration
        strcpy(mqtt_server, custom_mqtt_server.getValue());
        strcpy(mqtt_port, custom_mqtt_port.getValue());
        strcpy(mqtt_user, custom_mqtt_user.getValue());
        strcpy(mqtt_pass, custom_mqtt_pass.getValue());

        // Save the new configuration to LittleFS
        saveConfig();

        // Setup MQTT with new parameters
        mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    }

    // Connect to MQTT
    connectToMqtt();

    // Setup reset button
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
}

void loop()
{
    if (!mqttClient.connected())
    {
        connectToMqtt();
    }
    mqttClient.loop();
    display.displayAnimate();
    checkResetButton();
}

void connectToMqtt()
{
    while (!mqttClient.connected())
    {
        char message[128];
        sprintf(message, "Attempting MQTT connection to %s:%s", mqtt_server, mqtt_port);
        Serial.println(message);
        setMessage("Connecting to MQTT");

        if (mqttClient.connect("ESP8266mqttClient", mqtt_user, mqtt_pass))
        {
            Serial.println("connected");
            setMessage("MQTT Connected");
            mqttClient.subscribe("leddisplay/message");
            mqttClient.subscribe("leddisplay/intensity");
            mqttClient.subscribe("leddisplay/delay");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" try again in 5 seconds");
            setMessage("MQTT Conn Fail");
            delay(5000);
        }
    }
}

void setMessage(const char *message, textEffect_t effect)
{
    Serial.printf("Displaying message: %s\n", message);
    // Display the message
    display.displayClear();
    display.displayText(message, PA_CENTER, 50, 2000, effect, effect);

    while (!display.displayAnimate())
    {
        // Wait for the display to finish animating
        yield(); // Allow system to process other tasks to prevent WDT reset
    }
}

void setIntensity(int intensity)
{
    Serial.printf("Setting display intensity to %i\n", intensity);
    display.setIntensity(intensity);
}

void setScrollDelay(int delay)
{
    Serial.printf("Setting scroll delay to %i\n", delay);
    display.setSpeed(delay);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");

    // Allocate a buffer to store the payload
    char *messageBuffer = new char[length + 1];
    if (!messageBuffer)
    {
        Serial.println("Memory allocation failed");
        return;
    }

    for (unsigned int i = 0; i < length; i++)
    {
        messageBuffer[i] = (char)payload[i];
    }
    messageBuffer[length] = '\0'; // Null-terminate the string

    Serial.print("Payload: ");
    Serial.println(messageBuffer);

    if (strcmp(topic, "leddisplay/message") == 0)
    {
        // Parse JSON
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, messageBuffer);
        if (error)
        {
            Serial.println("Failed to parse JSON");
            setMessage("JSON Parse Fail");
        }
        else
        {
            const char *message = doc["message"];
            const char *effect = doc["effect"];

            textEffect_t effectValue = PA_SCROLL_LEFT; // Default effect
            if (effect)
            {
                effectValue = getEffectFromString(effect);
            }

            setMessage(message, effectValue);
        }
    }
    else if (strcmp(topic, "leddisplay/intensity") == 0)
    {
        setIntensity(atoi(messageBuffer));
    }
    else if (strcmp(topic, "leddisplay/delay") == 0)
    {
        setScrollDelay(atoi(messageBuffer));
    }

    // Free the allocated buffer
    delete[] messageBuffer;
}

void saveConfig()
{
    File configFile = LittleFS.open(configFilePath, "w");
    if (!configFile)
    {
        Serial.println("Failed to open config file for writing");
        setMessage("Config Save Fail");
        return;
    }

    JsonDocument doc;
    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_port"] = mqtt_port;
    doc["mqtt_user"] = mqtt_user;
    doc["mqtt_pass"] = mqtt_pass;

    if (serializeJson(doc, configFile) == 0)
    {
        Serial.println("Failed to write to config file");
        setMessage("Config Write Fail");
    }

    configFile.close();
}

void loadConfig()
{
    File configFile = LittleFS.open(configFilePath, "r");
    if (!configFile)
    {
        Serial.println("Failed to open config file");
        setMessage("Config Load Fail");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, configFile);
    if (error)
    {
        Serial.println("Failed to read config file, using default configuration");
        setMessage("Config Read Fail");
    }
    else
    {
        strlcpy(mqtt_server, doc["mqtt_server"], sizeof(mqtt_server));
        strlcpy(mqtt_port, doc["mqtt_port"], sizeof(mqtt_port));
        strlcpy(mqtt_user, doc["mqtt_user"], sizeof(mqtt_user));
        strlcpy(mqtt_pass, doc["mqtt_pass"], sizeof(mqtt_pass));

        // Debug prints to verify loaded values
        Serial.println("Config loaded:");
        Serial.print("MQTT Server: ");
        Serial.println(mqtt_server);
        Serial.print("MQTT Port: ");
        Serial.println(mqtt_port);
        Serial.print("MQTT User: ");
        Serial.println(mqtt_user);
        Serial.print("MQTT Pass: ");
        Serial.println(mqtt_pass);
    }

    configFile.close();
}

bool validateMQTTConfig()
{
    if (strlen(mqtt_server) == 0 || strlen(mqtt_port) == 0)
    {
        Serial.println("MQTT configuration is invalid.");
        return false;
    }
    return true;
}

void checkResetButton()
{
    static unsigned long buttonPressTime = 0;
    static bool buttonPressed = false;

    if (digitalRead(RESET_BUTTON_PIN) == LOW)
    {
        if (!buttonPressed)
        {
            buttonPressed = true;
            buttonPressTime = millis();
        }
        else if (millis() - buttonPressTime > RESET_HOLD_TIME)
        {
            // Reset configuration
            Serial.println("Reset button held for 5 seconds. Resetting configuration...");
            setMessage("Resetting Config");
            LittleFS.remove(configFilePath); // Remove the configuration file
            ESP.restart();                   // Restart the device
        }
    }
    else
    {
        buttonPressed = false;
    }
}

textEffect_t getEffectFromString(const String &effectStr)
{
    if (effectStr == "PA_PRINT")
        return PA_PRINT;
    if (effectStr == "PA_SCROLL_LEFT")
        return PA_SCROLL_LEFT;
    if (effectStr == "PA_SCROLL_RIGHT")
        return PA_SCROLL_RIGHT;
    if (effectStr == "PA_SCROLL_UP")
        return PA_SCROLL_UP;
    if (effectStr == "PA_SCROLL_DOWN")
        return PA_SCROLL_DOWN;
    if (effectStr == "PA_SLICE")
        return PA_SLICE;
    if (effectStr == "PA_MESH")
        return PA_MESH;
    if (effectStr == "PA_FADE")
        return PA_FADE;
    if (effectStr == "PA_WIPE")
        return PA_WIPE;
    if (effectStr == "PA_WIPE_CURSOR")
        return PA_WIPE_CURSOR;
    if (effectStr == "PA_BLINDS")
        return PA_BLINDS;
    if (effectStr == "PA_DISSOLVE")
        return PA_DISSOLVE;
    if (effectStr == "PA_RANDOM")
        return PA_RANDOM;
    if (effectStr == "PA_OPENING")
        return PA_OPENING;
    if (effectStr == "PA_OPENING_CURSOR")
        return PA_OPENING_CURSOR;
    if (effectStr == "PA_CLOSING")
        return PA_CLOSING;
    if (effectStr == "PA_CLOSING_CURSOR")
        return PA_CLOSING_CURSOR;
    if (effectStr == "PA_SCROLL_UP_LEFT")
        return PA_SCROLL_UP_LEFT;
    if (effectStr == "PA_SCROLL_UP_RIGHT")
        return PA_SCROLL_UP_RIGHT;
    if (effectStr == "PA_SCROLL_DOWN_LEFT")
        return PA_SCROLL_DOWN_LEFT;
    if (effectStr == "PA_SCROLL_DOWN_RIGHT")
        return PA_SCROLL_DOWN_RIGHT;
    if (effectStr == "PA_SCAN_HORIZ")
        return PA_SCAN_HORIZ;
    if (effectStr == "PA_SCAN_VERT")
        return PA_SCAN_VERT;
    if (effectStr == "PA_GROW_UP")
        return PA_GROW_UP;
    if (effectStr == "PA_GROW_DOWN")
        return PA_GROW_DOWN;
    return PA_SCROLL_LEFT; // Default effect
}
