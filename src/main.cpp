#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <SPIFFS.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>
#include "time.h"

// ---------------- CONFIG ----------------
const char* WIFI_SSID = "Redmi5G";
const char* WIFI_PASSWORD = "12345678";

const int RELAY_PINS[4] = {16,17,18,19};
#define RELAY_ON HIGH
#define RELAY_OFF LOW

// 4 INA219 sensors
Adafruit_INA219 ina1(0x40);
Adafruit_INA219 ina2(0x41);
Adafruit_INA219 ina3(0x44);
Adafruit_INA219 ina4(0x45);
Adafruit_INA219* INA[4] = {&ina1, &ina2, &ina3, &ina4};
bool inaPresent[4] = {false,false,false,false};

// Web
WebServer server(80);
WebSocketsServer webSocket(81);

// SPIFFS files
const char* SETTINGS_FILE = "/settings.json";
const char* LOGS_FILE     = "/logs.json";
const char* NOTIFS_FILE   = "/notifs.json";

// Time config
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // UTC+5:30
const int daylightOffset_sec = 0;

// ---------------- Struct ----------------
struct Load {
  float V=0, I=0, P=0;
  double Wh=0;
  double cost=0;
  bool relay=false;
  unsigned long onSecondsToday=0;
  unsigned long usageLimitSeconds=12UL*3600;
  int timerMinutes=0;
  unsigned long timerEndEpoch=0;
};
Load L[4];

double unitPrice = 8.0;
unsigned long lastSec = 0;

// ---------------- Forward decl ----------------
void broadcastState();
void saveSettingsToFS();
void saveLogsToFS();
void pushNotification(const String &s);
void loadSettingsFromFS();
void loadLogsFromFS();
void loadNotifsFromFS();
void addLogEntry(const String &period,const String &key,const String &payload);
bool fileExists(const char* path){ return SPIFFS.exists(path); }

// ---------------- WiFi ----------------
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<20000){
    delay(300); Serial.print(".");
  }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
  } else Serial.println("WiFi connection failed!");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// ---------------- SPIFFS ----------------
void initSPIFFS(){
  if(!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed!");
  else Serial.println("SPIFFS mounted.");
}

// ---------------- Settings ----------------
void saveSettingsToFS(){
  StaticJsonDocument<512> doc;
  doc["unitPrice"] = unitPrice;
  JsonArray loads = doc.createNestedArray("loads");
  for(int i=0;i<4;i++){
    JsonObject o = loads.createNestedObject();
    o["limitSec"] = L[i].usageLimitSeconds;
    o["timerMin"] = L[i].timerMinutes;
  }
  File f = SPIFFS.open(SETTINGS_FILE, FILE_WRITE);
  if(f){ serializeJson(doc,f); f.close(); }
  else Serial.println("Failed to open settings file for writing");
}

void loadSettingsFromFS(){
  if(!fileExists(SETTINGS_FILE)){ saveSettingsToFS(); return; }
  File f = SPIFFS.open(SETTINGS_FILE, FILE_READ);
  if(!f){ Serial.println("Failed to open settings"); return; }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc,f);
  f.close();
  if(err){ Serial.println("Settings JSON parse fail"); return; }
  if(doc.containsKey("unitPrice")) unitPrice = doc["unitPrice"].as<double>();
  if(doc.containsKey("loads")){
    JsonArray arr = doc["loads"].as<JsonArray>();
    for(int i=0;i<4 && i<(int)arr.size();i++){
      if(arr[i].containsKey("limitSec")) L[i].usageLimitSeconds = arr[i]["limitSec"].as<unsigned long>();
      if(arr[i].containsKey("timerMin")) L[i].timerMinutes = arr[i]["timerMin"].as<int>();
    }
  }
}

