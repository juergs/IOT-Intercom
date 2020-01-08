/*
 * TODO
 * make code work
 * watchdog https://techtutorialsx.com/2017/01/21/esp8266-watchdog-functions/
 *  ESP.wdtEnable(1000); / ESP.wdtFeed();
 * wifimanager
 * mqtt
  */

//Libs
#include <Arduino.h>
#include <RotaryDialer.h> // https://github.com/markfickett/Rotary-Dial
#include <CircularBuffer.h> // https://github.com/rlogiacco/CircularBuffer
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <MQTT.h>
#include <IotWebConf.h>

#define DEBUG
#define SERIAL_COMMANDS


#ifdef DEBUG
 #define DEBUG_PRINT(x)     Serial.print (x)
 #define DEBUG_PRINTDEC(x)     Serial.print (x, DEC)
 #define DEBUG_PRINTLN(x)  Serial.println (x)
#else
 #define DEBUG_PRINT(x)
 #define DEBUG_PRINTDEC(x)
 #define DEBUG_PRINTLN(x)
#endif


/* pins
Not all pins can be used as input. Check
https://randomnerdtutorials.com/esp8266-pinout-reference-gpios/
*/
// Output PINS
#define HB_INA D3 // H-Bridge INA
#define HB_INB D4 // H-Bridge INB
#define REED_RELAY_PIN D5 //

// Input pins
#define OCTO_COUPLER_PIN D6 // Input to detect when someone ringes the doorbel
#define BUZZER_BUTTON_PIN D7 // Button to press to open the door
#define ROTARY_PULSE_PIN D1 // Input for the rotary dailer pulse pin (normally closed
#define ROTARY_READY_PIN D2 // Input pin for rotary dailer ready pin (normally open)


#define BAUDRATE 115200
#define RINGER_PERIOD 25000 //in us (microsecond) blink once a second = 25000us = 25ms
#define DOOR_UNLOCK_DURATION 150

#define INT_CHECK_INTERVAL_MS 20
#define INT_MIN_STABLE_VALS 6

void silenceBell();
void handler_ringer(void);
void buzzerButtonInterrupt();
void doorBelInterrupt();
void handleRoot();
void wifiConnected();
void configSaved();
boolean formValidator();
boolean connectMqtt();
void mqttMessageReceived(String &topic, String &payload);


unsigned long prevIntCheckMillis;
char buzzerButtonStableVals, octoPinStableVals;


// Classes and things
RotaryDialer dialer = RotaryDialer(ROTARY_READY_PIN, ROTARY_PULSE_PIN);
CircularBuffer<int, 10> serOutBuff;
CircularBuffer<int, 10> commandBuff;
Ticker bellTimer;

enum programStates {
  STATE_INIT, // Initialise
  STATE_IDLE, // Idle
  STATE_ROTARY_INPUT,
  STATE_RING_BELL,
  STATE_UNLOCK_DOOR,
  //STATE_DOORBELL_PRESSED,
  STATE_SERIAL_INPUT,
  STATE_SERIAL_OUTPUT,
  STATE_HANDLE_ACTION
};

programStates newState = STATE_INIT;
programStates prevState = STATE_IDLE;

struct bellConfig {
  unsigned int onTimeMs;
  unsigned int offTimeMs;
  unsigned int repeats;
};

bellConfig normalBellRing = {1000, 500, 3};

byte toggle = 0;
byte intToggle = 1;

unsigned long stateTime = 0;
unsigned long prevStateTime = 0;

boolean doorBellIntFlag = false;
boolean autoUnlockFlag = false;

boolean autoUnlockEnabled = false;
boolean buzzerPressedFlag = false;
//boolean autoOpenEnabled = false;
boolean muteBellEnabled = false;

unsigned int dialerInput = 0;
unsigned int bellRepeats = 0;
boolean bellOn = false;

