#include <TFT_eSPI.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h> // watchdog
#include <DNSServer.h>
DNSServer dnsServer;

// ================= TFT =================
TFT_eSPI tft = TFT_eSPI();
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define LINE_HEIGHT 8
#define MAX_COLS 50

// ================= UART =================
HardwareSerial SerialNMEA(2); // RX na GPIO35
HardwareSerial SerialOut(1);  // TX na GPIO22
#define TX_PIN 22

// ================= NMEA BUFFER =================
char incomingData[128];
uint8_t dataIndex = 0;
int cursorY = 50;
bool paused = false;

// ================= TOUCH =================
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
#define TS_MINX 300
#define TS_MAXX 3800
#define TS_MINY 300
#define TS_MAXY 3800
uint32_t lastTouch = 0;

// ================= BAUD =================
long baudRates[] = {4800, 9600, 19200, 38400, 57600, 115200};
int baudIndex = 0;
long currentBaud = 4800;
const int numBauds = sizeof(baudRates) / sizeof(baudRates[0]);

// ================= WEB =================
WebServer server(80);
String nmeaLog = "";
const int MAX_LOG_LINES = 400;
int logLineCount = 0;
String webRawBuffer = "";
String repeatSentences[4];
unsigned long repeatInterval[4] = {1000, 1000, 1000, 1000};
unsigned long lastRepeatTime[4] = {0, 0, 0, 0};
bool repeatEnabled[4] = {false, false, false, false};

// SSE – prosta obsługa jednego klienta (wystarczy w 99% przypadków)
WiFiClient sseClient;
bool sseConnected = false;
int rawX = 0;
int rawY = 50;

int webRawCounter = 0;

// ================= NVS =================
Preferences prefs;

// ================= DIAGNOSTYKA =================
unsigned long lastDiagUpdate = 0;
const unsigned long diagInterval = 1000;

// ================= GET COLOR =================
uint16_t getColor(const char *line)
{
  if (strncmp(line, "WEB:", 4) == 0)
    return TFT_MAGENTA;
  if (!strncmp(line, "$GPGGA", 6) || !strncmp(line, "$GPRMC", 6) || !strncmp(line, "$GPZDA", 6) ||
      !strncmp(line, "$GPGLL", 6) || !strncmp(line, "$GPVTG", 6))
    return TFT_GREEN;
  if (!strncmp(line, "$GPHDT", 6) || !strncmp(line, "$GPHDG", 6))
    return TFT_ORANGE;
  if (!strncmp(line, "!AI", 3))
    return TFT_RED;
  return TFT_WHITE;
}

// ================= DODATKOWE ZMIENNE GLOBALNE =================
String wifiPassword = "12345678";   // hasło WiFi – możesz później zrobić edycję przez web
bool wifiEnabled = true;            // stan WiFi AP
unsigned long passwordShowTime = 0; // kiedy pokazano hasło (do auto-ukrycia)

// ================= DRAW INTERFACE – Z NOWYMI PRZYCISKAMI =================
void drawInterface()
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(1, 1);
  tft.print("BAUD: ");
  tft.setTextSize(2);
  tft.setCursor(1, 10);
  tft.print(currentBaud);

  // Przycisk <
  tft.fillRoundRect(150, 5, 50, 30, 5, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.setTextSize(2);
  tft.setCursor(165, 13);
  tft.print("<");

  // Przycisk >
  tft.fillRoundRect(205, 5, 50, 30, 5, TFT_BLUE);
  tft.setCursor(230, 13);
  tft.print(">");

  // Przycisk PAUSE/RUN
  tft.fillRoundRect(260, 5, 55, 30, 5, paused ? TFT_RED : TFT_GREEN);
  tft.setTextColor(TFT_BLACK, paused ? TFT_RED : TFT_GREEN);
  tft.setTextSize(1);
  tft.setCursor(268, 15);
  tft.print(paused ? "PAUSE" : "RUN");

  // NOWOŚĆ: Przycisk WiFi ON/OFF
  uint16_t wifiColor = wifiEnabled ? TFT_RED : TFT_GREEN;
  tft.fillRoundRect(95, 5, 50, 30, 5, wifiColor);
  tft.setTextColor(TFT_BLACK, wifiColor);
  tft.setTextSize(1);
  tft.setCursor(95, 15);
  tft.print(wifiEnabled ? "WiFi OFF" : "WiFi ON");

  //// NOWOŚĆ: Przycisk PASS
  // tft.fillRoundRect(70, 5, 70, 30, 5, TFT_ORANGE);
  // tft.setTextColor(TFT_BLACK, TFT_ORANGE);
  // tft.setCursor(80, 15);
  // tft.print("PASS");

  // Pokaz hasło jeśli wciśnięto przycisk (na 5 sekund)
  if (millis() - passwordShowTime < 5000 && passwordShowTime > 0)
  {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 25);
    tft.print("Pass: ");
    tft.print(wifiPassword);
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  cursorY = 50;
}

