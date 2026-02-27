#include "WebServer.h"
#include "WebSocketClient.h"
#include "RGBMatrix.h"
#include "Settings.h"
#include <SPI.h>
#include <EthernetBonjour.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// Debug flag - set to false to disable debug messages for better timing
#define DEBUG_WEBSERVER false

// Debug printing macros
#if DEBUG_WEBSERVER
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
#endif

// Font includes (12pt and below for 64x32 display)
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMono12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerifBold9pt7b.h>
#include <Fonts/FreeSerifBold12pt7b.h>
// Retro/pixel fonts
#include <Fonts/Org_01.h>
#include <Fonts/Picopixel.h>
#include <Fonts/TomThumb.h>
// Custom fonts
#include <CustomFonts/Aquire_BW0ox12pt7b.h>
#include <CustomFonts/AquireBold_8Ma6012pt7b.h>
#include <CustomFonts/AquireLight_YzE0o12pt7b.h>

namespace WebServer {
    // Pin definitions for W5500
    const int CS = 1;    // RP2040 GPIO1
    const int SCK = 2;   // RP2040 GPIO2
    const int MOSI = 3;  // RP2040 GPIO3
    const int MISO = 4;  // RP2040 GPIO4

    EthernetServer* server = nullptr;
    bool mdns_initialized = false;
    const char* mdns_hostname = nullptr;
    const char* mdns_service_name = nullptr;
    uint16_t mdns_service_port = 0;
    unsigned long last_mdns_retry_ms = 0;
    WebSocketClient* wsClient = nullptr;
    int current_orientation = 180;  // Track current display orientation

    // WebSocket server for real-time browser communication
    WebSocketsServer* wsServer = nullptr;
    TimerDisplay* timerDisplayPtr = nullptr;

    // Track current settings so we can report them to newly-connected clients
    int currentFontId = 4;       // Default: FreeSansBold12pt7b
    int8_t currentSpacing = 3;   // Default letter spacing

    // Flag for main loop: commands that change visual appearance need a redraw
    volatile bool redrawFlag = false;

    // Forward declarations for WebSocket server helpers
    void sendInitialState(uint8_t clientNum);
    void sendThresholds(uint8_t clientNum);
    void handleWsCommand(uint8_t num, uint8_t* payload, size_t length);
    const GFXfont* getFontById(int fontId);
    uint8_t getTextSizeForFont(int fontId);

    // Collect live settings into a SettingsData struct and stage for flash write
    void saveCurrentSettings() {
        if (!timerDisplayPtr) return;

        SettingsData sd;
        Timer::Components dur = timerDisplayPtr->getTimer().getDuration();
        sd.duration_sec    = dur.minutes * 60 + dur.seconds;
        sd.font_id         = (uint8_t)currentFontId;
        sd.spacing         = currentSpacing;
        sd.brightness      = timerDisplayPtr->getBrightness();
        sd.orientation     = (uint16_t)current_orientation;

        uint8_t r, g, b;
        timerDisplayPtr->getDefaultColor(r, g, b);
        sd.default_r = r;
        sd.default_g = g;
        sd.default_b = b;

        size_t count;
        const TimerDisplay::ColorThreshold* th = timerDisplayPtr->getColorThresholds(count);
        sd.threshold_count = (uint8_t)count;
        for (size_t i = 0; i < count && i < 10; i++) {
            sd.thresholds[i].seconds = (uint16_t)th[i].seconds;
            sd.thresholds[i].r       = th[i].r;
            sd.thresholds[i].g       = th[i].g;
            sd.thresholds[i].b       = th[i].b;
        }

        Settings::save(sd);
    }

    bool init(uint8_t mac[6], uint8_t ip[4]) {
        // Configure SPI pins for W5500
        SPI.setSCK(SCK);
        SPI.setTX(MOSI);
        SPI.setRX(MISO);
        SPI.setCS(CS);
        
        // Must call SPI.begin() to actually start the SPI peripheral
        SPI.begin();

        // Tell Ethernet library which CS pin to use
        Ethernet.init(CS);
        
        // Try DHCP first, fall back to static IP if DHCP fails
        DEBUG_PRINTLN("Attempting DHCP...");
        // DHCP timeout is in milliseconds; 30 ms is too short for most networks.
        if (Ethernet.begin(mac, 10000) == 0) {
            DEBUG_PRINTLN("DHCP failed, using static IP");
            // DHCP failed, use static IP
            Ethernet.begin(mac, IPAddress(ip[0], ip[1], ip[2], ip[3]));
        } else {
            DEBUG_PRINTLN("DHCP successful");
        }

        // Give Ethernet time to initialize and establish link
        delay(2000);

        // Check if Ethernet is connected
        if (Ethernet.hardwareStatus() == EthernetNoHardware) {
            DEBUG_PRINTLN("ERROR: Ethernet hardware not found");
            return false;
        }

        if (Ethernet.linkStatus() == LinkOFF) {
            DEBUG_PRINTLN("WARNING: Ethernet cable not connected");
        }

        DEBUG_PRINT("Ethernet initialized - IP: ");
        DEBUG_PRINTLN(Ethernet.localIP());

        return true;
    }

    bool initMDNS(const char* hostname) {
        mdns_hostname = hostname;
        last_mdns_retry_ms = millis();
        if (!EthernetBonjour.begin(hostname)) {
            DEBUG_PRINTLN("ERROR: Failed to start mDNS responder");
            mdns_initialized = false;
            return false;
        }

        DEBUG_PRINT("mDNS responder started: ");
        DEBUG_PRINT(hostname);
        DEBUG_PRINTLN(".local");
        mdns_initialized = true;
        if (mdns_service_name != nullptr && mdns_service_port != 0) {
            EthernetBonjour.addServiceRecord(mdns_service_name, mdns_service_port, MDNSServiceTCP);
        }
        DEBUG_PRINTLN("mDNS ready");
        return true;
    }

    bool addMDNSService(const char* serviceName, uint16_t port) {
        mdns_service_name = serviceName;
        mdns_service_port = port;
        if (!mdns_initialized || serviceName == nullptr || port == 0) {
            return false;
        }

        return EthernetBonjour.addServiceRecord(serviceName, port, MDNSServiceTCP) == 1;
    }