// ---------------- Notifications ----------------
void pushNotification(const String &s){
  Serial.println("NOTIF: "+s);
  StaticJsonDocument<1024> doc;
  if(fileExists(NOTIFS_FILE)){
    File f = SPIFFS.open(NOTIFS_FILE, FILE_READ);
    if(f){
      deserializeJson(doc,f);
      f.close();
    }
  }
  if(!doc.containsKey("notifs")) doc.createNestedArray("notifs");
  JsonArray arr = doc["notifs"].as<JsonArray>();
  JsonObject o = arr.createNestedObject();
  o["ts"] = time(nullptr);
  o["text"] = s;
  File fw = SPIFFS.open(NOTIFS_FILE, FILE_WRITE);
  if(fw){ serializeJson(doc,fw); fw.close(); }
  DynamicJsonDocument out(256);
  out["type"] = "notification"; out["text"] = s;
  String outS; serializeJson(out,outS);
  webSocket.broadcastTXT(outS);
}

// ---------------- WebSocket ----------------
void handleWS(uint8_t num, WStype_t type, uint8_t * payload, size_t length){
  if(type != WStype_TEXT) return;
  String msg = String((char*)payload);
  StaticJsonDocument<512> doc;
  if(deserializeJson(doc,msg)){ Serial.println("WS JSON parse error"); return; }
  const char* cmd = doc["cmd"];
  if(!cmd) return;

  if(strcmp(cmd,"relay")==0){
    int id = doc["id"] | 1; bool state = doc["state"] | false;
    if(id>=1 && id<=4){
      digitalWrite(RELAY_PINS[id-1], state ? RELAY_ON : RELAY_OFF);
      L[id-1].relay = state;
      if(state && L[id-1].timerMinutes>0) L[id-1].timerEndEpoch = time(nullptr)+L[id-1].timerMinutes*60;
      else L[id-1].timerEndEpoch=0;
      pushNotification("Relay "+String(id)+(state?" ON":" OFF"));
    }
  } else if(strcmp(cmd,"setTimer")==0){
    int id=doc["id"]|1; int m=doc["minutes"]|0;
    if(id>=1 && id<=4){ 
      L[id-1].timerMinutes=m; 
      if(L[id-1].relay && m>0) L[id-1].timerEndEpoch=time(nullptr)+m*60; 
      else L[id-1].timerEndEpoch=0; 
      saveSettingsToFS();
    }
  } else if(strcmp(cmd,"setLimit")==0){
    int id=doc["id"]|1; unsigned long s=doc["seconds"]|0;
    if(id>=1 && id<=4 && s>0){ L[id-1].usageLimitSeconds=s; saveSettingsToFS(); }
  } else if(strcmp(cmd,"setPrice")==0){ 
    unitPrice=doc["price"]|8.0; 
    saveSettingsToFS();
  } else if(strcmp(cmd,"clearNotifs")==0){ 
    SPIFFS.remove(NOTIFS_FILE); 
    pushNotification("Notifs cleared"); 
  }
}

// ---------------- HTTP (static files) ----------------
void handleFileRead(String path){
  if(path.endsWith("/")) path += "index.html";
  String ct="text/plain";
  if(path.endsWith(".html")) ct="text/html";
  else if(path.endsWith(".js")) ct="application/javascript";
  else if(path.endsWith(".css")) ct="text/css";
  else if(path.endsWith(".json")) ct="application/json";
  else if(path.endsWith(".ico")) ct="image/x-icon";

  if(!SPIFFS.exists(path)){ server.send(404,"text/plain","Not found"); return; }
  File f = SPIFFS.open(path,"r");
  server.streamFile(f,ct);
  f.close();
}

// ---------------- Broadcast ----------------
void broadcastState(){
  DynamicJsonDocument doc(1024);
  doc["type"]="state"; doc["unitPrice"]=unitPrice;
  JsonArray arr = doc.createNestedArray("loads");
  for(int i=0;i<4;i++){
    JsonObject o=arr.createNestedObject();
    o["id"]=i+1; o["voltage"]=L[i].V; o["current"]=L[i].I; o["power"]=L[i].P; o["energy"]=L[i].Wh;
    o["relay"]=L[i].relay; o["onSecToday"]=L[i].onSecondsToday; o["limitSec"]=L[i].usageLimitSeconds;
    o["timerMin"]=L[i].timerMinutes; if(L[i].timerEndEpoch>0) o["timerEnd"]=L[i].timerEndEpoch; o["cost"]=L[i].cost;
  }
  String out; serializeJson(doc,out); webSocket.broadcastTXT(out);
}