void drawPauseButton()
{
  tft.fillRoundRect(260, 5, 55, 30, 5, paused ? TFT_RED : TFT_GREEN);
  tft.setTextColor(TFT_BLACK, paused ? TFT_RED : TFT_GREEN);
  tft.setCursor(268, 15);
  tft.print(paused ? "PAUSE" : "RUN");
}

void printWrappedLine(const char *text, uint16_t color)
{
  tft.setTextColor(color, TFT_BLACK);
  int len = strlen(text), pos = 0;
  while (pos < len)
  {
    char line[MAX_COLS + 1];
    int i = 0;
    while (i < MAX_COLS && pos < len)
    {
      char c = text[pos++];
      if (c == '\r')
        continue;
      if (c == '\n')
        break;
      line[i++] = c;
    }
    if (i == 0)
      break;
    line[i] = '\0';
    tft.setCursor(0, cursorY);
    tft.print(line);
    cursorY += LINE_HEIGHT;
    if (cursorY >= SCREEN_HEIGHT - LINE_HEIGHT - 20)
    {
      tft.fillRect(0, 40, SCREEN_WIDTH, SCREEN_HEIGHT - 60, TFT_BLACK);
      cursorY = 50;
    }
  }
}

void drawDiagnostics()
{
  tft.fillRect(0, SCREEN_HEIGHT - 20, SCREEN_WIDTH, 20, TFT_BLACK);
  tft.setTextSize(1);

  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  uint8_t usedPercent = 100 - map(freeHeap, 0, totalHeap, 0, 100);

  uint16_t color = TFT_CYAN;
  if (freeHeap < 30000)
    color = TFT_RED;
  else if (freeHeap < 60000)
    color = TFT_YELLOW;

  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(0, SCREEN_HEIGHT - 16);

  tft.print("Heap: ");
  tft.print(freeHeap / 1024);
  tft.print("kB/");
  tft.print(totalHeap / 1024);
  tft.print("kB (");
  tft.print(usedPercent);
  tft.print("%)");

  UBaseType_t stackHW = uxTaskGetStackHighWaterMark(NULL);
  if (stackHW > 0)
  {
    tft.print(" | Stack: ");
    tft.print(stackHW);
    tft.print("B");
  }

  unsigned long uptimeMin = millis() / 60000;
  tft.print(" | Up: ");
  tft.print(uptimeMin);
  tft.print("min");
}

void changeBaud()
{
  currentBaud = baudRates[baudIndex];
  SerialNMEA.end();
  SerialNMEA.begin(currentBaud, SERIAL_8N1, 35, -1);
  SerialOut.end();
  SerialOut.begin(currentBaud, SERIAL_8N1, -1, TX_PIN);
  drawInterface();
  drawDiagnostics();
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(0, cursorY);
  tft.println("Baud changed...");
  cursorY += LINE_HEIGHT * 2;
}

