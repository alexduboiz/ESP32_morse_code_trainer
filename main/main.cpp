#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <cerrno>
#include "sdkconfig.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// Logging tag used for ESP-IDF log output.
static const char* TAG = "morse";

// WiFi credentials are loaded from project configuration via sdkconfig.
static const char* WIFI_SSID = CONFIG_WIFI_SSID;
static const char* WIFI_PASSWORD = CONFIG_WIFI_PASSWORD;

// Pin assignments for the dual-paddle keyer and the status LED.
static const int DIT_PIN = 4;
static const int DAH_PIN = 5;
static const int RGB_PIN = 48;

// Morse timing settings. dotTime is controlled by the web UI.
static const int DOT_DEFAULT = 200;
static const int SPEED_MIN = 50;
static const int SPEED_MAX = 250;

// WebSocket server port used for live browser updates.
static const int WS_PORT = 81;

static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static int wsServerSocket = -1;
static int wsClientSocket = -1;
static httpd_handle_t httpServer = nullptr;

// Runtime state for the Morse decoder and the live web UI.
static int dotTime = DOT_DEFAULT;
static std::string decodedText;
static std::string symbolBuffer;
static uint64_t lastSymbolEndMillis = 0;
static uint64_t symbolEndMillis = 0;
static uint64_t nextSymbolMillis = 0;
static bool symbolActive = false;
static bool ditNext = true;
static bool virtualDitPressed = false;
static bool virtualDahPressed = false;