    void updateMDNS() {
        if (mdns_initialized) {
            EthernetBonjour.run();
            return;
        }

        if (mdns_hostname == nullptr) {
            return;
        }

        if (Ethernet.linkStatus() != LinkON) {
            return;
        }

        unsigned long now = millis();
        if (now - last_mdns_retry_ms < 5000) {
            return;
        }

        last_mdns_retry_ms = now;
        if (EthernetBonjour.begin(mdns_hostname)) {
            mdns_initialized = true;
            if (mdns_service_name != nullptr && mdns_service_port != 0) {
                EthernetBonjour.addServiceRecord(mdns_service_name, mdns_service_port, MDNSServiceTCP);
            }
            DEBUG_PRINTLN("mDNS retry succeeded");
        } else {
            DEBUG_PRINTLN("mDNS retry failed");
        }
    }


    String getIPAddressString() {
        IPAddress ip = Ethernet.localIP();
        return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
    }

    void applySettings(const SettingsData& data) {
        if (!timerDisplayPtr) return;

        // Duration
        unsigned int minutes = data.duration_sec / 60;
        unsigned int seconds = data.duration_sec % 60;
        timerDisplayPtr->getTimer().setDuration({minutes, seconds, 0});
        timerDisplayPtr->getTimer().reset();

        // Font
        currentFontId = data.font_id;
        timerDisplayPtr->setFont(getFontById(currentFontId));
        timerDisplayPtr->setTextSize(getTextSizeForFont(currentFontId));

        // Spacing
        currentSpacing = data.spacing;
        timerDisplayPtr->setLetterSpacing(currentSpacing);

        // Brightness
        timerDisplayPtr->setBrightness(data.brightness);

        // Orientation
        current_orientation = data.orientation;
        RGBMatrix::setOrientation(current_orientation);

        // Default color
        timerDisplayPtr->setDefaultColor(data.default_r, data.default_g, data.default_b);

        // Color thresholds
        timerDisplayPtr->clearColorThresholds();
        for (uint8_t i = 0; i < data.threshold_count && i < 10; i++) {
            timerDisplayPtr->addColorThreshold(
                data.thresholds[i].seconds,
                data.thresholds[i].r,
                data.thresholds[i].g,
                data.thresholds[i].b);
        }
    }

    void startWebServer(uint16_t port) {
        if (server != nullptr) {
            delete server;
        }
        server = new EthernetServer(port);
        server->begin();
        DEBUG_PRINT("Web server started on port ");
        DEBUG_PRINTLN(port);
    }

    EthernetServer& getServer() {
        return *server;
    }

    void setWebSocketClient(WebSocketClient* client) {
        wsClient = client;
        DEBUG_PRINTLN("WebSocket client registered with WebServer");
    }

    // Helper function to send HTTP response
    void sendHTTPResponse(EthernetClient& client, int code, const char* contentType, const String& body) {
        client.print("HTTP/1.1 ");
        client.print(code);
        client.println(code == 200 ? " OK" : " Error");
        client.print("Content-Type: ");
        client.println(contentType);
        client.print("Content-Length: ");
        client.println(body.length());
        client.println("Connection: close");
        client.println();
        client.print(body);
    }

    // Helper function to URL decode a string
    String urlDecode(const String& str) {
        String decoded = "";
        char temp[] = "00";
        for (unsigned int i = 0; i < str.length(); i++) {
            char c = str.charAt(i);
            if (c == '%' && i + 2 < str.length()) {
                temp[0] = str.charAt(i + 1);
                temp[1] = str.charAt(i + 2);
                decoded += (char)strtol(temp, NULL, 16);
                i += 2;
            } else if (c == '+') {
                decoded += ' ';
            } else {
                decoded += c;
            }
        }
        return decoded;
    }

    // Helper function to parse hex color string to RGB
    void parseColor(const String& hexColor, uint8_t& r, uint8_t& g, uint8_t& b) {
        String color = hexColor;
        if (color.startsWith("#")) {
            color = color.substring(1);
        }
        
        long number = strtol(color.c_str(), NULL, 16);
        r = (number >> 16) & 0xFF;
        g = (number >> 8) & 0xFF;
        b = number & 0xFF;
    }

    // Helper function to get font pointer from font ID
    const GFXfont* getFontById(int fontId) {
        switch(fontId) {
            case 0: return nullptr; // Default font (5x7 pixels)
            // Sans-Serif fonts
            case 1: return &FreeSans9pt7b;
            case 2: return &FreeSans12pt7b;
            case 3: return &FreeSansBold9pt7b;
            case 4: return &FreeSansBold12pt7b;
            // Monospace fonts
            case 5: return &FreeMono9pt7b;
            case 6: return &FreeMono12pt7b;
            case 7: return &FreeMonoBold9pt7b;
            case 8: return &FreeMonoBold12pt7b;
            // Serif fonts
            case 9: return &FreeSerif9pt7b;
            case 10: return &FreeSerif12pt7b;
            case 11: return &FreeSerifBold9pt7b;
            case 12: return &FreeSerifBold12pt7b;
            // Retro/Pixel fonts
            case 13: return &Org_01;
            case 14: return &Picopixel;
            case 15: return &TomThumb;
            // Custom fonts
            case 16: return &Aquire_BW0ox12pt7b;
            case 17: return &AquireBold_8Ma6012pt7b;
            case 18: return &AquireLight_YzE0o12pt7b;
            default: return &FreeSansBold12pt7b; // Default to 12pt bold
        }
    }

    // Helper function to get text size for font ID
    uint8_t getTextSizeForFont(int fontId) {
        // Default font and retro fonts use larger scaling
        if (fontId == 0) return 2;  // Default 5x7 @ 2x
        if (fontId >= 13 && fontId <= 15) return 3;  // Retro fonts @ 3x (they're very small)
        return 1;  // All other fonts @ 1x
    }

    // =========================================================================
    // WebSocket Server - real-time communication with browser clients
    // =========================================================================

    void setTimerDisplay(TimerDisplay* display) {
        timerDisplayPtr = display;
    }

    void requestRedraw() {
        redrawFlag = true;
    }

    bool needsRedraw() {
        if (redrawFlag) {
            redrawFlag = false;
            return true;
        }
        return false;
    }