// ================= NVS =================
void saveConfigToNVS(int index)
{
  prefs.begin("nmea", false);
  prefs.putString(("sent" + String(index)).c_str(), repeatSentences[index]);
  prefs.putULong(("intv" + String(index)).c_str(), repeatInterval[index]);
  prefs.putBool(("enab" + String(index)).c_str(), repeatEnabled[index]);
  prefs.end();
}

void loadConfigFromNVS()
{
  prefs.begin("nmea", true);
  for (int i = 0; i < 4; i++)
  {
    repeatSentences[i] = prefs.getString(("sent" + String(i)).c_str(), "");
    repeatInterval[i] = prefs.getULong(("intv" + String(i)).c_str(), 1000);
    repeatEnabled[i] = false; // zawsze false po restarcie (zgodnie z Twoją prośbą wcześniej)
  }
  prefs.end();
}

// ================= LOG TAIL =================
String getLogTail(int maxLines)
{
  int lines = 0;
  int pos = nmeaLog.length();

  while (pos > 0 && lines < maxLines)
  {
    pos = nmeaLog.lastIndexOf('\n', pos - 1);
    lines++;
  }

  if (pos < 0)
    return nmeaLog;
  return nmeaLog.substring(pos + 1);
}

// ================= SSE PUSH =================
void pushSSE(const String &line)
{
  if (sseConnected && sseClient.connected())
  {
    sseClient.print("data: ");
    sseClient.print(line);
    sseClient.print("\n\n");
    sseClient.flush();
  }
}