static const std::map<char, std::string> morseCode = {
    {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
    {'F', "..-."}, {'G', "--."},  {'H', "...."}, {'I', ".."},  {'J', ".---"},
    {'K', "-.-"},  {'L', ".-.."}, {'M', "--"},   {'N', "-."},  {'O', "---"},
    {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},  {'S', "..."}, {'T', "-"},
    {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"}, {'Y', "-.--"}, {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
    {'(', "-.--.-"}, {')', "-.--.-"}, {'&', ".-..."}, {':', "---..."},
    {';', "-.-.-."}, {'=', "-...-"}, {'+', ".-.-."}, {'-', "-....-"},
    {'_', "..--.-"}, {'"', ".-..-."}, {'@', ".--.-."}
};
static std::map<std::string, char> reverseMorse;

// Morse timing helpers based on the current dot length.
static int dashTime() {
    return dotTime * 3;
}

static int symbolPause() {
    return dotTime;
}

static int letterPause() {
    return dotTime * 3;
}

static int wordPause() {
    return dotTime * 7;
}

// Return the current time in milliseconds using the ESP timer.
static uint64_t millis64() {
    return esp_timer_get_time() / 1000ULL;
}

// Return the web page served to the browser. This page includes the UI,
// live WebSocket update support, and the speed/clear controls.
static std::string htmlPage() {
    return R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Morse Decoder</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; }
    h1 { margin-bottom: 8px; }
    #decoded { white-space: pre-wrap; background: #111; color: #0f0; padding: 12px; border-radius: 6px; min-height: 80px; }
    #buffer { font-weight: bold; }
    .control { margin-top: 14px; }
    .control label { display: inline-block; margin-right: 12px; }
    button { padding: 10px 16px; font-size: 1rem; }
    #paddleContainer { display: flex; gap: 20px; margin-top: 14px; }
    .paddle-btn { width: 120px; height: 120px; font-size: 1.2rem; font-weight: bold; border-radius: 12px; border: 3px solid #555; background: #e0e0e0; cursor: pointer; user-select: none; -webkit-user-select: none; touch-action: none; }
    .paddle-btn.active { background: #333; color: #fff; border-color: #111; }
  </style>
</head>
<body>
  <h1>Morse Decoder</h1>
  <div><strong>Decoded text:</strong></div>
  <pre id="decoded">Connecting...</pre>
  <div class="control"><strong>Current symbol buffer:</strong> <span id="buffer">-</span></div>
  <div class="control">
    <button id="clearBtn">Clear</button>
    <button id="soundBtn">Sound: OFF</button>
  </div>
  <div class="control">
    <label for="speedSlider">Keyer speed: <span id="speedValue"></span> ms</label>
    <input id="speedSlider" type="range" min="50" max="250" value="100" step="10">
  </div>
  <div id="paddleContainer" class="control">
    <button id="ditBtn" class="paddle-btn">DIT</button>
    <button id="dahBtn" class="paddle-btn">DAH</button>
  </div>
  <script>
    const decodedEl = document.getElementById('decoded');
    const bufferEl = document.getElementById('buffer');
    const speedValue = document.getElementById('speedValue');
    const speedSlider = document.getElementById('speedSlider');
    const clearBtn = document.getElementById('clearBtn');
    const soundBtn = document.getElementById('soundBtn');

    let soundEnabled = false;
    let audioCtx = null;
    let oscillator = null;
    let gainNode = null;

    soundBtn.addEventListener('click', () => {
      soundEnabled = !soundEnabled;
      soundBtn.textContent = 'Sound: ' + (soundEnabled ? 'ON' : 'OFF');
      if (!soundEnabled) stopTone();
    });

    function startTone() {
      if (!soundEnabled || oscillator) return;
      if (!audioCtx) audioCtx = new AudioContext();
      if (audioCtx.state === 'suspended') audioCtx.resume();
      gainNode = audioCtx.createGain();
      gainNode.gain.setValueAtTime(0.5, audioCtx.currentTime);
      gainNode.connect(audioCtx.destination);
      oscillator = audioCtx.createOscillator();
      oscillator.type = 'sine';
      oscillator.frequency.setValueAtTime(650, audioCtx.currentTime);
      oscillator.connect(gainNode);
      oscillator.start();
    }

    function stopTone() {
      if (!oscillator) return;
      oscillator.stop();
      oscillator.disconnect();
      oscillator = null;
      gainNode.disconnect();
      gainNode = null;
    }

    function updateState(state) {
      decodedEl.textContent = state.decoded || '';
      bufferEl.textContent = state.buffer || '';
      speedValue.textContent = state.speed || speedSlider.value;
      speedSlider.value = state.speed || speedSlider.value;
    }

    const protocol = window.location.protocol === 'https:' ? 'wss://' : 'ws://';
    const socketURL = protocol + window.location.hostname + ':81/';
    const socket = new WebSocket(socketURL);

    socket.onopen = () => {
      console.log('WebSocket connected');
    };

    socket.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data);
        if (message.type === 'update') {
          updateState(message);
        } else if (message.type === 'tone') {
          if (message.active) startTone(); else stopTone();
        }
      } catch (err) {
        console.error('Invalid WS message', err);
      }
    };

    socket.onclose = () => {
      decodedEl.textContent = 'Disconnected. Reload page to reconnect.';
    };

    speedSlider.addEventListener('input', () => {
      speedValue.textContent = speedSlider.value;
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ action: 'speed', dot: Number(speedSlider.value) }));
      }
    });

    clearBtn.addEventListener('click', () => {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ action: 'clear' }));
      }
    });

    let ditDown = false;
    let dahDown = false;

    function sendPaddleState() {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ action: 'paddle', dit: ditDown, dah: dahDown }));
      }
    }

    function bindPaddle(el, setter) {
      el.addEventListener('touchstart',  (e) => { e.preventDefault(); setter(true);  sendPaddleState(); }, { passive: false });
      el.addEventListener('touchend',    (e) => { e.preventDefault(); setter(false); sendPaddleState(); }, { passive: false });
      el.addEventListener('touchcancel', (e) => { e.preventDefault(); setter(false); sendPaddleState(); }, { passive: false });
      el.addEventListener('mousedown',   ()  => { setter(true);  sendPaddleState(); });
      el.addEventListener('mouseup',     ()  => { setter(false); sendPaddleState(); });
      el.addEventListener('mouseleave',  ()  => { setter(false); sendPaddleState(); });
    }

    const ditBtn = document.getElementById('ditBtn');
    const dahBtn = document.getElementById('dahBtn');
    bindPaddle(ditBtn, (v) => { ditDown = v; ditBtn.classList.toggle('active', v); });
    bindPaddle(dahBtn, (v) => { dahDown = v; dahBtn.classList.toggle('active', v); });
  </script>
</body>
</html>)rawliteral";
}

// Encode binary data into Base64 for the WebSocket handshake.
static std::string toBase64(const uint8_t *data, size_t length) {
    size_t outputLength = 0;
    mbedtls_base64_encode(nullptr, 0, &outputLength, data, length);
    std::vector<uint8_t> buffer(outputLength);
    mbedtls_base64_encode(buffer.data(), buffer.size(), &outputLength, data, length);
    return std::string(reinterpret_cast<char *>(buffer.data()), outputLength);
}

// Compute the Sec-WebSocket-Accept header value from the client key.
static std::string computeWebSocketAccept(const std::string &key) {
    std::string concatenated = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha1Digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char *>(concatenated.c_str()), concatenated.length(), sha1Digest);
    return toBase64(sha1Digest, sizeof(sha1Digest));
}