    void sendInitialState(uint8_t clientNum) {
        if (!timerDisplayPtr || !wsServer) return;

        // 1. Timer state
        Timer::Components remaining = timerDisplayPtr->getTimer().getRemainingTime();
        const char* state = "idle";
        if (timerDisplayPtr->getTimer().isExpired()) state = "expired";
        else if (timerDisplayPtr->getTimer().isRunning()) state = "running";
        else if (timerDisplayPtr->getTimer().isPaused()) state = "paused";

        char json[192];
        snprintf(json, sizeof(json),
            "{\"type\":\"state\",\"time\":\"%u:%02u\",\"ms\":%u,\"state\":\"%s\"}",
            remaining.minutes, remaining.seconds, remaining.milliseconds, state);
        wsServer->sendTXT(clientNum, json);

        // 2. Network info
        snprintf(json, sizeof(json),
            "{\"type\":\"network\",\"ip\":\"%s\"}", getIPAddressString().c_str());
        wsServer->sendTXT(clientNum, json);

        // 3. FightTimer connection status
        snprintf(json, sizeof(json),
            "{\"type\":\"ftStatus\",\"connected\":%s,\"status\":\"%s\",\"url\":\"%s\"}",
            (wsClient && wsClient->isConnected()) ? "true" : "false",
            wsClient ? wsClient->getStatus() : "Not initialized",
            wsClient ? wsClient->getServerUrl() : "");
        wsServer->sendTXT(clientNum, json);

        // 4. Thresholds
        sendThresholds(clientNum);

        // 5. Current settings
        Timer::Components dur = timerDisplayPtr->getTimer().getDuration();
        int totalSec = dur.minutes * 60 + dur.seconds;
        snprintf(json, sizeof(json),
            "{\"type\":\"settings\",\"duration\":%d,\"font\":%d,\"spacing\":%d,\"brightness\":%d}",
            totalSec, currentFontId, currentSpacing, timerDisplayPtr->getBrightness());
        wsServer->sendTXT(clientNum, json);
    }

    void sendThresholds(uint8_t clientNum) {
        if (!timerDisplayPtr || !wsServer) return;

        size_t count;
        const TimerDisplay::ColorThreshold* thresholds = timerDisplayPtr->getColorThresholds(count);
        uint8_t def_r, def_g, def_b;
        timerDisplayPtr->getDefaultColor(def_r, def_g, def_b);

        JsonDocument doc;
        doc["type"] = "thresholds";
        JsonArray arr = doc["thresholds"].to<JsonArray>();
        for (size_t i = 0; i < count; i++) {
            JsonObject t = arr.add<JsonObject>();
            t["seconds"] = thresholds[i].seconds;
            char color[8];
            snprintf(color, sizeof(color), "#%02x%02x%02x",
                thresholds[i].r, thresholds[i].g, thresholds[i].b);
            t["color"] = String(color);
        }
        char defColor[8];
        snprintf(defColor, sizeof(defColor), "#%02x%02x%02x", def_r, def_g, def_b);
        doc["defaultColor"] = String(defColor);

        String output;
        serializeJson(doc, output);
        wsServer->sendTXT(clientNum, output);
    }

    void handleWsCommand(uint8_t num, uint8_t* payload, size_t length) {
        if (!timerDisplayPtr) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error) {
            DEBUG_PRINT("WS JSON parse error: ");
            DEBUG_PRINTLN(error.c_str());
            return;
        }

        const char* cmd = doc["cmd"];
        if (!cmd) return;

        DEBUG_PRINT("WS command: ");
        DEBUG_PRINTLN(cmd);

        if (strcmp(cmd, "start") == 0) {
            timerDisplayPtr->getTimer().start();
        } else if (strcmp(cmd, "pause") == 0) {
            timerDisplayPtr->getTimer().stop();
        } else if (strcmp(cmd, "reset") == 0) {
            timerDisplayPtr->getTimer().reset();
        } else if (strcmp(cmd, "flip") == 0) {
            current_orientation = (current_orientation == 180) ? 0 : 180;
            RGBMatrix::setOrientation(current_orientation);
            redrawFlag = true;
        } else if (strcmp(cmd, "settings") == 0) {
            // Duration
            if (doc["duration"].is<int>()) {
                int totalSeconds = doc["duration"];
                if (totalSeconds > 0 && totalSeconds <= 3600) {
                    unsigned int minutes = totalSeconds / 60;
                    unsigned int seconds = totalSeconds % 60;
                    timerDisplayPtr->getTimer().setDuration({minutes, seconds, 0});
                    timerDisplayPtr->getTimer().reset();
                }
            }
            // Font
            if (doc["font"].is<int>()) {
                currentFontId = doc["font"];
                timerDisplayPtr->setFont(getFontById(currentFontId));
                timerDisplayPtr->setTextSize(getTextSizeForFont(currentFontId));
            }
            // Spacing
            if (doc["spacing"].is<int>()) {
                currentSpacing = doc["spacing"];
                timerDisplayPtr->setLetterSpacing(currentSpacing);
            }
            // Brightness
            if (doc["brightness"].is<int>()) {
                int brightness = doc["brightness"];
                if (brightness >= 0 && brightness <= 255) {
                    timerDisplayPtr->setBrightness((uint8_t)brightness);
                }
            }
            redrawFlag = true;
        } else if (strcmp(cmd, "thresholds") == 0) {
            // Default color
            if (doc["default"].is<const char*>()) {
                uint8_t r, g, b;
                parseColor(doc["default"].as<String>(), r, g, b);
                timerDisplayPtr->setDefaultColor(r, g, b);
            }
            // Threshold array
            timerDisplayPtr->clearColorThresholds();
            JsonArray arr = doc["thresholds"].as<JsonArray>();
            if (!arr.isNull()) {
                for (JsonObject t : arr) {
                    unsigned int seconds = t["seconds"] | 0;
                    const char* color = t["color"];
                    if (color) {
                        uint8_t r, g, b;
                        parseColor(String(color), r, g, b);
                        timerDisplayPtr->addColorThreshold(seconds, r, g, b);
                    }
                }
            }
            redrawFlag = true;
        } else if (strcmp(cmd, "saveSettings") == 0) {
            saveCurrentSettings();
            // Notify the requesting client whether flash was actually written
            wsServer->sendTXT(num, "{\"type\":\"saved\",\"ok\":true}");
        } else if (strcmp(cmd, "ftConnect") == 0) {
            if (wsClient) {
                const char* host = doc["host"];
                uint16_t port = doc["port"] | 8765;
                const char* path = doc["path"] | "/socket.io/";

                if (host && strcmp(host, "localhost") != 0 && strcmp(host, "127.0.0.1") != 0) {
                    wsClient->connect(host, port, path);
                } else if (host) {
                    // Send error back to this client
                    wsServer->sendTXT(num,
                        "{\"type\":\"error\",\"message\":\"Cannot use localhost. Use your computer's actual IP.\"}");
                    return;
                }
            }
        } else if (strcmp(cmd, "ftDisconnect") == 0) {
            if (wsClient) {
                wsClient->disconnect();
            }
        }

