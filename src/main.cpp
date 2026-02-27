#include <Arduino.h>
#include <RGBMatrix.h>
#include <TimerDisplay.h>
#include <WebServer.h>
#include <WebSocketClient.h>
#include <Settings.h>

// Debug flag - set to false to disable debug messages for better timing
#define DEBUG_MAIN false

// Debug printing macros
#if DEBUG_MAIN
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
#endif

// Include various fonts (12pt and below for 64x32 display)
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

// Network configuration
// MAC is auto-generated from the RP2040's unique board ID (flash serial number).
// Byte 0 has bit 1 set (locally administered) and bit 0 cleared (unicast).
uint8_t mac[6];
uint8_t static_ip[] = {10, 0, 0, 21};  // Static IP fallback
const char* hostname = "arenatimer";     // Access via http://arenatimer.local

// Create timer display in TIMER mode (countdown)
TimerDisplay timerDisplay(RGBMatrix::getMatrix(), TimerDisplay::Mode::TIMER);

// Create WebSocket client for FightTimer integration
WebSocketClient* wsClient = nullptr;

void setup()
{
  Serial.begin(115200);
  delay(1000);
  
  DEBUG_PRINTLN("\n=== Arena Timer Firmware ===");

  // Generate a unique MAC address from the RP2040 flash serial number.
  // This guarantees each board gets a different MAC with no manual config.
  pico_unique_board_id_t board_id;
  pico_get_unique_board_id(&board_id);
  mac[0] = (board_id.id[0] | 0x02) & 0xFE;  // locally administered, unicast
  mac[1] = board_id.id[1];
  mac[2] = board_id.id[2];
  mac[3] = board_id.id[5];  // spread across the 8 ID bytes for better entropy
  mac[4] = board_id.id[6];
  mac[5] = board_id.id[7];
  
  DEBUG_PRINT("MAC: ");
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) DEBUG_PRINT("0");
    DEBUG_PRINT(String(mac[i], HEX));
    if (i < 5) DEBUG_PRINT(":");
  }
  DEBUG_PRINTLN("");
  
  // Initialize RGB Matrix
  DEBUG_PRINTLN("Initializing RGB Matrix...");
  RGBMatrix::init();
  RGBMatrix::setOrientation(180);
  RGBMatrix::clear();
  
  // Initialize Ethernet with static IP
  DEBUG_PRINTLN("Initializing Ethernet...");
  bool ethernet_ok = false;
  String ip_address;
  
  if (WebServer::init(mac, static_ip))
  {
    ethernet_ok = true;
    ip_address = WebServer::getIPAddressString();
    DEBUG_PRINT("IP Address: ");
    DEBUG_PRINTLN(ip_address);
    
    // Initialize mDNS for easy hostname access
    if (WebServer::initMDNS(hostname))
    {
      DEBUG_PRINT("Access timer at: http://");
      DEBUG_PRINT(hostname);
      DEBUG_PRINTLN(".local");
    }
  }
  else
  {
    DEBUG_PRINTLN("ERROR: Ethernet initialization failed!");
    DEBUG_PRINTLN("Timer will work, but web interface is unavailable.");
    DEBUG_PRINTLN("Check your W5500 wiring and connections.");
  }
  
  if (ethernet_ok)
  {
    DEBUG_PRINT("IP Address: ");
    DEBUG_PRINTLN(ip_address);
    
    // Display IP address on LED matrix for 5 seconds
    RGBMatrix::clear();
    RGBMatrix::getMatrix().setFont(&TomThumb);  // Smaller font for multi-line IP
    RGBMatrix::getMatrix().setTextSize(1);
    RGBMatrix::getMatrix().setTextColor(RGBMatrix::getMatrix().color565(0, 255, 0));  // Green
    
    // Split IP into two lines if too long
    if (ip_address.length() > 15)
    {
      RGBMatrix::getMatrix().setCursor(2, 6);
      RGBMatrix::getMatrix().print("IP:");
      RGBMatrix::getMatrix().setCursor(2, 14);
      RGBMatrix::getMatrix().print(ip_address.substring(0, 10));
      RGBMatrix::getMatrix().setCursor(2, 22);
      RGBMatrix::getMatrix().print(ip_address.substring(10));
    }
    else
    {
      RGBMatrix::getMatrix().setCursor(2, 10);
      RGBMatrix::getMatrix().print("IP:");
      RGBMatrix::getMatrix().setCursor(2, 18);
      RGBMatrix::getMatrix().print(ip_address);
    }
    
    RGBMatrix::show();
    delay(10000);  // Show IP for 10 seconds
    
    // Start web server
    WebServer::startWebServer(80);
    DEBUG_PRINTLN("Web server started!");

    // Advertise HTTP service over mDNS for better .local discovery
    WebServer::addMDNSService("arenatimer._http", 80);
  }
  
  // Configure initial timer display
  DEBUG_PRINTLN("Configuring timer display...");
  timerDisplay.setFont(&FreeSansBold12pt7b);
  // Default color thresholds already set in constructor: Blue (default), Yellow (<2min), Red (<1min)
  timerDisplay.getTimer().setDuration({3, 0, 0});  // 3 minutes default
  timerDisplay.getTimer().reset();
  
  // Initialize WebSocket client
  DEBUG_PRINTLN("Initializing WebSocket client...");
  wsClient = new WebSocketClient(&timerDisplay.getTimer());
  DEBUG_PRINTLN("WebSocket client ready (not connected)");
  
  // Pass WebSocket client to WebServer for API access
  WebServer::setWebSocketClient(wsClient);
  
  // Give WebServer a pointer to the timer display for WebSocket broadcasting
  WebServer::setTimerDisplay(&timerDisplay);

  // Load persisted settings from flash (must happen after setTimerDisplay)
  Settings::begin();
  SettingsData saved;
  if (Settings::load(saved)) {
    DEBUG_PRINTLN("Loaded saved settings from flash");
    WebServer::applySettings(saved);
  } else {
    DEBUG_PRINTLN("No saved settings – using defaults");
  }
  
  // Start WebSocket server on port 81 for real-time browser communication
  WebServer::initWebSocket(81);
  DEBUG_PRINTLN("WebSocket server started on port 81");
  
  DEBUG_PRINTLN("\n=== Setup Complete ===");
  if (ethernet_ok)
  {
    DEBUG_PRINTLN("Web Interface:");
    DEBUG_PRINT("  - http://");
    DEBUG_PRINT(hostname);
    DEBUG_PRINTLN(".local");
    DEBUG_PRINT("  - http://");
    DEBUG_PRINTLN(ip_address);
  }
  DEBUG_PRINTLN("======================\n");
}