// ---------------- Setup ----------------
void setup(){
  Serial.begin(115200);
  initSPIFFS(); 
  connectWiFi();

  for(int i=0;i<4;i++){ 
    pinMode(RELAY_PINS[i],OUTPUT); 
    digitalWrite(RELAY_PINS[i],RELAY_OFF); 
    L[i].relay=false; 
  }

  for(int i=0;i<4;i++){ 
    if(INA[i]->begin()){ 
      inaPresent[i]=true; 
      Serial.printf("INA %d found\n",i+1); 
    } else { 
      inaPresent[i]=false; 
      Serial.printf("INA %d NOT found\n",i+1); 
    } 
  }

  loadSettingsFromFS();

  // -------- Static files (one-liner) --------
  // Serve everything in /data as web root, defaulting to index.html
  server.serveStatic("/", SPIFFS, "/");

  // Optional explicit routes (keep for clarity)
  server.on("/", [](){ handleFileRead("/index.html"); });
  server.on("/index.html", [](){ handleFileRead("/index.html"); });
  server.on("/styles.css", [](){ handleFileRead("/styles.css"); });
  server.on("/app.js", [](){ handleFileRead("/app.js"); });
  server.on("/logs.json", [](){ handleFileRead("/logs.json"); });
  server.on("/settings.json", [](){ handleFileRead("/settings.json"); });
  server.on("/notifs.json", [](){ handleFileRead("/notifs.json"); });
  server.on("/favicon.ico", [](){ handleFileRead("/favicon.ico"); }); // optional

  // Catch-all: never trigger the internal "request handler not found"
  server.onNotFound([](){
    // Try to serve the requested file from SPIFFS; if missing, fall back to index.html
    String uri = server.uri();
    if(uri == "/") uri = "/index.html";
    if(SPIFFS.exists(uri)) { handleFileRead(uri); return; }
    handleFileRead("/index.html"); // SPA fallback
  });

  server.begin();
  webSocket.begin(); 
  webSocket.onEvent(handleWS);
  Serial.println("Server started at "+WiFi.localIP().toString());
}

// ---------------- Loop ----------------
void loop(){
  webSocket.loop(); 
  server.handleClient();

  unsigned long now=millis(); 
  if(now-lastSec<1000) return; 
  lastSec=now;

  time_t tnow=time(nullptr);

  for(int i=0;i<4;i++){
    if(inaPresent[i]){
      float v=INA[i]->getBusVoltage_V();
      float c=INA[i]->getCurrent_mA()/1000.0f; 
      if(c<0)c=0;
      float p=v*c; 
      L[i].V=v; 
      L[i].I=c; 
      L[i].P=p;
      L[i].Wh+=(p/3600.0); 
      L[i].cost=(L[i].Wh/1000.0)*unitPrice;
    } else { 
      L[i].V=L[i].I=L[i].P=0; 
    }

    if(L[i].relay){
      L[i].onSecondsToday++; 
      if(L[i].usageLimitSeconds>0 && L[i].onSecondsToday>=L[i].usageLimitSeconds){
        digitalWrite(RELAY_PINS[i],RELAY_OFF); 
        L[i].relay=false; 
        pushNotification("Relay "+String(i+1)+" auto OFF by limit"); 
      }
    }

    if(L[i].timerEndEpoch>0 && tnow>=L[i].timerEndEpoch){
      digitalWrite(RELAY_PINS[i],RELAY_OFF); 
      L[i].relay=false; 
      L[i].timerEndEpoch=0; 
      pushNotification("Relay "+String(i+1)+" auto OFF by timer"); 
    }
  }

  broadcastState();
}
