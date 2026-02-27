/**
 * Web Server - HTTP server, web interface, and WebSocket server for Arena Timer
 * Handles web UI serving, real-time WebSocket communication, and network setup
 */

#pragma once

#include <Ethernet.h>
#include <TimerDisplay.h>

// Forward declarations
class WebSocketClient;
struct SettingsData;

namespace WebServer
{
    // Pin definitions for W5500
    extern const int CS;
    extern const int SCK;
    extern const int MOSI;
    extern const int MISO;

    /// @brief Initialize the Ethernet connection with static IP
    /// @param mac MAC address (6 bytes)
    /// @param ip IP address (4 bytes)
    /// @return true if successful, false otherwise
    bool init(uint8_t mac[6], uint8_t ip[4]);

    /// @brief Initialize mDNS responder for hostname resolution
    /// @param hostname Hostname (without .local suffix)
    /// @return true if successful, false otherwise
    bool initMDNS(const char* hostname);

    /// @brief Register an mDNS service record (e.g., "arenatimer._http")
    /// @param serviceName Service instance and type (without .local suffix)
    /// @param port Port number to advertise
    /// @return true if successful, false otherwise
    bool addMDNSService(const char* serviceName, uint16_t port);

    /// @brief Update mDNS responder (call in loop)
    void updateMDNS();

    /// @brief Start the HTTP web server on specified port (serves HTML page only)
    /// @param port Port number (default 80)
    void startWebServer(uint16_t port = 80);

    /// @brief Initialize the WebSocket server for real-time browser communication
    /// @param port Port number (default 81)
    void initWebSocket(uint16_t port = 81);

    /// @brief Process WebSocket server events (call in loop)
    void loopWebSocket();

    /// @brief Broadcast current timer state to all connected WebSocket clients
    void broadcastTimerState();

    /// @brief Broadcast FightTimer connection status to all connected WebSocket clients
    void broadcastFightTimerStatus();

    /// @brief Handle incoming HTTP client connections (call in loop)
    void handleClient();

    /// @brief Set the TimerDisplay instance for WebSocket control
    /// @param display Pointer to TimerDisplay instance
    void setTimerDisplay(TimerDisplay* display);

    /// @brief Request a display redraw (called when visual settings change)
    void requestRedraw();

    /// @brief Check and clear the redraw request flag
    /// @return true if a redraw was requested since last check
    bool needsRedraw();

    /// @brief Set the WebSocket client instance (FightTimer connection)
    /// @param wsClient Pointer to WebSocketClient instance
    void setWebSocketClient(WebSocketClient* wsClient);

    /// @brief Get the current Ethernet server
    EthernetServer& getServer();

    /// @brief Get the IP address as a string
    /// @return IP address string (e.g., "192.168.1.100")
    String getIPAddressString();

    /// @brief Apply a loaded SettingsData to the timer display and internal state.
    ///        Call after setTimerDisplay() during setup.
    /// @param data Settings loaded from flash
    void applySettings(const SettingsData& data);
}