// ================= HTML Z SSE =================
const char *htmlForm = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
body { background:#000; color:#0f0; font-family: monospace; margin:10px;}
input[type=text]{ width:60%; padding:5px; margin:5px; background:#222; color:#0f0; border:none; border-radius:3px;}
input[type=number]{ width:15%; padding:5px; margin:5px; background:#222; color:#0f0; border:none; border-radius:3px;}
button{ padding:5px 10px; border:none; border-radius:3px; font-weight:bold; margin:2px;}
button.start.active{ background:#0f0; color:#000; }
button.start.inactive{ background:#444; color:#aaa; }
button.stop.active{ background:#f00; color:#fff; }
button.stop.inactive{ background:#444; color:#aaa; }
#log{ 
  background:#111; 
  padding:5px; 
  height:300px; 
  overflow-y: scroll; 
  white-space: pre-wrap;
  word-wrap: break-word;
  margin-top:10px;
  font-size: 13px;
  line-height: 1.3;
}
</style>
</head>
<body>
<h2>NMEA Console</h2>
<form action="javascript:void(0);">
<input type="text" id="sentence0" placeholder="Enter NMEA sentence 1" onkeydown="if(event.keyCode==13) event.preventDefault();">
<input type="number" id="interval0" value="1000">
<button id="start0" class="start inactive" onclick="startSentence(0);return false;">Start</button>
<button id="stop0" class="stop active" onclick="stopSentence(0);return false;">Stop</button><br>

<input type="text" id="sentence1" placeholder="Enter NMEA sentence 2" onkeydown="if(event.keyCode==13) event.preventDefault();">
<input type="number" id="interval1" value="1000">
<button id="start1" class="start inactive" onclick="startSentence(1);return false;">Start</button>
<button id="stop1" class="stop active" onclick="stopSentence(1);return false;">Stop</button><br>

<input type="text" id="sentence2" placeholder="Enter NMEA sentence 3" onkeydown="if(event.keyCode==13) event.preventDefault();">
<input type="number" id="interval2" value="1000">
<button id="start2" class="start inactive" onclick="startSentence(2);return false;">Start</button>
<button id="stop2" class="stop active" onclick="stopSentence(2);return false;">Stop</button><br>

<input type="text" id="sentence3" placeholder="Enter NMEA sentence 4" onkeydown="if(event.keyCode==13) event.preventDefault();">
<input type="number" id="interval3" value="1000">
<button id="start3" class="start inactive" onclick="startSentence(3);return false;">Start</button>
<button id="stop3" class="stop active" onclick="stopSentence(3);return false;">Stop</button><br>
</form>
<pre id="log"></pre>

<script>
const logDiv = document.getElementById('log');

// Główny mechanizm: SSE (real-time)
if ('EventSource' in window) {
  const eventSource = new EventSource('/events');

  eventSource.onmessage = function(event) {
    if (event.data) {
      logDiv.textContent += event.data + "\n";
      logDiv.scrollTop = logDiv.scrollHeight;
    }
  };

  eventSource.onerror = function() {
    console.log("SSE błąd – przechodzę na polling");
    eventSource.close();
    startPolling();
  };
} else {
  startPolling();
}

// Fallback: polling co 500ms
function startPolling() {
  setInterval(() => {
    fetch('/log?t=' + Date.now())
      .then(r => r.text())
      .then(txt => {
        logDiv.textContent = txt;
        logDiv.scrollTop = logDiv.scrollHeight;
      });
  }, 500);
}

function setButtonState(i, started){
    const startBtn = document.querySelector('#start'+i);
    const stopBtn = document.querySelector('#stop'+i);
    if(started){
        startBtn.className="start active";
        stopBtn.className="stop inactive";
    } else {
        startBtn.className="start inactive";
        stopBtn.className="stop active";
    }
}

window.addEventListener("load", () => {
  fetch('/getSentences?v=' + Date.now())
    .then(resp => resp.json())
    .then(data => {
      for (let i = 0; i < 4; i++) {
        document.getElementById('sentence' + i).value = data['sent' + i] || '';
        document.getElementById('interval' + i).value = data['intv' + i] || 1000;
        setButtonState(i, data['enab' + i] === true);
      }
    });
});

function startSentence(i){
    const sentence = encodeURIComponent(document.getElementById('sentence'+i).value);
    const interval = document.getElementById('interval'+i).value;
    fetch('/start'+i+'?interval='+interval+'&sentence='+sentence);
    setButtonState(i, true);
}
function stopSentence(i){
    fetch('/stop'+i);
    setButtonState(i, false);
}
</script>
</body>
</html>
)rawliteral";

// ================= WEB HANDLERS =================
void handleRoot()
{
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "text/html", htmlForm);
}

void handleGetSentences()
{
  String json = "{";
  for (int i = 0; i < 4; i++)
  {
    if (i > 0)
      json += ",";
    String sent = repeatSentences[i];
    sent.replace("\\", "\\\\");
    sent.replace("\"", "\\\"");
    sent.replace("\n", "\\n");
    sent.replace("\r", "\\r");
    sent.replace("\t", "\\t");
    json += "\"sent" + String(i) + "\":\"" + sent + "\"";
    json += ",\"intv" + String(i) + "\":" + String(repeatInterval[i]);
    json += ",\"enab" + String(i) + "\":" + (repeatEnabled[i] ? "true" : "false");
  }
  json += "}";
  server.send(200, "application/json", json);
}

#define START_HANDLER(N)                                                   \
  void handleStart##N()                                                    \
  {                                                                        \
    repeatEnabled[N] = true;                                               \
    if (server.hasArg("interval"))                                         \
      repeatInterval[N] = server.arg("interval").toInt();                  \
    if (server.hasArg("sentence"))                                         \
      repeatSentences[N] = server.arg("sentence");                         \
    saveConfigToNVS(N);                                                    \
    if (repeatSentences[N].length() > 0)                                   \
    {                                                                      \
      String toSend = repeatSentences[N] + "\r\n";                         \
      String s = "WEB:" + toSend;                                          \
      SerialOut.print(toSend);                                             \
      printWrappedLine(toSend.c_str(), TFT_MAGENTA);                       \
      nmeaLog += s;                                                        \
      pushSSE(s);                                                          \
      logLineCount++;                                                      \
      while (logLineCount > MAX_LOG_LINES)                                 \
      {                                                                    \
        size_t pos = nmeaLog.indexOf("\r\n");                              \
        if (pos == -1)                                                     \
          pos = nmeaLog.indexOf('\n');                                     \
        if (pos == -1)                                                     \
          break;                                                           \
        nmeaLog.remove(0, pos + (pos == nmeaLog.indexOf("\r\n") ? 2 : 1)); \
        logLineCount--;                                                    \
      }                                                                    \
    }                                                                      \
    server.send(200, "text/plain", "OK");                                  \
  }

#define STOP_HANDLER(N)                   \
  void handleStop##N()                    \
  {                                       \
    repeatEnabled[N] = false;             \
    server.send(200, "text/plain", "OK"); \
  }

START_HANDLER(0)
START_HANDLER(1)
START_HANDLER(2)
START_HANDLER(3)
STOP_HANDLER(0)
STOP_HANDLER(1) STOP_HANDLER(2) STOP_HANDLER(3)

    // ================= SETUP WEB =================
void setupWeb()
{
  WiFi.softAP("NMEA_SNIFFER_192.168.4.1", "12345678");
  dnsServer.start(53, "*", WiFi.softAPIP());

  IPAddress IP = WiFi.softAPIP();
  tft.setCursor(0, 40);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.print("AP IP: ");
  tft.println(IP);

  // ================= SSE =================
  server.on("/events", HTTP_GET, []()
  {
    WiFiClient client = server.client();
    if (!client) return;

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/event-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println("Access-Control-Allow-Origin: *");
    client.println();

    client.print("data: ");
    client.print(getLogTail(50));
    client.print("\n\n");
    client.flush();

    sseClient = client;
    sseConnected = true;
  });

  // ================= LOG =================
  server.on("/log", HTTP_GET, []()
  {
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "text/plain", getLogTail(50));
  });

  // ================= MAIN =================
  server.on("/", handleRoot);
  server.on("/getSentences", HTTP_GET, handleGetSentences);

  server.on("/start0", HTTP_GET, handleStart0);
  server.on("/stop0", HTTP_GET, handleStop0);
  server.on("/start1", HTTP_GET, handleStart1);
  server.on("/stop1", HTTP_GET, handleStop1);
  server.on("/start2", HTTP_GET, handleStart2);
  server.on("/stop2", HTTP_GET, handleStop2);
  server.on("/start3", HTTP_GET, handleStart3);
  server.on("/stop3", HTTP_GET, handleStop3);

  // ================= CAPTIVE PORTAL =================

  // Android
  server.on("/generate_204", []()
  {
    server.send(200, "text/html", htmlForm);
  });

  server.on("/connecttest.txt", []()
  {
    server.send(200, "text/plain", "OK");
  });

  // Apple
  server.on("/hotspot-detect.html", []()
  {
    server.send(200, "text/html", htmlForm);
  });

  // Windows
  server.on("/ncsi.txt", []()
  {
    server.send(200, "text/plain", "Microsoft NCSI");
  });

  // Fallback – przekieruj wszystko
  server.onNotFound([]()
  {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();

  WiFi.softAPdisconnect(true);
}

// ================= SETUP =================
void setup()
{
  tft.init();
  tft.setRotation(1);
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  SerialNMEA.begin(currentBaud, SERIAL_8N1, 35, -1);
  SerialOut.begin(currentBaud, SERIAL_8N1, -1, TX_PIN);

  loadConfigFromNVS();
  drawInterface();
  drawDiagnostics();
  setupWeb();

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
}

void loop()
{
  esp_task_wdt_reset();
  server.handleClient();

  // ================= REPEAT =================
  for (int i = 0; i < 4; i++)
  {
    if (repeatEnabled[i] && repeatSentences[i].length() > 0)
    {
      unsigned long now = millis();
      if (now - lastRepeatTime[i] >= repeatInterval[i])
      {
        String toSend = repeatSentences[i] + "\r\n";
        String s = "WEB:" + toSend;
        SerialOut.print(toSend);
        printWrappedLine(toSend.c_str(), TFT_MAGENTA);
        nmeaLog += s;
        pushSSE(s);
        logLineCount++;

        while (logLineCount > MAX_LOG_LINES)
        {
          size_t pos = nmeaLog.indexOf("\r\n");
          if (pos == -1)
            pos = nmeaLog.indexOf('\n');
          if (pos == -1)
            break;
          nmeaLog.remove(0, pos + (pos == nmeaLog.indexOf("\r\n") ? 2 : 1));
          logLineCount--;
        }

        lastRepeatTime[i] = now;
      }
    }
  }

  // ================= TOUCH =================
  if (ts.touched() && millis() - lastTouch > 200)
  {
    lastTouch = millis();
    TS_Point p = ts.getPoint();
    int16_t x = map(p.x, TS_MINX, TS_MAXX, 0, SCREEN_WIDTH);
    int16_t y = map(p.y, TS_MINY, TS_MAXY, 0, SCREEN_HEIGHT);

    if (y < 40)
    {
      if (x > 150 && x < 200)
      {
        baudIndex = (baudIndex - 1 + numBauds) % numBauds;
        changeBaud();
      }
      else if (x > 205 && x < 255)
      {
        baudIndex = (baudIndex + 1) % numBauds;
        changeBaud();
      }
      else if (x > 260 && x < 315)
      {
        paused = !paused;
        drawPauseButton();
      }
      else if (x > 95 && x < 145)
      {
        wifiEnabled = !wifiEnabled;
        if (wifiEnabled)
          WiFi.softAPdisconnect(true);
        else
          WiFi.softAP("ESP32_NMEA", wifiPassword.c_str());

        drawInterface();
      }
    }
  }

  // ================= UART =================
  while (SerialNMEA.available())
  {
    char c = SerialNMEA.read();
    if (paused)
      continue;

    // ================= LCD RAW =================
    char out = c;
    if (out < 32 || out > 126)
      out = '.';

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(rawX, rawY);
    tft.print(out);

    rawX += 6;

    if (rawX > SCREEN_WIDTH - 6)
    {
      rawX = 0;
      rawY += LINE_HEIGHT;
    }

    if (rawY > SCREEN_HEIGHT - 20)
    {
      tft.fillRect(0, 40, SCREEN_WIDTH, SCREEN_HEIGHT - 60, TFT_BLACK);
      rawY = 50;
    }

    // ================= WEB RAW =================
    char webChar = c;
    if (webChar < 32 || webChar > 126)
      webChar = '.';

    webRawBuffer += webChar;

    // wysyłaj tylko pełną linię
    if (c == '\n')
    {
      pushSSE(webRawBuffer);
      webRawBuffer = "";
    }

    dnsServer.processNextRequest();

    // ================= NORMALNE NMEA =================
    if (dataIndex < sizeof(incomingData) - 1)
      incomingData[dataIndex++] = c;

    if (c == '\n')
    {
      incomingData[dataIndex] = '\0';
      dataIndex = 0;

      uint16_t color = getColor(incomingData);
      printWrappedLine(incomingData, color);

      // synchronizacja kursora RAW
      rawX = 0;
      rawY = cursorY;

      webRawCounter = 0;

      String line = String(incomingData);
      if (!line.endsWith("\r\n"))
        line += "\r\n";

      nmeaLog += line;
      pushSSE(line);
      logLineCount++;

      while (logLineCount > MAX_LOG_LINES)
      {
        size_t pos = nmeaLog.indexOf("\r\n");
        if (pos == -1)
          pos = nmeaLog.indexOf('\n');
        if (pos == -1)
          break;
        nmeaLog.remove(0, pos + (pos == nmeaLog.indexOf("\r\n") ? 2 : 1));
        logLineCount--;
      }
    }
  }

  // ================= DIAG =================
  if (millis() - lastDiagUpdate >= diagInterval)
  {
    drawDiagnostics();
    lastDiagUpdate = millis();
  }
}