// Escape control characters inside JSON string values before sending them over WS.
static std::string escapeJson(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

// Close the active WebSocket client connection if present.
static void closeWsClient() {
    if (wsClientSocket >= 0) {
        close(wsClientSocket);
        wsClientSocket = -1;
    }
}

// Close the WebSocket listening server socket.
static void closeWsServer() {
    if (wsServerSocket >= 0) {
        close(wsServerSocket);
        wsServerSocket = -1;
    }
}

// Create the raw TCP socket used for incoming WebSocket connections.
static bool initWebSocketServer() {
    wsServerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (wsServerSocket < 0) {
        ESP_LOGE(TAG, "Failed to create WebSocket server socket: %s", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(wsServerSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serverAddr;
    std::memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(WS_PORT);

    if (bind(wsServerSocket, reinterpret_cast<struct sockaddr *>(&serverAddr), sizeof(serverAddr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind WebSocket socket: %s", strerror(errno));
        closeWsServer();
        return false;
    }

    if (listen(wsServerSocket, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen on WebSocket socket: %s", strerror(errno));
        closeWsServer();
        return false;
    }

    int flags = fcntl(wsServerSocket, F_GETFL, 0);
    fcntl(wsServerSocket, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "WebSocket server listening on port %d", WS_PORT);
    return true;
}

static bool beginWebSocketHandshake(int clientSocket) {
    std::string request;
    request.reserve(512);
    char buffer[128];
    int64_t deadline = esp_timer_get_time() + 2000LL * 1000LL;

    while (esp_timer_get_time() < deadline) {
        int received = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (received > 0) {
            request.append(buffer, received);
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        } else if (received == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else {
            ESP_LOGE(TAG, "WebSocket handshake receive failed: %s", strerror(errno));
            return false;
        }
    }

    auto endOfHeaders = request.find("\r\n\r\n");
    if (endOfHeaders == std::string::npos) {
        return false;
    }

    auto keyPos = request.find("Sec-WebSocket-Key: ");
    if (keyPos == std::string::npos) {
        return false;
    }

    auto keyStart = keyPos + strlen("Sec-WebSocket-Key: ");
    auto keyEnd = request.find("\r\n", keyStart);
    if (keyEnd == std::string::npos) {
        return false;
    }

    std::string key = request.substr(keyStart, keyEnd - keyStart);
    std::string acceptKey = computeWebSocketAccept(key);

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";

    if (send(clientSocket, response.c_str(), response.size(), 0) < 0) {
        ESP_LOGE(TAG, "WebSocket handshake send failed: %s", strerror(errno));
        return false;
    }
    return true;
}

static void sendWebSocketFrame(const std::string& payload);
static void broadcastToneState(bool active);

// Turn the status LED on while a symbol is active.
static void setPixelForSymbol(char symbol) {
    gpio_set_level((gpio_num_t)RGB_PIN, 1);
    broadcastToneState(true);
}

// Turn the status LED off when the current symbol ends.
static void clearPixel() {
    gpio_set_level((gpio_num_t)RGB_PIN, 0);
    broadcastToneState(false);
}

// Decode a full Morse code symbol string into a character.
// Returns '?' when the symbol is not recognized.
static char decodeBuffer(const std::string &buffer) {
    auto it = reverseMorse.find(buffer);
    if (it != reverseMorse.end()) {
        return it->second;
    }
    return '?';
}

static void broadcastUpdate();

// Append a decoded character to the output buffer and update the websocket UI.
static void appendDecodedChar(char decoded) {
    if (decoded == '?') {
        return;
    }
    decodedText.push_back(decoded);
    broadcastUpdate();
}

static void appendSpaceIfNeeded() {
    if (decodedText.empty()) {
        return;
    }
    if (!decodedText.empty() && decodedText.back() == ' ') {
        return;
    }
    decodedText.push_back(' ');
    broadcastUpdate();
}

// Handle incoming WebSocket commands from the browser UI.
// Supports clearing the decoded text and adjusting the keyer speed.
static void handleWebSocketMessage(const std::string &message) {
    if (message.find("\"action\":\"clear\"") != std::string::npos) {
        decodedText.clear();
        symbolBuffer.clear();
        broadcastUpdate();
    } else if (message.find("\"action\":\"speed\"") != std::string::npos) {
        auto dotIndex = message.find("\"dot\":");
        if (dotIndex != std::string::npos) {
            int value = std::stoi(message.substr(dotIndex + 6));
            if (value < SPEED_MIN) value = SPEED_MIN;
            if (value > SPEED_MAX) value = SPEED_MAX;
            dotTime = value;
            broadcastUpdate();
        }
    } else if (message.find("\"action\":\"paddle\"") != std::string::npos) {
        virtualDitPressed = message.find("\"dit\":true") != std::string::npos;
        virtualDahPressed = message.find("\"dah\":true") != std::string::npos;
    }
}

static void sendWebSocketFrame(const std::string& payload) {
    if (wsClientSocket < 0) return;

    std::vector<uint8_t> frame;
    frame.reserve(10 + payload.size());

    frame.push_back(0x81);
    size_t length = payload.size();
    if (length <= 125) {
        frame.push_back(static_cast<uint8_t>(length));
    } else if (length <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back((length >> 8) & 0xFF);
        frame.push_back(length & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 0; i < 8; i++)
            frame.push_back((length >> (56 - 8 * i)) & 0xFF);
    }
    frame.insert(frame.end(),
                 reinterpret_cast<const uint8_t*>(payload.data()),
                 reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());

    ssize_t sent = send(wsClientSocket, frame.data(), frame.size(), 0);
    if (sent != static_cast<ssize_t>(frame.size())) {
        closeWsClient();
    }
}

static void broadcastUpdate() {
    if (wsClientSocket < 0) return;
    std::string payload = "{\"type\":\"update\",\"decoded\":\"";
    payload += escapeJson(decodedText);
    payload += "\",\"buffer\":\"";
    payload += escapeJson(symbolBuffer);
    payload += "\",\"speed\":" + std::to_string(dotTime) + "}";
    sendWebSocketFrame(payload);
}

static void broadcastToneState(bool active) {
    if (wsClientSocket < 0) return;
    std::string payload = "{\"type\":\"tone\",\"active\":";
    payload += active ? "true" : "false";
    payload += "}";
    sendWebSocketFrame(payload);
}

// Process a single WebSocket frame if it is available from the client.
// This reads the masked payload, decodes it, and forwards the data.
static bool processWebSocketFrame() {
    if (wsClientSocket < 0) {
        return false;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(wsClientSocket, &readSet);
    struct timeval timeout = {0, 0};
    int ready = select(wsClientSocket + 1, &readSet, NULL, NULL, &timeout);
    if (ready <= 0) {
        return false;
    }

    uint8_t header[2];
    int received = recv(wsClientSocket, header, 2, MSG_DONTWAIT);
    if (received <= 0) {
        if (received == 0 || (received < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
            closeWsClient();
        }
        return false;
    }
    if (received != 2) {
        return false;
    }

    uint8_t opcode = header[0] & 0x0F;
    uint8_t masked = header[1] & 0x80;
    uint64_t payloadLength = header[1] & 0x7F;

    if (opcode == 0x8) {
        closeWsClient();
        return false;
    }
    if (!masked) {
        closeWsClient();
        return false;
    }

    if (payloadLength == 126) {
        uint8_t ext[2];
        if (recv(wsClientSocket, ext, 2, MSG_WAITALL) != 2) {
            return false;
        }
        payloadLength = (ext[0] << 8) | ext[1];
    } else if (payloadLength == 127) {
        uint8_t ext[8];
        if (recv(wsClientSocket, ext, 8, MSG_WAITALL) != 8) {
            return false;
        }
        payloadLength = 0;
        for (int i = 0; i < 8; i++) {
            payloadLength = (payloadLength << 8) | ext[i];
        }
    }

    uint8_t mask[4];
    if (recv(wsClientSocket, mask, 4, MSG_WAITALL) != 4) {
        return false;
    }

    std::string payload;
    payload.resize(payloadLength);
    if (recv(wsClientSocket, payload.data(), payloadLength, MSG_WAITALL) != static_cast<int>(payloadLength)) {
        return false;
    }

    for (size_t i = 0; i < payloadLength; i++) {
        payload[i] ^= mask[i % 4];
    }
    handleWebSocketMessage(payload);
    return true;
}

// Accept a new WebSocket client if a connection is pending.
// Only one live WS client is allowed at a time.
static void maybeAcceptWebSocketClient() {
    if (wsClientSocket >= 0) {
        return;
    }

    if (wsServerSocket < 0) {
        return;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(wsServerSocket, &readSet);
    struct timeval timeout = {0, 0};
    int ready = select(wsServerSocket + 1, &readSet, NULL, NULL, &timeout);
    if (ready <= 0) {
        return;
    }

    int client = accept(wsServerSocket, NULL, NULL);
    if (client < 0) {
        return;
    }
    int flags = fcntl(client, F_GETFL, 0);
    fcntl(client, F_SETFL, flags | O_NONBLOCK);
    int nodelay = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (!beginWebSocketHandshake(client)) {
        close(client);
        return;
    }
    wsClientSocket = client;
    broadcastUpdate();
}

static void checkForPauseActions();

// Read the paddle inputs and start/stop Morse symbols.
// The paddles are pulled up, so a pressed switch reads low.
static void processPaddles() {
    bool ditPressed = gpio_get_level((gpio_num_t)DIT_PIN) == 0 || virtualDitPressed;
    bool dahPressed = gpio_get_level((gpio_num_t)DAH_PIN) == 0 || virtualDahPressed;
    uint64_t now = millis64();

    if (symbolActive) {
        if (now >= symbolEndMillis) {
            symbolActive = false;
            clearPixel();
            lastSymbolEndMillis = now;
            nextSymbolMillis = now + symbolPause();
            if (!ditPressed && !dahPressed) {
                return;
            }
        } else {
            return;
        }
    }

    if (now < nextSymbolMillis) {
        return;
    }

    if (ditPressed && dahPressed) {
        char symbol = ditNext ? '.' : '-';
        ditNext = !ditNext;
        symbolBuffer.push_back(symbol);
        setPixelForSymbol(symbol);
        symbolActive = true;
        symbolEndMillis = now + (symbol == '.' ? dotTime : dashTime());
        return;
    }

    if (ditPressed) {
        symbolBuffer.push_back('.');
        setPixelForSymbol('.');
        symbolActive = true;
        symbolEndMillis = now + dotTime;
        ditNext = false;
        return;
    }

    if (dahPressed) {
        symbolBuffer.push_back('-');
        setPixelForSymbol('-');
        symbolActive = true;
        symbolEndMillis = now + dashTime();
        ditNext = true;
        return;
    }
}

// Check for symbol/letter/word pauses and decode completed Morse symbols.
static void checkForPauseActions() {
    uint64_t now = millis64();
    if (!symbolActive) {
        if (!symbolBuffer.empty() && now - lastSymbolEndMillis >= static_cast<uint64_t>(letterPause())) {
            char decoded = decodeBuffer(symbolBuffer);
            symbolBuffer.clear();
            if (decoded != '?') {
                appendDecodedChar(decoded);
            }
            lastSymbolEndMillis = now;
        } else if (symbolBuffer.empty() && now - lastSymbolEndMillis >= static_cast<uint64_t>(wordPause())) {
            appendSpaceIfNeeded();
            lastSymbolEndMillis = now;
        }
    }
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    std::string page = htmlPage();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), page.size());
}

// Start the HTTP server and register the root page handler.
// The station page contains the live WebSocket UI.
static void startHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&httpServer, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t indexUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(httpServer, &indexUri);
}

// WiFi event callback to manage reconnection and notify when an IP is obtained.
static void wifiEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected from WiFi, retrying...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Connected, IP: %s", ip4addr_ntoa(reinterpret_cast<const ip4_addr_t *>(&event->ip_info.ip)));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi STA mode and connect to the configured network.
// Also initializes NVS and the network stack required by ESP-IDF.
static void connectWiFi() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    if (std::strlen(WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "WIFI_SSID is not configured. Run idf.py menuconfig and set WiFi SSID.");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID '%s'...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "Failed to connect to WiFi within timeout");
    }
}

extern "C" void app_main() {
    // Create a FreeRTOS event group for WiFi connection state.
    wifi_event_group = xEventGroupCreate();

    // Configure paddle input pins with pull-up resistors.
    gpio_config_t inputConfig = {};
    inputConfig.intr_type = GPIO_INTR_DISABLE;
    inputConfig.mode = GPIO_MODE_INPUT;
    inputConfig.pin_bit_mask = (1ULL << DIT_PIN) | (1ULL << DAH_PIN);
    inputConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
    inputConfig.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&inputConfig));

    // Configure the RGB status LED pin as output only.
    gpio_config_t outputConfig = {};
    outputConfig.intr_type = GPIO_INTR_DISABLE;
    outputConfig.mode = GPIO_MODE_OUTPUT;
    outputConfig.pin_bit_mask = (1ULL << RGB_PIN);
    outputConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
    outputConfig.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&outputConfig));
    gpio_set_level((gpio_num_t)RGB_PIN, 0);

    // Initialize network, servers, and reverse lookup table.
    connectWiFi();
    startHttpServer();
    initWebSocketServer();

    reverseMorse.clear();
    for (const auto &entry : morseCode) {
        reverseMorse[entry.second] = entry.first;
    }

    lastSymbolEndMillis = millis64();
    ESP_LOGI(TAG, "HTTP and WebSocket servers started.");

    // Main loop: accept WS clients, process paddles, decode pauses,
    // and process any pending WebSocket messages.
    while (true) {
        maybeAcceptWebSocketClient();
        processPaddles();
        checkForPauseActions();
        processWebSocketFrame();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