//Put ISR's in IRAM.
void ICACHE_RAM_ATTR buzzerButtonInterrupt();
void ICACHE_RAM_ATTR doorBelInterrupt();


// Wifi stuff
const char thingName[] = "doorbell";
const char wifiInitialApPassword[] = "10doorbell10!";
#define STRING_LEN 128
#define CONFIG_VERSION "mqttdoorbell v3.1ESP"
#define STATUS_PIN LED_BUILTIN

#define MQTT_TOPIC_PREFIX "/devices/"
#define ACTION_FEQ_LIMIT 7000
#define NO_ACTION -1

// -- Callback method declarations.
void wifiConnected();
void configSaved();
boolean formValidator();
void mqttMessageReceived(String &topic, String &payload);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;
MQTTClient mqttClient;

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];


IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPasswordValue, STRING_LEN, "password");
boolean needMqttConnect = false;
boolean needReset = false;
unsigned long lastMqttConnectionAttempt = 0;
int needAction = NO_ACTION;
int state = LOW;
unsigned long lastAction = 0;


char mqttActionTopic[STRING_LEN];
char mqttStatusTopic[STRING_LEN];


void setup(void) {
  #ifdef DEBUG
    Serial.begin(BAUDRATE);
  #endif
  // PINS
  pinMode(HB_INA, OUTPUT);
  pinMode(HB_INB, OUTPUT);
  pinMode(REED_RELAY_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(OCTO_COUPLER_PIN, INPUT_PULLUP); // When low input
  pinMode(BUZZER_BUTTON_PIN, INPUT_PULLUP); // When low input
  attachInterrupt(digitalPinToInterrupt(OCTO_COUPLER_PIN), doorBelInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUZZER_BUTTON_PIN), buzzerButtonInterrupt, RISING);
  // Webserver
  //iotWebConf.setStatusPin(STATUS_PIN);
  //iotWebConf.setConfigPin(BUTTON_PIN);
  iotWebConf.addParameter(&mqttServerParam);
  iotWebConf.addParameter(&mqttUserNameParam);
  iotWebConf.addParameter(&mqttUserPasswordParam);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.setupUpdateServer(&httpUpdater);

  // -- Initializing the configuration.
  boolean validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  // -- Prepare dynamic topic names
  String temp = String(MQTT_TOPIC_PREFIX);
  temp += iotWebConf.getThingName();
  temp += "/action";
  temp.toCharArray(mqttActionTopic, STRING_LEN);
  temp = String(MQTT_TOPIC_PREFIX);
  temp += iotWebConf.getThingName();
  temp += "/status";
  temp.toCharArray(mqttStatusTopic, STRING_LEN);

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);

  DEBUG_PRINTLN("Setup finished");
}