void loop()
{
  // Intelligent redraw logic: only redraw when displayed content actually changes
  static Timer::Components last_displayed_time = {0, 0, 0};
  static bool last_displayed_ms = false;  // Track if we were showing milliseconds
  static unsigned long last_redraw_ms = 0;
  static bool last_was_running = false;
  static bool last_was_paused = false;
  static bool last_was_expired = false;
  static bool last_was_idle = true;
  bool content_changed = false;
  
  unsigned long loop_start = millis();
  
  // Stagger network operations into separate time slots to spread SPI load.
  // Each operation runs every 30ms (~33Hz, plenty responsive), but offset
  // by 10ms so only ONE W5500 SPI interaction happens per slot.
  // This prevents bursting all 3 operations together which causes visible
  // display flicker from ISR contention.
  static unsigned long last_http_ms = 0;
  static unsigned long last_ws_server_ms = 10;   // offset by 10ms
  static unsigned long last_ws_client_ms = 20;   // offset by 20ms
  
  if (loop_start - last_http_ms >= 30) {
    last_http_ms = loop_start;
    WebServer::handleClient();
  }
  
  if (loop_start - last_ws_server_ms >= 30) {
    last_ws_server_ms = loop_start;
    WebServer::loopWebSocket();
  }
  
  if (loop_start - last_ws_client_ms >= 30) {
    last_ws_client_ms = loop_start;
    if (wsClient) {
      wsClient->poll();
    }
  }
  
  // Get current timer state
  Timer::Components current_time = timerDisplay.getTimer().getRemainingTime();
  bool is_idle = timerDisplay.getTimer().isIdle();
  bool is_running = timerDisplay.getTimer().isRunning();
  bool is_paused = timerDisplay.getTimer().isPaused();
  bool is_expired = timerDisplay.getTimer().isExpired();
  
  // Determine if milliseconds are being displayed (same logic as TimerDisplay::draw)
  bool show_ms = false;
  if (is_running || is_paused || is_expired) {
    // Only show ms in timer mode when < 1 minute remaining
    if (current_time.minutes == 0) {
      show_ms = true;
    }
  }
  
  // Tick blink state every iteration (single source of truth for blink timing)
  if (timerDisplay.tickBlink()) {
    content_changed = true;
  }
  
  // Force redraw on any timer state transition (pause/unpause/reset/expire)
  if (is_running != last_was_running || is_paused != last_was_paused ||
      is_expired != last_was_expired || is_idle != last_was_idle) {
    content_changed = true;
  }
  last_was_running = is_running;
  last_was_paused = is_paused;
  last_was_expired = is_expired;
  last_was_idle = is_idle;
  
  // Check if a WebSocket command requested a redraw (flip, settings, thresholds)
  if (WebServer::needsRedraw()) {
    content_changed = true;
  }
  
  // Determine if we need to redraw based on what's actually displayed
  if (!content_changed && is_idle) {
    // Idle state: only redraw if minutes/seconds changed
    if (current_time.seconds != last_displayed_time.seconds ||
        current_time.minutes != last_displayed_time.minutes) {
      content_changed = true;
    }
  } else if (!content_changed && is_running) {
    // Running: redraw based on what's actually displayed
    if (show_ms) {
      // Showing milliseconds: update only when deciseconds (0.1s) changes
      int last_deciseconds = (last_displayed_time.milliseconds / 100);
      int curr_deciseconds = (current_time.milliseconds / 100);
      
      if (current_time.seconds != last_displayed_time.seconds ||
          current_time.minutes != last_displayed_time.minutes ||
          curr_deciseconds != last_deciseconds) {
        content_changed = true;
      }
    } else {
      // Not showing milliseconds: update only when seconds change
      if (current_time.seconds != last_displayed_time.seconds ||
          current_time.minutes != last_displayed_time.minutes) {
        content_changed = true;
      }
    }
  }
  
  // Only update display if content changed
  if (content_changed) {
    // Store current state for next comparison
    last_displayed_time = current_time;
    last_displayed_ms = show_ms;
    last_redraw_ms = loop_start;
    
    // Clear the display
    RGBMatrix::clear();
    
    // Update and draw the timer
    timerDisplay.update();
    
    // Show the display
    RGBMatrix::show();
    
    // Push updated timer state to connected browser clients
    WebServer::broadcastTimerState();
  }
  
  // Print status to serial for debugging (every 5 seconds)
  static unsigned long last_serial = 0;
  if (millis() - last_serial >= 5000)
  {
    Timer::Components remaining = timerDisplay.getTimer().getRemainingTime();
    DEBUG_PRINT("Status: ");
    if (timerDisplay.getTimer().isExpired())
    {
      DEBUG_PRINT("EXPIRED ");
    }
    else if (timerDisplay.getTimer().isRunning())
    {
      DEBUG_PRINT("RUNNING ");
    }
    else if (timerDisplay.getTimer().isPaused())
    {
      DEBUG_PRINT("PAUSED ");
    }
    else if (timerDisplay.getTimer().isIdle())
    {
      DEBUG_PRINT("IDLE ");
    }
    
    DEBUG_PRINT("| Time: ");
    DEBUG_PRINT(remaining.minutes);
    DEBUG_PRINT(":");
    if (remaining.seconds < 10) DEBUG_PRINT("0");
    DEBUG_PRINT(remaining.seconds);
    DEBUG_PRINT(".");
    if (remaining.milliseconds < 100) DEBUG_PRINT("0");
    if (remaining.milliseconds < 10) DEBUG_PRINT("0");
    DEBUG_PRINTLN(remaining.milliseconds);
    
    last_serial = millis();
  }
}