        // After any command, broadcast updated state to all clients
        broadcastTimerState();
        broadcastFightTimerStatus();
    }

    void wsServerEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
        switch (type) {
            case WStype_CONNECTED:
                DEBUG_PRINTLN("Browser WebSocket client connected");
                sendInitialState(num);
                break;
            case WStype_DISCONNECTED:
                DEBUG_PRINTLN("Browser WebSocket client disconnected");
                break;
            case WStype_TEXT:
                handleWsCommand(num, payload, length);
                break;
            default:
                break;
        }
    }

    void initWebSocket(uint16_t port) {
        wsServer = new WebSocketsServer(port);
        wsServer->begin();
        wsServer->onEvent(wsServerEvent);
        DEBUG_PRINT("WebSocket server started on port ");
        DEBUG_PRINTLN(port);
    }

    void loopWebSocket() {
        if (wsServer) {
            wsServer->loop();
        }
    }

    void broadcastTimerState() {
        if (!wsServer || !timerDisplayPtr) return;

        Timer::Components remaining = timerDisplayPtr->getTimer().getRemainingTime();
        const char* state = "idle";
        if (timerDisplayPtr->getTimer().isExpired()) state = "expired";
        else if (timerDisplayPtr->getTimer().isRunning()) state = "running";
        else if (timerDisplayPtr->getTimer().isPaused()) state = "paused";

        // Total remaining in milliseconds for client-side interpolation
        unsigned long totalMs = (unsigned long)remaining.minutes * 60000UL
                              + (unsigned long)remaining.seconds * 1000UL
                              + remaining.milliseconds;

        char json[128];
        snprintf(json, sizeof(json),
            "{\"type\":\"state\",\"time\":\"%u:%02u\",\"ms\":%lu,\"state\":\"%s\"}",
            remaining.minutes, remaining.seconds, totalMs, state);
        wsServer->broadcastTXT(json);
    }

    void broadcastFightTimerStatus() {
        if (!wsServer) return;

        char json[256];
        snprintf(json, sizeof(json),
            "{\"type\":\"ftStatus\",\"connected\":%s,\"status\":\"%s\",\"url\":\"%s\"}",
            (wsClient && wsClient->isConnected()) ? "true" : "false",
            wsClient ? wsClient->getStatus() : "Not initialized",
            wsClient ? wsClient->getServerUrl() : "");
        wsServer->broadcastTXT(json);
    }

    // =========================================================================
    // HTTP Server - serves the web page only (all control via WebSocket)
    // =========================================================================

    void handleClient() {
        // Update mDNS responder to keep hostname resolution alive
        updateMDNS();
        
        if (server == nullptr) return;

        EthernetClient client = server->available();
        if (client) {
            String currentLine = "";
            String requestType = "";
            String requestPath = "";
            // Read HTTP request (blocking until headers complete)
            while (client.connected()) {
                if (client.available()) {
                    char c = client.read();
                    
                    if (c == '\n') {
                        if (currentLine.length() == 0) {
                            // End of headers
                            break;
                        } else {
                            // Parse request line
                            if (requestType == "") {
                                int firstSpace = currentLine.indexOf(' ');
                                int secondSpace = currentLine.indexOf(' ', firstSpace + 1);
                                if (firstSpace > 0 && secondSpace > firstSpace) {
                                    requestType = currentLine.substring(0, firstSpace);
                                    requestPath = currentLine.substring(firstSpace + 1, secondSpace);
                                }
                            }
                            currentLine = "";
                        }
                    } else if (c != '\r') {
                        currentLine += c;
                    }
                }
            }

            // Handle different endpoints
            if (requestPath == "/" || requestPath.startsWith("/?")) {
                // Serve web page
                DEBUG_PRINTLN("Client connected - serving web page");
                client.println("HTTP/1.1 200 OK");
                client.println("Content-Type: text/html");
                client.println("Connection: close");
                client.println();
                
                // Send HTML in chunks using F() to save RAM
                client.print(F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"));
                client.print(F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>"));
                client.print(F("<title>Arena Timer Control</title><style>"));
                client.print(F("body{font-family:Arial,sans-serif;margin:0;padding:20px;"));
                client.print(F("background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"));
                client.print(F("min-height:100vh}"));
                client.print(F(".container{background:white;border-radius:10px;padding:30px;"));
                client.print(F("box-shadow:0 10px 40px rgba(0,0,0,0.2);max-width:1400px;margin:0 auto}"));
                client.print(F("h1{text-align:center;color:#333;margin-bottom:30px}"));
                client.print(F(".grid-container{display:grid;grid-template-columns:repeat(3,1fr);"));
                client.print(F("gap:20px;margin-top:20px}"));
                client.print(F(".grid-column{display:flex;flex-direction:column;gap:20px}"));
                client.print(F("@media (max-width:1200px){.grid-container{grid-template-columns:1fr}}"));
                client.print(F("@media (max-width:600px){"));
                client.print(F("body{padding:12px}h1{font-size:22px}"));
                client.print(F(".container{padding:16px}"));
                client.print(F(".section{padding:14px}"));
                client.print(F(".controls{grid-template-columns:1fr}"));
                client.print(F(".duration-inputs{flex-wrap:wrap}"));
                client.print(F(".duration-inputs input{width:60px;padding:8px}"));
                client.print(F(".threshold-item{flex-wrap:wrap;gap:8px}"));
                client.print(F(".threshold-item .time-inputs{flex-wrap:wrap;white-space:normal}"));
                client.print(F(".threshold-item input[type='number']{width:48px;padding:6px}"));
                client.print(F("input[type='color']{min-width:44px;height:40px}"));
                client.print(F("}"));
                client.print(F(".section{padding:20px;background:#f5f5f5;"));
                client.print(F("border-radius:8px}.section h2{margin-top:0;color:#667eea;font-size:18px}"));
                client.print(F(".controls{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:15px}"));
                client.print(F("button{padding:15px 20px;border:none;border-radius:6px;font-size:16px;"));
                client.print(F("cursor:pointer;transition:all 0.3s;font-weight:bold}"));
                client.print(F(".btn-start{background:#4CAF50;color:white}"));
                client.print(F(".btn-start:hover{background:#45a049}"));
                client.print(F(".btn-pause{background:#FF9800;color:white}"));
                client.print(F(".btn-pause:hover{background:#e68900}"));
                client.print(F(".btn-resume{background:#4CAF50;color:white}"));
                client.print(F(".btn-resume:hover{background:#45a049}"));
                client.print(F(".btn-reset{background:#f44336;color:white}"));
                client.print(F(".btn-reset:hover{background:#da190b}"));
                client.print(F(".btn-save{background:#2196F3;color:white}"));
                client.print(F(".btn-save:hover{background:#1976D2}"));
                client.print(F(".save-section{margin-top:20px;text-align:center}"));
                client.print(F(".form-group{margin-bottom:15px}"));
                client.print(F("label{display:block;margin-bottom:5px;color:#555;font-weight:bold}"));
                client.print(F("input[type='number'],input[type='color'],select{width:100%;padding:10px;"));
                client.print(F("border:2px solid #ddd;border-radius:6px;font-size:14px;box-sizing:border-box}"));
                client.print(F("input[type='number']:focus,input[type='color']:focus,select:focus{"));
                client.print(F("border-color:#667eea;outline:none}"));
                client.print(F("input[type='color']{height:45px;cursor:pointer;border-radius:6px;min-width:60px}"));
                client.print(F(".threshold-list{margin-bottom:15px}"));
                client.print(F(".threshold-item{display:flex;align-items:center;gap:10px;"));
                client.print(F("margin-bottom:10px;padding:12px;background:white;border-radius:8px;"));
                client.print(F("border-left:4px solid #667eea;box-shadow:0 2px 4px rgba(0,0,0,0.05)}"));
                client.print(F(".threshold-item .time-inputs{display:flex;gap:5px;align-items:center;"));
                client.print(F("flex:1;white-space:nowrap}"));
                client.print(F(".threshold-item .when-label{color:#666;font-weight:500;white-space:nowrap}"));
                client.print(F(".threshold-item input[type='number']{width:60px;padding:8px;text-align:center;"));
                client.print(F("font-size:16px;font-weight:bold;flex-shrink:0}"));
                client.print(F(".threshold-item .time-label{font-size:12px;color:#999;font-weight:normal}"));
                client.print(F(".threshold-item .arrow{color:#667eea;font-size:20px;margin:0 8px}"));
                client.print(F(".threshold-default{display:flex;align-items:center;gap:10px;"));
                client.print(F("padding:12px;background:white;border-radius:8px;"));
                client.print(F("border-left:4px solid #667eea;box-shadow:0 2px 4px rgba(0,0,0,0.05);margin-bottom:10px}"));
                client.print(F(".threshold-default .label{flex:1;color:#666;font-weight:500}"));
                client.print(F(".duration-card{padding:20px;background:white;border-radius:8px;"));
                client.print(F("box-shadow:0 2px 4px rgba(0,0,0,0.05);margin-top:15px}"));
                client.print(F(".duration-inputs{display:flex;gap:8px;align-items:center;margin-top:10px}"));
                client.print(F(".duration-inputs input{width:80px;text-align:center;font-size:16px;font-weight:bold}"));
                client.print(F(".duration-inputs span{color:#666;font-size:14px}"));
                client.print(F(".btn-remove{background:#ff5252;color:white;padding:8px 12px;border:none;"));
                client.print(F("border-radius:6px;cursor:pointer;font-size:14px;font-weight:bold;"));
                client.print(F("transition:background 0.2s}"));
                client.print(F(".btn-remove:hover{background:#ff1744}"));
                client.print(F(".btn-add{background:#4CAF50;color:white;padding:12px;border:none;"));
                client.print(F("border-radius:8px;cursor:pointer;width:100%;font-size:14px;font-weight:bold;"));
                client.print(F("margin-bottom:15px;transition:background 0.2s}"));
                client.print(F(".btn-add:hover{background:#45a049}"));
                client.print(F(".info-display{background:white;padding:12px;border-radius:8px;"));
                client.print(F("margin-bottom:15px;border-left:4px solid #667eea;"));
                client.print(F("box-shadow:0 2px 4px rgba(0,0,0,0.05)}"));
                client.print(F(".info-label{color:#666;font-size:12px;font-weight:500;text-transform:uppercase}"));
                client.print(F(".info-value{color:#333;font-size:16px;font-weight:bold;margin-top:4px;"));
                client.print(F("font-family:monospace}"));
                client.print(F("</style></head><body><div class='container'>"));
                client.print(F("<h1>⏱️ Arena Timer Control</h1>"));
                client.print(F("<div class='grid-container'>"));
                
                // Column 1: Timer Controls & Duration
                client.print(F("<div class='grid-column'>"));
                client.print(F("<div class='section'><h2>🎮 Timer Controls</h2>"));
                client.print(F("<div class='info-display'>"));
                client.print(F("<div class='form-group'>Current Time</div>"));
                client.print(F("<div class='info-value' id='timerTime'>--:--</div>"));
                client.print(F("</div>"));
                client.print(F("<div class='controls'>"));
                client.print(F("<button id='startBtn' class='btn-start' onclick='toggleStartPause()'>▶️ Start</button>"));
                client.print(F("<button class='btn-reset' onclick='sendCommand(\"reset\")'>↩️ Reset</button>"));
                client.print(F("<button class='btn-pause' onclick='toggleOrientation()'>🔄 Flip Display</button>"));
                client.print(F("</div>"));
                client.print(F("</div>"));  // End Timer Controls section
                client.print(F("<div class='section'><h2>⏲️ Timer Duration</h2>"));
                client.print(F("<div class='duration-inputs'>"));
                client.print(F("<input type='number' id='durationMin' value='3' min='0' max='60' onchange='sendSettings()'>")); 
                client.print(F("<span>min</span>"));
                client.print(F("<input type='number' id='durationSec' value='0' min='0' max='59' onchange='sendSettings()'>")); 
                client.print(F("<span>sec</span></div></div>"));
                
                client.print(F("</div>"));  // End column 1
                
                // Column 2: Color Thresholds & Font Selection
                client.print(F("<div class='grid-column'>"));
                client.print(F("<div class='section'><h2>⏱️ Color Thresholds</h2>"));
                client.print(F("<p style='font-size:13px;color:#666;margin-bottom:20px'>"));
                client.print(F("The timer automatically changes color as time runs out</p>"));
                client.print(F("<div id='thresholds' class='threshold-list'></div>"));
                client.print(F("<button class='btn-add' onclick='addThreshold()'>+ Add Threshold</button>"));
                client.print(F("<p style='font-size:13px;color:#666;margin:15px 0 10px 0;font-style:italic'>"));
                client.print(F("When no threshold matches:</p>"));
                client.print(F("<div class='threshold-default'>"));
                client.print(F("<span class='label'>Default Color</span>"));
                client.print(F("<span class='arrow'>→</span>"));
                client.print(F("<input type='color' id='defaultColor' value='#00ff00' oninput='if(isColorTooDark(this.value)){return;}sendThresholdSettings()' onchange='if(isColorTooDark(this.value)){return;}sendThresholdSettings()'>")); 
                client.print(F("</div></div>"));
                
                client.print(F("<div class='section'><h2>🔤 Font Selection</h2>"));
                client.print(F("<div class='duration-card'>"));
                client.print(F("<label for='fontSelect' style='margin-bottom:10px'>Display Font:</label>"));
                client.print(F("<select id='fontSelect' style='font-size:16px' onchange='sendSettings()'>")); 
                client.print(F("<option value='0'>Adafruit Default (5x7 @ 2x scale)</option>"));
                client.print(F("<optgroup label='Sans-Serif'>"));
                client.print(F("<option value='1'>Sans 9pt</option>"));
                client.print(F("<option value='2'>Sans 12pt</option>"));
                client.print(F("<option value='3'>Sans Bold 9pt</option>"));
                client.print(F("<option value='4' selected>Sans Bold 12pt (default)</option>"));
                client.print(F("</optgroup>"));
                client.print(F("<optgroup label='Monospace'>"));
                client.print(F("<option value='5'>Mono 9pt</option>"));
                client.print(F("<option value='6'>Mono 12pt</option>"));
                client.print(F("<option value='7'>Mono Bold 9pt</option>"));
                client.print(F("<option value='8'>Mono Bold 12pt</option>"));
                client.print(F("</optgroup>"));
                client.print(F("<optgroup label='Serif'>"));
                client.print(F("<option value='9'>Serif 9pt</option>"));
                client.print(F("<option value='10'>Serif 12pt</option>"));
                client.print(F("<option value='11'>Serif Bold 9pt</option>"));
                client.print(F("<option value='12'>Serif Bold 12pt</option>"));
                client.print(F("</optgroup>"));
                client.print(F("<optgroup label='Retro/Pixel'>"));
                client.print(F("<option value='13'>Org_01 (Retro @ 3x)</option>"));
                client.print(F("<option value='14'>Picopixel (Tiny @ 3x)</option>"));
                client.print(F("<option value='15'>TomThumb (Pixel @ 3x)</option>"));
                client.print(F("</optgroup>"));
                client.print(F("<optgroup label='Custom Fonts'>"));
                client.print(F("<option value='16'>Aquire (12pt)</option>"));
                client.print(F("<option value='17'>Aquire Bold (12pt)</option>"));
                client.print(F("<option value='18'>Aquire Light (12pt)</option>"));
                client.print(F("</optgroup>"));
                client.print(F("</select>"));
                client.print(F("<label for='letterSpacing' style='margin-top:15px;margin-bottom:5px'>Character Spacing:</label>"));
                client.print(F("<div style='display:flex;align-items:center;gap:10px'>"));
                client.print(F("<input type='range' id='letterSpacing' min='-2' max='5' value='3' style='flex:1'>"));
                client.print(F("<span id='spacingValue' style='min-width:30px;text-align:center'>3</span>"));
                client.print(F("</div>"));
                client.print(F("<label for='brightness' style='margin-top:15px;margin-bottom:5px'>Display Brightness:</label>"));
                client.print(F("<div style='display:flex;align-items:center;gap:10px'>"));
                client.print(F("<input type='range' id='brightness' min='0' max='255' value='255' style='flex:1'>"));
                client.print(F("<span id='brightnessValue' style='min-width:30px;text-align:center'>100%</span>"));
                client.print(F("</div></div></div></div>"));  // End duration-card, Font Selection section, and column 2
                
                // Column 3: System Status & WebSocket Connection
                client.print(F("<div class='grid-column'>"));
                
                // System Status Card
                client.print(F("<div class='section'><h2>📊 System Status</h2>"));
                client.print(F("<div class='info-display'>"));
                client.print(F("<div class='form-group'>IP Address</div>"));
                client.print(F("<div class='info-value' id='ipAddress'>Loading...</div>"));
                client.print(F("</div>"));
                client.print(F("<div class='info-display'>"));
                client.print(F("<div class='form-group'>FightTimer Connection</div>"));
                client.print(F("<div class='info-value' id='wsStatus'>"));
                client.print(F("<span style='color:#888'>Checking...</span></div>"));
                client.print(F("</div></div>"));  // End System Status section
                
                // WebSocket Connection Card
                client.print(F("<div class='section'><h2>🔗 WebSocket Connection</h2>"));
                client.print(F("<div class='form-group'><label>Server Host / IP:</label>"));
                client.print(F("<input type='text' id='wsHost' value='localhost'>"));
                client.print(F("</div><div class='form-group'><label>Port:</label>"));
                client.print(F("<input type='number' id='wsPort' value='8765' min='1' max='65535'>"));
                client.print(F("</div><div class='form-group'><label>Path:</label>"));
                client.print(F("<input type='text' id='wsPath' value='/socket.io/'>"));
                client.print(F("</div><div style='display:flex;gap:10px'>"));
                client.print(F("<button class='btn-start' onclick='connectWebSocket()' style='flex:1'>"));
                client.print(F("🔗 Connect</button>"));
                client.print(F("<button class='btn-reset' onclick='disconnectWebSocket()' style='flex:1'>"));
                client.print(F("❌ Disconnect</button></div></div>"));  // End WebSocket Connection section
                
                client.print(F("</div>"));  // End column 3
                
                client.print(F("</div>"));  // End grid-container
                
                // Save Settings card (below grid, full-width)
                client.print(F("<div class='section save-section'>"));
                client.print(F("<button class='btn-save' onclick='saveToFlash()' style='width:100%'>💾 Save Settings</button>"));
                client.print(F("<p style='font-size:13px;color:#666;margin:10px 0 0 0;text-align:center'>"));
                client.print(F("Used to persist settings between power cycles / resets</p>"));
                client.print(F("</div>"));
                
                client.print(F("</div>"));  // End container
                client.print(F("<script>"));

                // --- WebSocket-based JavaScript (no HTTP polling) ---
                client.print(F("let ws;let thresholds=[];let wsReconnectTimer;"));

                // WebSocket connection to device
                client.print(F("function initWS(){"));
                client.print(F("const host=window.location.hostname;"));
                client.print(F("ws=new WebSocket('ws://'+host+':81/');"));
                client.print(F("ws.onopen=function(){};"));
                client.print(F("ws.onclose=function(){"));
                client.print(F("wsReconnectTimer=setTimeout(initWS,2000);};"));
                client.print(F("ws.onerror=function(){};"));
                client.print(F("ws.onmessage=function(e){try{handleMsg(JSON.parse(e.data));}catch(err){}};"));
                client.print(F("}"));

                // Client-side timer interpolation: predict countdown locally between
                // WebSocket syncs so the web display matches the LED with no perceivable lag.
                client.print(F("var _syncMs=0,_syncState='idle',_syncT=0,_raf=0;"));
                client.print(F("function fmtTime(ms){if(ms<0)ms=0;"));
                client.print(F("var m=Math.floor(ms/60000),s=Math.floor((ms%60000)/1000);"));
                client.print(F("return m+':'+(s<10?'0':'')+s;}"));
                client.print(F("function tickTimer(){_raf=requestAnimationFrame(tickTimer);"));
                client.print(F("if(_syncState!=='running')return;"));
                client.print(F("var now=performance.now(),elapsed=now-_syncT;"));
                client.print(F("var rem=_syncMs-elapsed;if(rem<0)rem=0;"));
                client.print(F("document.getElementById('timerTime').textContent=fmtTime(rem);}"));
                client.print(F("_raf=requestAnimationFrame(tickTimer);"));

                // Handle incoming messages from device
                client.print(F("function handleMsg(d){"));
                client.print(F("if(d.type==='state'){"));
                client.print(F("_syncMs=d.ms;_syncState=d.state;_syncT=performance.now();"));
                client.print(F("if(d.state!=='running'){document.getElementById('timerTime').textContent=d.time;}"));
                client.print(F("const btn=document.getElementById('startBtn');"));
                client.print(F("if(d.state==='running'){btn.textContent='⏸️ Pause';btn.className='btn-pause';btn.dataset.action='pause';}"));
                client.print(F("else if(d.state==='paused'){btn.textContent='▶️ Resume';btn.className='btn-resume';btn.dataset.action='start';}"));
                client.print(F("else{btn.textContent='▶️ Start';btn.className='btn-start';btn.dataset.action='start';}"));
                client.print(F("}else if(d.type==='network'){"));
                client.print(F("document.getElementById('ipAddress').textContent=d.ip;"));
                client.print(F("}else if(d.type==='ftStatus'){"));
                client.print(F("const el=document.getElementById('wsStatus');"));
                client.print(F("if(d.connected){el.innerHTML='<span style=\"color:#4CAF50\">✅ Connected to '+d.url+'</span>';}"));
                client.print(F("else{el.innerHTML='<span style=\"color:#888\">⚪ '+d.status+'</span>';}"));
                client.print(F("}else if(d.type==='thresholds'){"));
                client.print(F("thresholds=d.thresholds||[];"));
                client.print(F("if(d.defaultColor)document.getElementById('defaultColor').value=d.defaultColor;"));
                client.print(F("renderThresholds();"));
                client.print(F("}else if(d.type==='settings'){"));
                client.print(F("if(d.duration!==undefined){document.getElementById('durationMin').value=Math.floor(d.duration/60);"));
                client.print(F("document.getElementById('durationSec').value=d.duration%60;}"));
                client.print(F("if(d.font!==undefined)document.getElementById('fontSelect').value=d.font;"));
                client.print(F("if(d.spacing!==undefined){document.getElementById('letterSpacing').value=d.spacing;"));
                client.print(F("document.getElementById('spacingValue').textContent=d.spacing;}"));
                client.print(F("if(d.brightness!==undefined){document.getElementById('brightness').value=d.brightness;"));
                client.print(F("document.getElementById('brightnessValue').textContent=Math.round((d.brightness/255)*100)+'%';}"));
                client.print(F("}}"));

                // Send command to device via WebSocket
                client.print(F("function sendCmd(obj){"));
                client.print(F("if(ws&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify(obj));}"));

                // Color validation: reject near-black colors invisible on LED matrix
                client.print(F("function isColorTooDark(hex){"));
                client.print(F("const r=parseInt(hex.substr(1,2),16),g=parseInt(hex.substr(3,2),16),b=parseInt(hex.substr(5,2),16);"));
                client.print(F("return Math.max(r,g,b)<20;}"));

                // Timer control buttons
                client.print(F("function toggleStartPause(){"));
                client.print(F("const btn=document.getElementById('startBtn');"));
                client.print(F("const action=btn.dataset.action||'start';"));
                client.print(F("sendCmd({cmd:action});}"));
                client.print(F("function sendCommand(cmd){sendCmd({cmd:cmd});}"));
                client.print(F("function toggleOrientation(){sendCmd({cmd:'flip'});}"));
                client.print(F("function saveToFlash(){sendCmd({cmd:'saveSettings'});}"));

                // Apply all settings
                client.print(F("let _sTimer;function sendSettings(){"));
                client.print(F("clearTimeout(_sTimer);_sTimer=setTimeout(function(){"));
                client.print(F("const durationMin=parseInt(document.getElementById('durationMin').value)||0;"));
                client.print(F("const durationSec=parseInt(document.getElementById('durationSec').value)||0;"));
                client.print(F("const duration=durationMin*60+durationSec;"));
                client.print(F("const font=parseInt(document.getElementById('fontSelect').value);"));
                client.print(F("const spacing=parseInt(document.getElementById('letterSpacing').value);"));
                client.print(F("const brightness=parseInt(document.getElementById('brightness').value);"));
                client.print(F("sendCmd({cmd:'settings',duration:duration,font:font,spacing:spacing,brightness:brightness});"));
                client.print(F("saveSettingsToStorage();},50);}"));

                client.print(F("let _tTimer;function sendThresholdSettings(){"));
                client.print(F("clearTimeout(_tTimer);_tTimer=setTimeout(function(){"));
                client.print(F("const defaultColor=document.getElementById('defaultColor').value;"));
                client.print(F("sendCmd({cmd:'thresholds',thresholds:thresholds,default:defaultColor});"));
                client.print(F("saveSettingsToStorage();},50);}"));

                // Threshold management
                client.print(F("function renderThresholds(){"));
                client.print(F("const container=document.getElementById('thresholds');"));
                client.print(F("container.innerHTML='';"));
                client.print(F("thresholds.forEach((t,i)=>{"));
                client.print(F("const div=document.createElement('div');"));
                client.print(F("div.className='threshold-item';"));
                client.print(F("const mins=Math.floor(t.seconds/60);const secs=t.seconds%60;"));
                client.print(F("div.innerHTML=`<div class='time-inputs'>"));
                client.print(F("<span class='when-label'>When ≤</span>"));
                client.print(F("<input type='number' value='${mins}' min='0' max='60' "));
                client.print(F("onchange='updateThreshold(${i},\"minutes\",this.value)'>"));
                client.print(F("<span class='time-label'>min</span>"));
                client.print(F("<input type='number' value='${secs}' min='0' max='59' "));
                client.print(F("onchange='updateThreshold(${i},\"seconds\",this.value)'>"));
                client.print(F("<span class='time-label'>sec</span></div>"));
                client.print(F("<span class='arrow'>→</span>"));
                client.print(F("<input type='color' value='${t.color}' "));
                client.print(F("oninput='updateThreshold(${i},\"color\",this.value)' onchange='updateThreshold(${i},\"color\",this.value)'>"));
                client.print(F("<button class='btn-remove' onclick='removeThreshold(${i})'>✕</button>`;"));
                client.print(F("container.appendChild(div);});}"));
                client.print(F("function addThreshold(){"));
                client.print(F("const dc=document.getElementById('defaultColor').value;"));
                client.print(F("thresholds.push({seconds:60,color:dc});renderThresholds();sendThresholdSettings();}"));
                client.print(F("function removeThreshold(i){thresholds.splice(i,1);renderThresholds();sendThresholdSettings();}"));
                client.print(F("function updateThreshold(i,field,value){"));
                client.print(F("if(field==='minutes'){const s=thresholds[i].seconds%60;"));
                client.print(F("thresholds[i].seconds=parseInt(value)*60+s;}"));
                client.print(F("else if(field==='seconds'){const m=Math.floor(thresholds[i].seconds/60);"));
                client.print(F("thresholds[i].seconds=m*60+parseInt(value);}"));
                client.print(F("else if(field==='color'){if(isColorTooDark(value)){alert('Color is too dark to be visible on the LED display.');return;}"));
                client.print(F("thresholds[i].color=value;}"));
                client.print(F("sendThresholdSettings();}"));

                // FightTimer WebSocket connect/disconnect
                client.print(F("function connectWebSocket(){"));
                client.print(F("const host=document.getElementById('wsHost').value;"));
                client.print(F("const port=parseInt(document.getElementById('wsPort').value);"));
                client.print(F("const path=document.getElementById('wsPath').value;"));
                client.print(F("if(!host)return;"));
                client.print(F("sendCmd({cmd:'ftConnect',host:host,port:port,path:path});}"));
                client.print(F("function disconnectWebSocket(){"));
                client.print(F("sendCmd({cmd:'ftDisconnect'});}"));

                // Local storage persistence
                client.print(F("function saveSettingsToStorage(){"));
                client.print(F("const settings={"));
                client.print(F("durationMin:document.getElementById('durationMin').value,"));
                client.print(F("durationSec:document.getElementById('durationSec').value,"));
                client.print(F("font:document.getElementById('fontSelect').value,"));
                client.print(F("spacing:document.getElementById('letterSpacing').value,"));
                client.print(F("brightness:document.getElementById('brightness').value,"));
                client.print(F("defaultColor:document.getElementById('defaultColor').value,"));
                client.print(F("thresholds:JSON.stringify(thresholds)"));
                client.print(F("};localStorage.setItem('timerSettings',JSON.stringify(settings));}"));
                client.print(F("function loadSettingsFromStorage(){"));
                client.print(F("const stored=localStorage.getItem('timerSettings');"));
                client.print(F("if(!stored)return;"));
                client.print(F("try{const settings=JSON.parse(stored);"));
                client.print(F("if(settings.durationMin)document.getElementById('durationMin').value=settings.durationMin;"));
                client.print(F("if(settings.durationSec)document.getElementById('durationSec').value=settings.durationSec;"));
                client.print(F("if(settings.font)document.getElementById('fontSelect').value=settings.font;"));
                client.print(F("if(settings.spacing)document.getElementById('letterSpacing').value=settings.spacing;"));
                client.print(F("if(settings.brightness){"));
                client.print(F("document.getElementById('brightness').value=settings.brightness;"));
                client.print(F("const percent=Math.round((settings.brightness/255)*100);"));
                client.print(F("document.getElementById('brightnessValue').textContent=percent+'%';}"));
                client.print(F("if(settings.defaultColor)document.getElementById('defaultColor').value=settings.defaultColor;"));
                client.print(F("if(settings.thresholds){try{thresholds=JSON.parse(settings.thresholds);}catch(e){}}"));
                client.print(F("}catch(e){}}"));

                // Slider event listeners (update display label AND send settings live)
                client.print(F("document.getElementById('letterSpacing').addEventListener('input',function(){"));
                client.print(F("document.getElementById('spacingValue').textContent=this.value;sendSettings();});"));
                client.print(F("document.getElementById('brightness').addEventListener('input',function(){"));
                client.print(F("const percent=Math.round((this.value/255)*100);"));
                client.print(F("document.getElementById('brightnessValue').textContent=percent+'%';sendSettings();});"));

                // Initialize: load saved settings, then connect WebSocket (which delivers live state)
                client.print(F("loadSettingsFromStorage();initWS();"));
                client.print(F("</script></body></html>"));
                
                DEBUG_PRINTLN("Web page sent");
            } else {
                sendHTTPResponse(client, 404, "text/plain", "Not Found");
            }

            delay(1);
            client.stop();
            DEBUG_PRINTLN("Client disconnected");
        }
    }
}

