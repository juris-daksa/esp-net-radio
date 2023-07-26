
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include "SPIFFS.h"
#include "Audio.h"
#include "CSV_Parser.h"
#include "AiEsp32RotaryEncoder.h"
#include <U8g2lib.h>
#include <Wire.h>

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Search for parameter in HTTP POST request
const char* PARAM_INPUT_1 = "ssid";
const char* PARAM_INPUT_2 = "pass"; 
const char* PARAM_INPUT_3 = "ip";

// Acces Point name
const char* apName = "WEBRADIO-SETUP";

//Variables to save values from HTML form
String ssid = "";
String pass = "";
String ip = "";

// File paths to save input values permanently
const char* ssidPath = "/ssid.txt";
const char* passPath = "/pass.txt";
const char* ipPath = "/ip.txt";
const char* stationsPath = "/stations.csv";

IPAddress localIP;
//IPAddress localIP(192, 168, 1, 200); // hardcoded
IPAddress gateway;
IPAddress subnet;

// Timer variables
unsigned long previousMillis = 0;
const long interval = 10000;  // interval to wait for Wi-Fi connection (milliseconds)

// I2S pinout & audio object for I2S communication
#define I2S_BCLK      13
#define I2S_DOUT      12
#define I2S_LRC       14

Audio audio;
const int defaultVolume = 17;  //should be moved to config file
int currentVolume = defaultVolume;

// CSV parser for station reading
CSV_Parser csv_parse("-ss");

char** station_URLs;
char** station_names;
const int defaultStation = 1; //should be moved to config file
int currentStation = defaultStation;
int stationCount = 0;
bool isStationsLoaded = false;

// Command from Serial
char cmd[130];

// Rotary encoder
#define ROTARY_ENCODER_A_PIN 19
#define ROTARY_ENCODER_B_PIN 18
#define ROTARY_ENCODER_BUTTON_PIN 23
#define ROTARY_ENCODER_STEPS 4

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);

// Display
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

u8g2_uint_t offset;			// current offset for the scrolling text
u8g2_uint_t width;			// pixel width of the scrolling text (must be lesser than 128 unless U8G2_16BIT is defined
String text = "ok, radio ";	  // scroll this text from right to left
unsigned long screenUpdate = 0;

// Audio library functions
void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
void audio_id3data(const char *info){  //id3 metadata
    Serial.print("id3data     ");Serial.println(info);
}
void audio_eof_mp3(const char *info){  //end of file
    Serial.print("eof_mp3     ");Serial.println(info);
}
void audio_showstation(const char *info){
    Serial.print("station     ");Serial.println(info);
}
void audio_showstreamtitle(const char *info){
    Serial.print("streamtitle ");
    Serial.println(info);
    text = info;
}
void audio_bitrate(const char *info){
    Serial.print("bitrate     ");Serial.println(info);
}
void audio_commercial(const char *info){  //duration in sec
    Serial.print("commercial  ");Serial.println(info);
}
void audio_icyurl(const char *info){  //homepage
    Serial.print("icyurl      ");Serial.println(info);
}
void audio_lasthost(const char *info){  //stream URL played
    Serial.print("lasthost    ");Serial.println(info);
}
void audio_eof_speech(const char *info){
    Serial.print("eof_speech  ");Serial.println(info);
}

// Initialize SPIFFS
void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("An error has occurred while mounting SPIFFS");
  }

  Serial.println("SPIFFS mounted successfully");
}

// Read File from SPIFFS
String readFile(fs::FS &fs, const char * path){
  Serial.printf("Reading file: %s\r\n", path);

  File file = fs.open(path);
  if(!file || file.isDirectory()){
    Serial.println("- failed to open file for reading");
    return String();
  }
  
  String fileContent;
  while(file.available()){
    fileContent = file.readStringUntil('\n');
    break;     
  }

  file.close();

  return fileContent;
}

// Write file to SPIFFS
void writeFile(fs::FS &fs, const char * path, const char * message){
  Serial.printf("Writing file: %s\r\n", path);

  File file = fs.open(path, FILE_WRITE);
  if(!file){
    Serial.println("- failed to open file for writing");
    return;
  }

  if(file.print(message)){
    Serial.println("- file written");
  } else {
    Serial.println("- filewrite failed");
  }

  file.close();
}