void loop(void) {
  stateTime = millis();
  if (newState != prevState) {
    DEBUG_PRINT(prevState);
    DEBUG_PRINT(" to: ");
    DEBUG_PRINTLN(newState);
  }
    switch(newState) { // Initialisation
      // STATE 0 - Init
      case STATE_INIT: // Basically the same as setup.
        serOutBuff.clear();
        commandBuff.clear();
        // Configure timer
        // https://circuits4you.com/2018/01/02/esp8266-timer-ticker-example/
        bellTimer.attach_ms(50, handler_ringer); // 20hz = 50ms cycle, 25hz = 40ms
        silenceBell();
        dialer.setup();
        prevState = STATE_INIT;
        newState = STATE_IDLE;
      break;

      // STATE 1 - Idle
      case STATE_IDLE:
        //iwdg_feed(); // reset watchdog
        iotWebConf.doLoop();
        mqttClient.loop();
        if (needMqttConnect) {
          if (connectMqtt()) {
            needMqttConnect = false;
          }
        }
        else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected())) {
          DEBUG_PRINTLN("MQTT reconnect");
          connectMqtt();
        }

        if (needReset) {
          DEBUG_PRINTLN("Rebooting after 1 second.");
          iotWebConf.delay(1000);
          ESP.restart();
        }

        if (dialer.update()) {
          newState = STATE_ROTARY_INPUT;
        }

        if (!commandBuff.isEmpty()) { // We have actions to do
          newState = STATE_HANDLE_ACTION;
        }

        if (!serOutBuff.isEmpty()) { // We have stuff ready for transmission
          newState = STATE_SERIAL_OUTPUT;
        }

        #ifdef DEBUG
          if (Serial.available() > 0) {
            newState = STATE_SERIAL_INPUT;
          }
        #endif

        // Debouncing of interrupts
        if ((stateTime - prevIntCheckMillis) > INT_CHECK_INTERVAL_MS) {
          prevIntCheckMillis = stateTime;
          // Buzzerbutton debounde
          if ((digitalRead(BUZZER_BUTTON_PIN) == HIGH) && (buzzerPressedFlag == true))
          {
            buzzerButtonStableVals++;
            if (buzzerButtonStableVals >= INT_MIN_STABLE_VALS)
            {
              buzzerPressedFlag = false;
              serOutBuff.push(10);
              newState = STATE_UNLOCK_DOOR;
              buzzerButtonStableVals = 0;
              DEBUG_PRINTLN("Interrupt triggered unlock");
            }
          }
          else {
            buzzerButtonStableVals = 0;
          }
          // Octocoupler
          if ((digitalRead(OCTO_COUPLER_PIN) == LOW) && (doorBellIntFlag == true))
          {
            octoPinStableVals++;
            if (octoPinStableVals >= INT_MIN_STABLE_VALS)
            {
              doorBellIntFlag = false;
              serOutBuff.push(20);
              newState = STATE_RING_BELL;
              octoPinStableVals = 0;
            }
          }
          else {
            octoPinStableVals = 0;
          }
        }

        prevState = STATE_IDLE;
      break;

      //State 2 - Rotary input
      case STATE_ROTARY_INPUT:
        prevState = STATE_ROTARY_INPUT;
        newState = STATE_IDLE;
        dialerInput = dialer.getNextNumber();

        switch(dialerInput){
          case 1:
            commandBuff.push(1);
          break;
          case 6:
            commandBuff.push(6);
          break;
          case 7:
            commandBuff.push(7);
          break;
          case 8: // Toggle do not disturb
            commandBuff.push(8);
          break;
          case 9: // Toggle auto open mode
            commandBuff.push(9);
          break;
          default:
            serOutBuff.push(dialerInput);
          break;
        }
      break;

      //State 3 - Unlock door
      case STATE_UNLOCK_DOOR: // Activate relay to open door
        if (prevState != STATE_UNLOCK_DOOR) {
          DEBUG_PRINTLN("Relay ON");
          silenceBell();
          prevStateTime = stateTime;
          digitalWrite(REED_RELAY_PIN, HIGH);
        }
        if((stateTime - prevStateTime) >= DOOR_UNLOCK_DURATION) {
          DEBUG_PRINTLN("Relay OFF");
          digitalWrite(REED_RELAY_PIN, LOW);
          newState = STATE_IDLE;
        }
        prevState = STATE_UNLOCK_DOOR;
      break;

      //State 4 - Ring bell
      case STATE_RING_BELL:
        if(muteBellEnabled == true && autoUnlockEnabled == false) { // Dont ring the bell when we are on mute. But do if auto open is enabled
          newState = STATE_IDLE;
          prevState = STATE_RING_BELL;
          break;
        }
        if(prevState!=STATE_RING_BELL) {
          DEBUG_PRINTLN("Bell on INIT");
          bellOn = true;
          //Timer2.resume();
          bellTimer.attach_ms(50, handler_ringer);
          bellRepeats = 1;
          prevStateTime = stateTime;
        }
        if (bellOn == true) {
          if((stateTime - prevStateTime) > normalBellRing.onTimeMs) {
            DEBUG_PRINT("Bell off: ");
            DEBUG_PRINTLN(bellRepeats);
            silenceBell();
            prevStateTime = stateTime;
            bellOn = false;
            prevStateTime = stateTime;
            if (bellRepeats >= normalBellRing.repeats) {
                if (autoUnlockFlag == true) {
                  autoUnlockFlag = false;
                  newState = STATE_UNLOCK_DOOR;
                  DEBUG_PRINTLN("Auto unlock from RING_BELL_STATE triggered unlock");
                }
                else {
                  newState = STATE_IDLE;
                }
            }
          }
        }
        else if((stateTime  - prevStateTime) > normalBellRing.offTimeMs) {
            DEBUG_PRINT("Bell on: ");
            DEBUG_PRINTLN(bellRepeats);
            bellRepeats++;
            //Timer2.resume();
            bellTimer.attach_ms(50, handler_ringer);
            prevStateTime = stateTime;
            bellOn = true;
          }
        prevState = STATE_RING_BELL;
      break;

      // State 5 - Serial IN
      case STATE_SERIAL_INPUT:

        int input;
        input = Serial.parseInt();
        if (input > 0) {
          commandBuff.push(input);
          DEBUG_PRINT("Serial input: ");
          DEBUG_PRINTLN(input);
        }
        prevState = STATE_SERIAL_INPUT;
        newState = STATE_IDLE;
      break;

      // State 6 - Serial OUT
      case STATE_SERIAL_OUTPUT:
        //int intOut;
        //intOut = serOutBuff.shift();
        newState = STATE_IDLE;
        prevState = STATE_SERIAL_OUTPUT;
      break;

      // State 7 - Handle actio
      case STATE_HANDLE_ACTION:
        prevState = STATE_HANDLE_ACTION;
        newState = STATE_IDLE; // Don get trapped in here
        unsigned int iCommand, iTopic, iAction, iOutcome;
        String strAction = "";
        String strTopic = "";
        String strOutcome = "";
        iCommand = commandBuff.shift();
        if(iCommand<10 || iCommand>99) {break;} // We are expecting two digits
        iTopic = (iCommand/10) % 10;
        iAction = (iCommand % 10);
        switch(iAction) {
          case 0:
            strAction = "OFF";
            break;
          case 1:
            strAction = "ON";
            break;
          case 2:
            strAction = "TOGGLE";
            break;
          default:
            strAction = "STATUS";
            iAction = 9;
            break;
      }

        switch(iTopic) {
          case 5: // Unlock door
            strTopic = "unlock";
            if (iAction == 1) {newState = STATE_UNLOCK_DOOR;}
            if (iAction == 2) {newState = STATE_UNLOCK_DOOR;}
            iOutcome = 2;
            strOutcome = 2;
            break;

          case 7: // Toggle do not disturb
            strTopic = "auto_unlock";
            if (iAction < 2) {muteBellEnabled = iAction;}
            if (iAction == 2) {muteBellEnabled = !muteBellEnabled;}
            iOutcome = muteBellEnabled;
            if (iOutcome == 0) {strOutcome = "OFF";}
            else if(iOutcome == 1) {strOutcome = "ON";}
            break;
        }
        /*
        switch(command) {
          case 1: // Unlock the door
            DEBUG_PRINTLN("Action triggered unlock");
            newState = STATE_UNLOCK_DOOR;
            serOutBuff.push(command);
          break;

          case 6: // Ring the bell
            newState = STATE_RING_BELL;
            serOutBuff.push(command);
          break;

          case 7: // Toggle mute
            if(muteBellEnabled==true) {
              commandBuff.push(70);
            }
            else {
              commandBuff.push(71);
            }
          break;

          case 70: // Disable mute
            muteBellEnabled = false;
            serOutBuff.push(70);
            commandBuff.push(6);
          break;

          case 71: // Enable mute
            muteBellEnabled = true;
            serOutBuff.push(71);
          break;

          case 72: // Status mute
            if(muteBellEnabled==true) {
              serOutBuff.push(70);
            }
            else {
              serOutBuff.push(71);
            }
            break;


          case 8: // Toggle auto unlock
            if(autoUnlockEnabled==true) {
              commandBuff.push(80);
            }
            else {
              commandBuff.push(81);
            }
          break;

          case 80: // Disable auto unlock
            autoUnlockEnabled = false;
            serOutBuff.push(80);
            commandBuff.push(6);
          break;

          case 81: // Enable auto unlock
            autoUnlockEnabled = true;
            serOutBuff.push(81);
          break;

          case 82: // Status auto unlock
            if(autoUnlockEnabled==true) {
              serOutBuff.push(80);
            }
            else {
              serOutBuff.push(81);
            }
          break;


          case 9:
            newState = STATE_INIT;
          break;

          default:
            newState = STATE_IDLE;
          break;
        */

        DEBUG_PRINT("Command put in serout buffer: ");
        DEBUG_PRINTLN(iCommand);
        DEBUG_PRINTLN(strTopic);
        DEBUG_PRINTLN(strAction);
        DEBUG_PRINTLN(muteBellEnabled);
      break;
    }
}