// Load stations from CSV file
bool loadStationsFromCSV(fs::FS &fs, const char* path){
  Serial.printf("Reading file: %s\r\n", path);
  File file = fs.open(path);
  if(!file || file.isDirectory()){
    Serial.println("- failed to open file for reading");
    return false;
  }

  while (file.available()) {
    csv_parse << file.readStringUntil('\n').c_str();
  }

  file.close();
  station_URLs = (char**)csv_parse["STATION_URL"];
  station_names = (char**)csv_parse["STATION_NAME"];
  stationCount = csv_parse.getRowsCount() - 1;
  Serial.printf("Found %d stations: \r\n",stationCount);
  for(int i = 1; i < csv_parse.getRowsCount(); i++){
    char buff[64] = {0};
    snprintf(buff,64, "%3d %10s %10s",i,station_names[i],station_URLs[i]);
    Serial.println(buff);
  }

  Serial.println();

  return true;
}

// Initialize WiFi
bool initWiFi() {
  if((ssid != NULL) && (ssid[0] == '\0')){
    Serial.println("Undefined SSID.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  if((ip != NULL) && (ip[0] != '\0')){
    localIP.fromString(ip);
    if (!WiFi.config(localIP, gateway, subnet)){
      Serial.println("STA Failed to configure");
      return false;
    }
  }

  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("Connecting to WiFi...");

  unsigned long currentMillis = millis();
  previousMillis = currentMillis;

  while(WiFi.status() != WL_CONNECTED) {
    currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      Serial.println("Failed to connect.");
      return false;
    }
  }

  Serial.println(WiFi.localIP());

  return true;
}

void webserverSetup_AP (){
  // Connect to Wi-Fi network with SSID and password
  Serial.println("Setting AP (Access Point)");
  // NULL sets an open Access Point
  WiFi.softAP(apName, NULL);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP); 

  // Web Server Root URL
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/wifimanager.html", "text/html");
  });
  
  server.serveStatic("/", SPIFFS, "/");
  
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
    int params = request->params();
    for(int i=0;i<params;i++){
      AsyncWebParameter* p = request->getParam(i);
      if(p->isPost()){
        // HTTP POST ssid value
        if (p->name() == PARAM_INPUT_1) {
          ssid = (char*)p->value().c_str();
          Serial.print("SSID set to: ");
          Serial.println(ssid);
          // Write file to save value
          writeFile(SPIFFS, ssidPath, ssid.c_str());
        }

        // HTTP POST pass value
        if (p->name() == PARAM_INPUT_2) {
          pass = (char*)p->value().c_str();
          Serial.print("Password set to: ");
          Serial.println(pass);
          // Write file to save value
          writeFile(SPIFFS, passPath, pass.c_str());
        }

        // HTTP POST ip value
        if (p->name() == PARAM_INPUT_3) {
          ip = (char*)p->value().c_str();
          Serial.print("IP Address set to: ");
          Serial.println(ip);
          // Write file to save value
          writeFile(SPIFFS, ipPath, ip.c_str());
        }
      }
    }

    String ip_addr = ip;
    request->send(200, "text/plain", "Done. ESP will restart, connect to your router and go to IP address: " + ip_addr);
    delay(3000);
    ESP.restart();
  });
  
  server.begin();
}

void proccessInput(String &str){
  int inx;
  if((inx = str.indexOf("#")) >= 0){
    str.remove(inx);
  }

  str.trim();
}

const char* proccessVolume(int step){
  if (step < 0 && currentVolume > 0){
    audio.setVolume(currentVolume - 1);
  }

  if (step > 0 && currentVolume < 21){
    audio.setVolume(currentVolume + 1);
  }

  const char* reply = "Volume is now " + currentVolume;

  return reply;
}

const char* proccessCommand(const char* commandArgument, const char* commandValue){
  String argument;
  String value;
  int int_value;
  static char reply[180];
  int oldVolume;
  String tmpValue;
  
  strcpy(reply,"Command accepted");
  argument = String(commandArgument);
  proccessInput(argument);
  if(argument.length() == 0){
    return reply;
  }

  argument.toLowerCase();
  
  value = String(commandValue);
  proccessInput(value);
  int_value = value.toInt();
  int_value = abs(int_value);
  if (value.length()){
    tmpValue = value;
    Serial.println("Command \"" + argument + "\" with value \"" + value + "\"");
  } else {
    Serial.println("Command \"" + argument + "\" without value");
  }

  if (argument.indexOf("volume") == 0){
    if (value.indexOf("up") == 0){
      return proccessVolume(1);
    }
    
    if (value.indexOf("down") == 0){
      return proccessVolume(-1);
    }

    if (int_value >= 0 && int_value <= 21){
      audio.setVolume(int_value);
      sprintf(reply, "Volume is now %d", audio.getVolume());
      
      return reply;
    }
  }

  if (argument.indexOf("preset") == 0){
    int oldStation = currentStation;
    if (value.indexOf("up") == 0){
      currentStation = (oldStation < stationCount ? oldStation + 1 : 1);
    } else if (value.indexOf("down") == 0){
      currentStation = (oldStation == 1 ? stationCount : oldStation - 1);
    } else if (int_value >= 1 && int_value <= stationCount){
      currentStation = int_value;
    }

    audio.connecttohost(station_URLs[currentStation]);
    sprintf(reply, "Preset set to %d. %s", currentStation, station_names[currentStation]);
    return reply;
  }

  return reply;
}