void silenceBell() {
  bellTimer.detach();
  //Timer2.pause();
  digitalWrite(HB_INA, LOW);
  digitalWrite(HB_INB, LOW);
}

void handler_ringer(void) {
    toggle ^= 1;
    digitalWrite(LED_BUILTIN, toggle);
    digitalWrite(HB_INA, toggle);
    digitalWrite(HB_INB, !toggle);
}
// Interrupt routines
void buzzerButtonInterrupt() { // Interrupt when button to open door is pressed
  buzzerPressedFlag = true;
}


void doorBelInterrupt() { // Interrupt when doorbel is pressed
    doorBellIntFlag = true;
    if (autoUnlockEnabled == true) {
      autoUnlockFlag = true;
    }
}

// Wifi and MQTT
/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = F("<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>");
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>IotWebConf 07 MQTT Relay</title></head><body>";
  s += iotWebConf.getThingName();
  s += "<div>State: ";
  s += (state == HIGH ? "ON" : "OFF");
  s += "</div>";
  s += "<button type='button' onclick=\"location.href='';\" >Refresh</button>";
  s += "<div>Go to <a href='config'>configure page</a> to change values.</div>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void wifiConnected()
{
  needMqttConnect = true;
}

void configSaved()
{
  DEBUG_PRINTLN("Configuration was updated.");
  needReset = true;
}

boolean formValidator()
{
  DEBUG_PRINTLN("Validating form.");
  boolean valid = true;

  int l = server.arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }

  return valid;
}


boolean connectMqtt() {
  unsigned long now = millis();
  if (1000 > now - lastMqttConnectionAttempt)
  {
    // Do not repeat within 1 sec.
    return false;
  }
  DEBUG_PRINTLN("Connecting to MQTT server...");
  if (!mqttClient.connect(iotWebConf.getThingName())) {
    lastMqttConnectionAttempt = now;
    return false;
  }
  DEBUG_PRINTLN("Connected!");

  mqttClient.subscribe(mqttActionTopic);
  mqttClient.publish(mqttStatusTopic, state == HIGH ? "ON" : "OFF", true, 1);
  mqttClient.publish(mqttActionTopic, state == HIGH ? "ON" : "OFF", true, 1);

  return true;
}

void mqttMessageReceived(String &topic, String &payload)
{
  DEBUG_PRINTLN("Incoming: " + topic + " - " + payload);

  if (topic.endsWith("action"))
  {
    needAction = payload.equals("ON") ? HIGH : LOW;
    if (needAction == state)
    {
      needAction = NO_ACTION;
    }
  }
}