const char* proccessCommand(const char* str){
  char* value;
  const char* result;

  value = strstr(str, "=");
  if (value){
    *value = '\0';
    result = proccessCommand(str, value + 1);
    *value = '=';
  }else{
    result = proccessCommand(str, "0");
  }

  return result;
}

void scanSerial(){
  static String serialCmd;
  char c;
  const char* reply = "";
  uint16_t length;

  while(Serial.available()){
    c = (char)Serial.read();
    length = serialCmd.length();
    if((c == '\n') || (c == '\r')){
      if (length){
        strncpy(cmd, serialCmd.c_str(), sizeof(cmd));
        reply = proccessCommand(cmd);
        Serial.println(reply);
        serialCmd = "";
      }
    }

    if (c >= ' '){
      serialCmd += c;
    }

    if (length >= (sizeof(cmd) - 2)){
      serialCmd = "";
    }
  }
}

void rotary_onButtonClick()
{
  static unsigned long lastTimePressed = 0;
  if (millis() - lastTimePressed < 200){
    return;
  }

  lastTimePressed = millis();
  int oldStation = currentStation;
  currentStation = (oldStation < stationCount ? oldStation + 1 : 1);
  audio.connecttohost(station_URLs[currentStation]);
}

void IRAM_ATTR readEncoderISR()
{
    rotaryEncoder.readEncoder_ISR();
}

void setup() {
  Serial.begin(9600);

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(defaultVolume); // 0...21

  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 21, false); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)
  rotaryEncoder.setAcceleration(50);
  rotaryEncoder.setEncoderValue(defaultVolume);

  u8g2.begin();
  u8g2.setFont(u8g2_font_profont12_tr );		// draw the current pixel width
  u8g2.setFontMode(0);		// enable transparent mode, which is faster



  initSPIFFS();   
  ssid = readFile(SPIFFS, ssidPath);
  pass = readFile(SPIFFS, passPath);
  ip = readFile(SPIFFS, ipPath);
  isStationsLoaded = loadStationsFromCSV(SPIFFS, stationsPath);

  if (initWiFi()){
    audio.connecttohost(isStationsLoaded? station_URLs[defaultStation]:"live.pieci.lv/live19-hq.mp3");
  }else{
    webserverSetup_AP();
  }
  
  Serial.println("\n--- END OF SETUP ---\n");
}

void loop() {
  audio.loop();
  scanSerial();

  if (rotaryEncoder.encoderChanged())
  {
    currentVolume = rotaryEncoder.readEncoder();
    audio.setVolume(currentVolume);
  }
  
  if (rotaryEncoder.isEncoderButtonClicked())
  {
    rotary_onButtonClick();
  } 

  if (screenUpdate < millis()){
    u8g2_uint_t x;
    u8g2.setFont(u8g2_font_inb16_mr);		// set the target font
    const char *char_text = text.c_str();
    width = u8g2.getUTF8Width(char_text);
    u8g2.firstPage();
    do {
      // draw the scrolling text at current offset
      x = offset;
      u8g2.setFont(u8g2_font_inb16_mr);		// set the target font
      do {								// repeated drawing of the scrolling text...
        u8g2.drawUTF8(x, 30, char_text);			// draw the scolling text
        x += width +15;						// add the pixel width of the scrolling text
      } while( x < u8g2.getDisplayWidth() );		// draw again until the complete display is filled


    u8g2.setFont(u8g2_font_profont12_tr );		// draw the current pixel width
    u8g2.setCursor(0, 56);
    u8g2.print("ST>");
    if (currentStation < 10) u8g2.print(0);
    u8g2.print(currentStation);
    u8g2.setCursor(50, 56);
    u8g2.print("VOL>");
    if (currentVolume < 10){
      u8g2.print(0);
    }
    u8g2.print(currentVolume);
    u8g2.setCursor(0, 44);
    u8g2.print("                      ");
    u8g2.setCursor(0, 44);
    u8g2.print(station_names[currentStation]);					// this value must be lesser than 128 unless U8G2_16BIT is set

    } while ( u8g2.nextPage() );
    
    offset-= 6;							// scroll by one pixel
    if ( (u8g2_uint_t)offset < (u8g2_uint_t)-width )	
      offset = 0;							// start over again
    screenUpdate = millis() + 200;
  }

}

