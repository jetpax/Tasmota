/*
  be_wsserver_lib.c - WebSocket server support for Berry using ESP-IDF HTTP server

  Copyright (C) 2025 Jonathan E. Peace

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_BERRY_WSSERVER

// Standard C includes first
#include <stdlib.h>
#include <stdbool.h> // For bool
#include <string.h>
#include <stdint.h>  // For int64_t, uint8_t
#include <stddef.h>  // For size_t

#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#endif

// Berry includes
#include "be_vm.h"       // For bvm
#include "be_object.h"   // For bvalue
#include "be_constobj.h"
#include "be_mapping.h"
#include "be_exec.h"
#include "be_string.h"
#include "be_gc.h"
#include "be_debug.h"
#include "be_map.h"
#include "be_list.h"
#include "be_module.h"

// ESP-IDF includes
#include "esp_http_server.h" // For httpd_handle_t
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

// Socket-related includes
#include <sys/socket.h>
#include <netinet/in.h>

static const char *TAG = "WSS";

// Max number of concurrent clients
#define MAX_WS_CLIENTS 5

// Define event types
#define WSSERVER_EVENT_CONNECT    0
#define WSSERVER_EVENT_DISCONNECT 1
#define WSSERVER_EVENT_MESSAGE    2

// Message types for queue - match those in be_httpserver_lib.c
#define HTTP_MSG_WEBSOCKET 1
#define HTTP_MSG_FILE 2
#define HTTP_MSG_WEB 3

// Declarations for the HTTP server functions
extern httpd_handle_t be_httpserver_get_handle(void);
extern bool httpserver_queue_message(int msg_type, int client_id, 
                                const void *data, size_t data_len, void *user_data);
extern void httpserver_register_external_close_cb(void (*func)(int sockfd));


extern void be_webrepl_handle_input(bvm *vm, int client_id, const char* code, size_t len);


typedef enum {
    WS_STATE_INIT,       // Just connected, before prompt/trigger check
    WS_STATE_PASSWORD,   // Sent prompt, awaiting password
    WS_STATE_REPL,       // Password OK, processing WebREPL commands
    WS_STATE_NORMAL_APP  // Determined not to be WebREPL
} ws_client_state_t;

// Client tracking structure
typedef struct {
    int sockfd;
    bool active;
    int64_t last_activity;  // Timestamp of any client activity in milliseconds
    ws_client_state_t state; // Client state for WebREPL/Normal App
    char command_buffer[256]; // Buffer for accumulating WebREPL commands
    uint8_t command_len;     // Current length of command in buffer
    bool raw_repl_mode;      // Flag for RAW WebREPL mode
} ws_client_t;

// Callback structure to properly store Berry callbacks
typedef struct be_wsserver_callback_t {
  bvm *vm;                // VM instance
  bvalue func;            // Berry function value
  bool active;            // Whether this callback is active
} be_wsserver_callback_t;

// Forward declarations for all functions to prevent compiler errors
static void init_clients(void);
static int find_free_client_slot(void);
static int add_client(int sockfd);
static int find_client_by_fd(int sockfd);
static void remove_client(int slot);
static void handle_client_disconnect(int client_slot);
static void callBerryWsDispatcher(bvm *vm, int client_id, const char *event_name, const char *payload, int arg_count);
static void check_clients(void);
// Helper functions for server start
static bool parse_ws_start_parameters(bvm *vm, const char **path);
static bool register_ws_handler(const char* path);
static void start_ping_timer(void);

// Globals
httpd_handle_t ws_server = NULL;
int g_stream_sockfd = -1;

static bool wsserver_running = false;
static uint32_t ping_interval_s;  // Ping interval in seconds
static uint32_t ping_timeout_s;   // Activity timeout in seconds
static esp_timer_handle_t ping_timer = NULL;

// Client status and context
ws_client_t ws_clients[MAX_WS_CLIENTS] = {0};

// Storage for Berry callback functions
static be_wsserver_callback_t wsserver_callbacks[3]; // CONNECT, DISCONNECT, MESSAGE

// Forward declaration for processing WebSocket messages in main task context
void be_wsserver_handle_message(bvm *vm, int client_id, const char* data, size_t len);


// The exact trigger string for WebREPL password prompt
#define WEBREPL_PASSWORD_PROMPT "Password: "
// Placeholder for the actual WebREPL password - needs proper implementation/configuration
static const char* webrepl_password = "password"; // CHANGE THIS!

// Helper to send prompts/frames directly from C
void send_ws_text_frame(int sockfd, const char* text) {
    if (sockfd < 0 || !ws_server || !text) {
        ESP_LOGE(TAG, "Invalid parameters in send_ws_text_frame: sockfd=%d, ws_server=%p, text=%p", 
                 sockfd, ws_server, text);
        return;
    }
    
    // Log to help debug message routing
    ESP_LOGI(TAG, "Sending frame to socket %d: '%s'", sockfd, text);
    
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.payload = (uint8_t*)text;
    frame.len = strlen(text);
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send direct frame to sockfd %d: %s (%d)", 
                 sockfd, esp_err_to_name(ret), ret);
    } else {
        ESP_LOGI(TAG, "Successfully sent frame to socket %d", sockfd);
    }
}

// Check if a client is valid and connected
bool is_client_valid(int client_id) {
    // Check for valid client ID range
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid client ID: %d", client_id);
        return false;
    }
    // Check if client is active and has a valid socket
    if (!ws_clients[client_id].active || ws_clients[client_id].sockfd < 0) {
        ESP_LOGE(TAG, "Client %d is not active (active=%d, sockfd=%d)",
                client_id, ws_clients[client_id].active, ws_clients[client_id].sockfd);
        return false;
    }
    
    return true;
}


void send_ws_text_frame_to_client(int client_id, const char* text) {
    if (!is_client_valid(client_id) || !text) {
        ESP_LOGE(TAG, "Invalid parameters in send_ws_text_frame_to_client: client_id=%d, valid=%d, text=%p", 
                 client_id, is_client_valid(client_id), text);
        return;
    }
    
    int sockfd = ws_clients[client_id].sockfd;
    
    ESP_LOGI(TAG, "Sending frame to client %d (socket %d): '%s'", client_id, sockfd, text);
    
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.payload = (uint8_t*)text;
    frame.len = strlen(text);
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send frame to client %d (socket %d): %s (%d)", 
                 client_id, sockfd, esp_err_to_name(ret), ret);
    } else {
        ESP_LOGI(TAG, "Successfully sent frame to client %d (socket %d)", client_id, sockfd);
    }
}

// Call a Berry callback function registered by wsserver.on()
// CONTEXT: Main Tasmota Task (Berry VM Context)
// This function is only called from the main task when processing queued events
static void callBerryWsDispatcher(bvm *vm, int client_id, const char *event_name, const char *payload, int arg_count) {
    if (!vm) {
        ESP_LOGE(TAG, "Berry VM is NULL in callBerryWsDispatcher");
        return;
    }
    
    // Ensure parameters are valid
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS || !event_name) {
        ESP_LOGE(TAG, "Invalid parameters in callBerryWsDispatcher");
        return;
    }
    
    ESP_LOGI(TAG, "Calling Berry callback for event '%s', client %d, payload: %s", 
             event_name, client_id, payload ? payload : "nil");
    
    // Map event name to event type
    int event_type = -1;
    if (strcmp(event_name, "connect") == 0) {
        event_type = WSSERVER_EVENT_CONNECT;
    } else if (strcmp(event_name, "disconnect") == 0) {
        event_type = WSSERVER_EVENT_DISCONNECT;
    } else if (strcmp(event_name, "message") == 0) {
        event_type = WSSERVER_EVENT_MESSAGE;
    } else {
        ESP_LOGE(TAG, "Unknown event type: %s", event_name);
        return;
    }
    
    // Check if we have a registered callback for this event type
    if (!wsserver_callbacks[event_type].active) {
        ESP_LOGI(TAG, "No callback registered for event type %d (%s)", event_type, event_name);
        return;
    }
    
    ESP_LOGI(TAG, "Using registered callback for event type %d (%s)", event_type, event_name);
    
    // Save initial stack position for diagnostic logging
    int initial_top = be_top(vm);
    
    // Push the callback function onto the stack
    bvalue *reg = vm->top;
    var_setval(reg, &wsserver_callbacks[event_type].func);
    be_incrtop(vm);
    
    
    // Push the client ID first, event name second, payload third (if applicable)
    // This matches Berry function signature: function(client, event, message)
    be_pushint(vm, client_id);  // Push client ID argument
    be_pushstring(vm, event_name);  // Push event name argument
    
    // Push payload argument for message events (optional)
    if (arg_count > 2 && payload) {
        be_pushstring(vm, payload);
    }
    
    // Call the callback function with the appropriate number of arguments
    int call_result = be_pcall(vm, arg_count);
    
    // Handle the Berry call result
    if (call_result != BE_OK) {
        ESP_LOGE(TAG, "Berry callback error for event '%s': %s", 
                 event_name, be_tostring(vm, -1));
        
        be_error_pop_all(vm);
    } else {
        // Pop all arguments 
        be_pop(vm, arg_count);
                
        // Pop the function
        be_pop(vm, 1);
        
        if (be_top(vm) != initial_top) {
            ESP_LOGE(TAG, "[dispatcher-ERROR] Stack imbalance detected: %d (expected %d)", be_top(vm), initial_top);
        }
    }
}

// Process a WebSocket message in the main task context
// CONTEXT: Main Tasmota Task (Berry VM Context)
// This function processes messages from the queue in the main task
// It now routes messages based on client state
void be_wsserver_handle_message(bvm *vm, int client_id, const char* data, size_t len) {
    if (!vm) {
        ESP_LOGE(TAG, "Berry VM is NULL in be_wsserver_handle_message");
        return;
    }

    if (!data) {
        // Connect or disconnect event (no data)
        if (ws_clients[client_id].active) { // Use ws_clients state, not is_client_valid to handle disconnects correctly
            ESP_LOGI(TAG, "Handling WebSocket connect event in main task: client=%d", client_id);
            callBerryWsDispatcher(vm, client_id, "connect", NULL, 2);
        } else {
            ESP_LOGI(TAG, "Handling WebSocket disconnect event in main task: client=%d", client_id);
            callBerryWsDispatcher(vm, client_id, "disconnect", NULL, 2);
            
            // ===> ADDED: Stop stream capture if this client was capturing
            if (g_stream_sockfd == ws_clients[client_id].sockfd) {
                g_stream_sockfd = -1;
                 ESP_LOGI(TAG, "Stopped print capture (client %d disconnected)", client_id);
            }
            
            // Reset client state on disconnect processing in main task
            ESP_LOGW(TAG, "Main Task Handler: Resetting state for disconnected client %d (current state: %d, sockfd: %d)", 
                     client_id, ws_clients[client_id].state, ws_clients[client_id].sockfd);
                     
            ws_clients[client_id].sockfd = -1;
            ws_clients[client_id].state = WS_STATE_INIT; // Reset state
            ws_clients[client_id].command_len = 0; // Also reset command buffer
            ws_clients[client_id].command_buffer[0] = '\0';
        }
    } else {
        // Normal message event with data
        // Check client state to route the message
        ws_client_state_t state = ws_clients[client_id].state;
        int sockfd = ws_clients[client_id].sockfd;

        ESP_LOGI(TAG, "Handling WebSocket message event in main task: client=%d, state=%d, len=%d", 
                client_id, state, (int)len);

        if (state == WS_STATE_REPL) {
            // REPL command handling is now delegated to be_webrepl_handle_input
            ESP_LOGD(TAG, "Routing message for client %d (socket %d, len %d) to WebREPL handler", 
                     client_id, sockfd, (int)len);
            be_webrepl_handle_input(vm, client_id, data, len);
        } else if (state == WS_STATE_NORMAL_APP) {
            // Route to normal Berry message callback
            callBerryWsDispatcher(vm, client_id, "message", data, 3);
        } else if (state == WS_STATE_INIT) {
            // Client sent data before authentication was completed by Berry.
            // Pass it to the Berry message handler to decide what to do 
            // (e.g., ignore, buffer, treat as password attempt if prompt was sent).
            // DO NOT change state here; Berry controls the WebREPL transition.
            ESP_LOGI(TAG, "Message received from client %d in INIT state; passing to Berry callback.", client_id);
            callBerryWsDispatcher(vm, client_id, "message", data, 3);
        }
    }
}

// WebSocket Event Handler
// CONTEXT: ESP-IDF HTTP Server Task
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Handshake handling
        ESP_LOGI(TAG, "WebSocket handshake received");
        int sockfd = httpd_req_to_sockfd(req);        
        int client_slot = add_client(sockfd); 
        
        if (client_slot >= 0) {
            ESP_LOGI(TAG, "Client %d connected, socket fd: %d", client_slot, sockfd);
            
            // Queue connect event for processing in main task
            // TRANSITION: ESP-IDF HTTP Server Task → Main Tasmota Task
            // Use NULL as data to indicate this is a connect event
            if (!httpserver_queue_message(HTTP_MSG_WEBSOCKET, client_slot, NULL, 0, NULL)) {
                ws_clients[client_slot].sockfd = -1;
                ESP_LOGE(TAG, "WS Q connect failed!");
            }
        }
        
        // Return success for the handshake
        return ESP_OK;
    }
    
    ESP_LOGD(TAG, "WebSocket frame received");
    int sockfd = httpd_req_to_sockfd(req);
    int client_slot = find_client_by_fd(sockfd);
    
    if (client_slot < 0) {
        ESP_LOGE(TAG, "Received frame from unknown client (socket: %d)", sockfd);
        // Clean close sequence
        httpd_ws_frame_t close_frame = {
            .type = HTTPD_WS_TYPE_CLOSE,
            .len = 0,
            .payload = NULL
        };
        httpd_ws_send_frame(req, &close_frame);
        return ESP_FAIL;
    }
    
    // Update activity time for ANY frame
    ws_clients[client_slot].last_activity = esp_timer_get_time() / 1000; // Use ms
    
    // Standard ESP-IDF WebSocket frame reception
    httpd_ws_frame_t ws_pkt = {0};
    uint8_t *buf = NULL;

    ws_pkt.type = HTTPD_WS_TYPE_TEXT;  // Default type

    // Get the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (len query) failed with %d", ret);
        // Don't free buf here, it's not allocated yet
        handle_client_disconnect(client_slot); // Disconnect on receive error
        return ret;
    }
    
    ESP_LOGD(TAG, "Frame len is %d, type is %d", ws_pkt.len, ws_pkt.type);
    
    // Handle control frames immediately
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        // ESP_LOGI(TAG, "Received PONG from client %d", client_slot);
        return ESP_OK;
    } 
    
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Received CLOSE from client %d", client_slot);
        handle_client_disconnect(client_slot);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // ESP_LOGI(TAG, "Received PING from client %d", client_slot);
        httpd_ws_frame_t pong = {0};
        pong.type = HTTPD_WS_TYPE_PONG;
        pong.len = 0;
        ret = httpd_ws_send_frame(req, &pong);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send PONG: %d", ret);
        }
        return ESP_OK;
    }
    
    // Not a control frame, so allocate memory for the message payload
    // Only allocate if length > 0
    if (ws_pkt.len > 0) {
        buf = calloc(1, ws_pkt.len + 1); 
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory (%d bytes) for WS message", ws_pkt.len + 1);
            // Don't disconnect here, could be temp OOM, ESP-IDF might recover. Return error.
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;  
        // Receive the actual message payload
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame (payload) failed with %d", ret);
            free(buf);
            handle_client_disconnect(client_slot); // Disconnect on receive error
            return ret;
        }
    } else {
        // Empty data frame, nothing to process further
        // Still update activity time (done above)
        ESP_LOGD(TAG, "Received empty data frame from client %d", client_slot);
        return ESP_OK; // Successfully handled empty frame
    }

    // new ws message received successfully
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        const char* text_payload = (const char*)buf;
        ESP_LOGI(TAG, "Client %d (State:%d) Received TEXT (len %d): '%s'", client_slot, ws_clients[client_slot].state, ws_pkt.len, text_payload ? text_payload : "");
                
        // Queue the message - httpserver_queue_message will make its own copy
        if (!httpserver_queue_message(HTTP_MSG_WEBSOCKET, client_slot, text_payload, ws_pkt.len, NULL)) {
            ESP_LOGE(TAG, "WS Q message failed!");
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        ESP_LOGI(TAG, "Client %d (State:%d) Received BINARY, len=%d", client_slot, ws_clients[client_slot].state, ws_pkt.len);
        // TODO: Handle binary messages
    }
    
    // Free the allocated buffer
    free(buf);
    
    return ESP_OK;
}

// WebSocket socket cleanup callback for HTTP server
// CONTEXT: ESP-IDF HTTP Server Task
// This gets called by the HTTP server when any socket closes
static void wsserver_socket_cleanup_cb(int sockfd) {
    // Find which client this socket belongs to
    int client_slot = find_client_by_fd(sockfd);
    
    if (client_slot >= 0) {
        ESP_LOGI(TAG, "Socket %d closed, corresponds to WS client %d", sockfd, client_slot);
        // Handle client disconnection
        handle_client_disconnect(client_slot);
    }
}

// Handle client disconnection
// CONTEXT: Can be called from HTTP Server Task or other contexts (e.g., timer)
static void handle_client_disconnect(int client_slot) {
    if (client_slot < 0 || client_slot >= MAX_WS_CLIENTS || !ws_clients[client_slot].active) {
        // Already inactive or invalid slot
        return;
    }

    ESP_LOGW(TAG, "Disconnect Handler: Entered for client %d (socket %d, current state %d)", 
             client_slot, ws_clients[client_slot].sockfd, ws_clients[client_slot].state);

    int sockfd = ws_clients[client_slot].sockfd;

    ESP_LOGI(TAG, "WS Client %d disconnected (state was %d)", client_slot, ws_clients[client_slot].state);
    
    // Mark client as inactive BEFORE queuing the event
    ws_clients[client_slot].active = false;

    // Queue the disconnect event for VM context processing
    // The VM context handler will reset state and call callbacks
    if (!httpserver_queue_message(HTTP_MSG_WEBSOCKET, client_slot, NULL, 0, NULL)) {
        ESP_LOGE(TAG, "WS Q disconnect failed!");
        // Ensure cleanup happens if queuing fails
        ws_clients[client_slot].sockfd = -1;
        ws_clients[client_slot].state = WS_STATE_INIT;
        ws_clients[client_slot].command_len = 0; // Also reset command buffer
        ws_clients[client_slot].command_buffer[0] = '\0';
    }
}

// Timer callback for pinging clients
static void ws_ping_timer_callback(void* arg) {
    // Skip if server is not running or pings disabled
    if (!wsserver_running || !ws_server || ping_interval_s == 0) return;
    
    int64_t now = esp_timer_get_time();
    
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active) {
            // Check for inactivity timeout if enabled
            if (ping_timeout_s > 0) {
                // Calculate inactivity time based on milliseconds
                int64_t inactivity_s = ((now / 1000) - ws_clients[i].last_activity) / 1000; // now/1000 for ms, then /1000 for seconds
                
                if (inactivity_s > ping_timeout_s) {
                    ESP_LOGI(TAG, "Client %d timed out (inactive for %d seconds)", 
                             i, (int)inactivity_s);
                    
                    // Trigger session close
                    handle_client_disconnect(i);
                    continue;
                }
            }
            
            // Send ping
            httpd_ws_frame_t ping = {0};
            ping.type = HTTPD_WS_TYPE_PING;
            
            ESP_LOGI(TAG, "Sending PING to client %d", i);
            esp_err_t ret = httpd_ws_send_frame_async(ws_server, ws_clients[i].sockfd, &ping);
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send PING to client %d: %d", i, ret);
                
                // Trigger session close
                handle_client_disconnect(i);
            }
        }
    }
}

// Client Management Functions
static void init_clients(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        ws_clients[i].active = false;
        ws_clients[i].sockfd = -1;
    }
}

static int find_free_client_slot(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!ws_clients[i].active) {
            return i;
        }
    }
    return -1; // No free slots
}

static int add_client(int sockfd) {
    int slot = find_free_client_slot();
    if (slot >= 0) {
        ws_clients[slot].sockfd = sockfd;
        ws_clients[slot].active = true;
        ws_clients[slot].last_activity = esp_timer_get_time() / 1000; // Use ms
        ws_clients[slot].state = WS_STATE_INIT; // Start in INIT state
        ws_clients[slot].command_len = 0;      // Reset command buffer length
        ws_clients[slot].command_buffer[0] = '\0'; // Clear command buffer
        ws_clients[slot].raw_repl_mode = false; // Reset raw_repl_mode
        ESP_LOGI(TAG, "Added client %d (socket %d), state INIT.", slot, sockfd);
        return slot;
    }
    ESP_LOGE(TAG, "No free client slots available");
    return -1;
}

static int find_client_by_fd(int sockfd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active && ws_clients[i].sockfd == sockfd) {
            return i;
        }
    }
    return -1;
}

static void remove_client(int slot) {
    if (slot >= 0 && slot < MAX_WS_CLIENTS) {
        ESP_LOGI(TAG, "Removing client %d with socket %d", slot, ws_clients[slot].sockfd);
        ws_clients[slot].active = false;
        ws_clients[slot].sockfd = -1;
    }
}

// Helper functions for server start
static bool parse_ws_start_parameters(bvm *vm, const char **path) {
    // Check if we have at least the path parameter
    if (be_top(vm) < 1 || !be_isstring(vm, 1)) {
        ESP_LOGE(TAG, "Missing or invalid path parameter");
        return false;
    }
    
    // Get the path parameter
    *path = be_tostring(vm, 1);
    ESP_LOGI(TAG, "WebSocket server path: '%s'", *path);
    
    // Try to get the handle from the HTTP server
    httpd_handle_t handle = be_httpserver_get_handle();
    if (!handle) {
        ESP_LOGE(TAG, "HTTP server not started. Start HTTP server before WebSocket server");
        return false;
    }
    
    // Assign to global ws_server variable
    ws_server = handle;
    ESP_LOGI(TAG, "Using HTTP server handle: %p", ws_server);
    
    // Get ping parameters - now at positions 2 and 3 (no server parameter)
    ping_interval_s = 5;  // Default: 5 seconds
    ping_timeout_s = 10;  // Default: 10 seconds
    
    // Check for ping interval parameter (position 2)
    if (be_top(vm) >= 2 && be_isint(vm, 2)) {
        ping_interval_s = be_toint(vm, 2);
        
        // Check for ping timeout parameter (position 3)
        if (be_top(vm) >= 3 && be_isint(vm, 3)) {
            ping_timeout_s = be_toint(vm, 3);
        }
        
        ESP_LOGI(TAG, "WebSocket ping configured: interval=%ds, timeout=%ds", 
                 ping_interval_s, ping_timeout_s);
    }
    
    return true;
}

static bool register_ws_handler(const char* path) {
    // Register URI handler for WebSocket endpoint
    httpd_uri_t ws_uri = {
        .uri        = path,
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true  // Allow ESP-IDF to properly handle control frames
    };
    
    ESP_LOGI(TAG, "Registering WebSocket handler for '%s'", path);
    esp_err_t ret = httpd_register_uri_handler(ws_server, &ws_uri);
    
    if (ret != ESP_OK) {
        // Check if the handler already exists (which is acceptable)
        if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "WebSocket handler for '%s' already exists, continuing", path);
            return true;
        }
        
        ESP_LOGE(TAG, "Failed to register URI handler: %d (0x%x)", ret, ret);
    
        wsserver_running = false;
        return false;
    }
    
    return true;
}

static void start_ping_timer(void) {
    if (ping_interval_s > 0) {
        ESP_LOGI(TAG, "Starting ping timer with interval %ds", ping_interval_s);
        esp_timer_create_args_t timer_args = {
            .callback = ws_ping_timer_callback,
            .name = "ws_ping"
        };
        esp_timer_create(&timer_args, &ping_timer);
        esp_timer_start_periodic(ping_timer, ping_interval_s * 1000000); // seconds to microseconds
    }
}

// Berry Interface Functions
static int w_wsserver_start(bvm *vm) {
    // Handle the case when server is already running - just update parameters
    if (wsserver_running) {
        ESP_LOGI(TAG, "WebSocket server already running");
        
        if (be_top(vm) >= 1 && be_isstring(vm, 1)) {
            // Get optional ping parameters and update them (positions 2 and 3)
            if (be_top(vm) >= 2 && be_isint(vm, 2)) {
                ping_interval_s = be_toint(vm, 2);
                ESP_LOGD(TAG, "Updated ping interval to %d seconds", ping_interval_s);
            }
            
            if (be_top(vm) >= 3 && be_isint(vm, 3)) {
                ping_timeout_s = be_toint(vm, 3);
                ESP_LOGD(TAG, "Updated ping timeout to %d seconds", ping_timeout_s);
            }
        }
        
        be_pushbool(vm, true);
        be_return(vm);
    }
    
    // Initialize a new server
    ESP_LOGI(TAG, "Initializing WebSocket server");
    
    // Parse parameters
    const char* path = NULL;
    
    if (!parse_ws_start_parameters(vm, &path)) {
        be_pushbool(vm, false);
        be_return(vm);
    }
    
    // Initialize client tracking
    init_clients();
    
    // Register socket close callback with HTTP server
    ESP_LOGI(TAG, "Registering socket close callback with HTTP server");
    httpserver_register_external_close_cb(wsserver_socket_cleanup_cb);
    
    // Register the WebSocket handler
    if (!register_ws_handler(path)) {
        be_pushbool(vm, false);
        be_return(vm);
    }
    
    // Start the ping timer if needed
    start_ping_timer();
    
    // Mark server as running
    wsserver_running = true;
    ESP_LOGI(TAG, "WebSocket server started successfully");
    
    be_pushbool(vm, true);
    be_return(vm);
}


static int w_wsserver_send(bvm *vm) {
    int initial_top = be_top(vm);
    
    // First validate parameters
    if (be_top(vm) < 2 || !be_isint(vm, 1)) {
        ESP_LOGE(TAG, "Invalid parameters for send");
        be_pushbool(vm, false);
        be_return (vm);
    }
    
    int client_slot = be_toint(vm, 1);
    size_t len = 0;
    const char* data = NULL;
    uint8_t *data_copy = NULL;
    
    // First check if client is valid before processing the message
    if (!is_client_valid(client_slot)) {
        be_pushbool(vm, false);
        be_return (vm);
    }

    // Check if we're dealing with a string or bytes object
    bool is_valid_data = false;
    bool is_bytes = false;
    
    if (be_isstring(vm, 2)) {
        is_valid_data = true;
    } else if (be_isbytes(vm, 2)) {
        is_valid_data = true;
        is_bytes = true;
    }
    
    if (!is_valid_data) {
        ESP_LOGE(TAG, "Data must be a string or bytes object");
        be_pushbool(vm, false);
        be_return (vm);
    }
    
    // Get data and length based on type
    if (is_bytes) {
        // It's a bytes object - get raw length
        data = be_tobytes(vm, 2, &len);
        ESP_LOGD(TAG, "Sending bytes object from Berry, length: %d", (int)len);
    } else {
        // For normal strings, get the length from Berry
        len = be_strlen(vm, 2);
        data = be_tostring(vm, 2);
        ESP_LOGD(TAG, "Sending string '%s' from Berry, length: %d", data, (int)len);
    }
    
    if (len == 0 || data == NULL) {
        ESP_LOGE(TAG, "Invalid data (empty or NULL)");
        be_pushbool(vm, false);
        be_return (vm);
    }
    
    // Check for optional binary flag parameter
    bool is_binary = false;
    if (be_top(vm) >= 3 && be_isbool(vm, 3)) {
        is_binary = be_tobool(vm, 3);
    }
    
    httpd_ws_frame_t ws_pkt = {0};
    
    // Create a copy of the data to ensure it remains valid during async operation
    data_copy = (uint8_t *)malloc(len);
    if (data_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for data copy of size %d", len);
        be_pushbool(vm, false);
        be_return (vm);
    }
    
    // Copy the data
    memcpy(data_copy, data, len);
    
    ws_pkt.payload = data_copy;
    ws_pkt.len = len;
    ws_pkt.type = is_binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
    ws_pkt.final = true;
    ws_pkt.fragmented = false;
    
    // Get the client's socket
    int sockfd = ws_clients[client_slot].sockfd;
    
    // Track last send attempt for this client
    int64_t now = esp_timer_get_time();
    ws_clients[client_slot].last_activity = now;
    
    // Send the frame asynchronously
    esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &ws_pkt);
    
    // Free the data copy regardless of send result
    free(data_copy);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message to client %d: %d (0x%x)", 
                client_slot, ret, ret);
        
        // Check if this is a fatal error indicating disconnection
        if (ret == ESP_ERR_HTTPD_INVALID_REQ || ret == ESP_ERR_HTTPD_RESP_SEND) {
            ESP_LOGE(TAG, "Fatal error sending to client %d, removing client", client_slot);
            
            // Trigger session close
            handle_client_disconnect(client_slot);
        }
        
        be_pushbool(vm, false);
        be_return (vm);
    }
    
    be_pushbool(vm, true);
    if (be_top(vm) != initial_top+1) {
        ESP_LOGE(TAG, "[ws_server_send-ERROR] Stack imbalance detected: %d (expected %d)", be_top(vm), initial_top);
    }
    be_return (vm);
}

static int w_wsserver_close(bvm *vm) {
    if (be_top(vm) >= 1 && be_isint(vm, 1)) {
        int client_slot = be_toint(vm, 1);
        
        ESP_LOGI(TAG, "Closing client %d", client_slot);
        
        if (!is_client_valid(client_slot)) {
            be_pushbool(vm, false);
            be_return (vm);
        }

        // Send a close frame
        httpd_ws_frame_t ws_pkt = {0};
        ws_pkt.type = HTTPD_WS_TYPE_CLOSE;
        ws_pkt.len = 0;
        
        int sockfd = ws_clients[client_slot].sockfd;
        esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send close frame: %d", ret);
        }
        
        // Trigger session close
        handle_client_disconnect(client_slot);
        
        be_pushbool(vm, (ret == ESP_OK));
        be_return (vm);
    }
    
    ESP_LOGE(TAG, "Invalid parameters for close");
    be_pushbool(vm, false);
    be_return (vm);
}

static int w_wsserver_on(bvm *vm) {
    // Save initial stack position for balance checking
    int initial_top = be_top(vm);
    if (be_top(vm) >= 2 && be_isint(vm, 1) && 
        (be_isfunction(vm, 2) || be_isclosure(vm, 2))) {  // Accept both function and closure types
        
        // Extract event type value (we'll need this in both phases)
        int event_type = be_toint(vm, 1);
        
        // Validate event type early
        if (event_type < 0 || event_type >= 3) {
            ESP_LOGE(TAG, "[ON-ERROR] Invalid event type: %d", event_type);
            be_pushbool(vm, false);
            be_return (vm);
        }

        // Log function type for debugging
        const char *type_str = be_isfunction(vm, 2) ? "function" : "closure";
        ESP_LOGI(TAG, "[ON-STACK] Registering %s callback (type %d)", type_str, event_type);
        
        // Make a safe copy of the function value - CRITICAL for stack stability
        bvalue func_copy = *be_indexof(vm, 2);
        bool is_gc_obj = be_isgcobj(&func_copy);

        // If there's already an active callback, unmark it for GC first
        if (wsserver_callbacks[event_type].active) {
            if (be_isgcobj(&wsserver_callbacks[event_type].func)) {
                ESP_LOGI(TAG, "[ON-GC] Unmarking existing callback for event %d", event_type);
                be_gc_fix_set(vm, wsserver_callbacks[event_type].func.v.gc, bfalse);
            }
        }
        
        // Store the callback information in our global registry
        wsserver_callbacks[event_type].vm = vm;
        wsserver_callbacks[event_type].active = true;
        wsserver_callbacks[event_type].func = func_copy;  // Store our copied function
        
        // Protect function from garbage collection if needed
        if (is_gc_obj) {
            ESP_LOGI(TAG, "[ON-GC] Marking callback as protected from GC for event %d", event_type);
            be_gc_fix_set(vm, func_copy.v.gc, btrue);
        }
        
        // Log event information
        const char* event_name = 
            event_type == WSSERVER_EVENT_CONNECT ? "connect" :
            event_type == WSSERVER_EVENT_DISCONNECT ? "disconnect" : "message";
        
        ESP_LOGI(TAG, "[ON-EVENT] Registered %s callback (event %d)", event_name, event_type);
        
        // Return success
        be_pushbool(vm, true);
    } else {
        ESP_LOGE(TAG, "[ON-ERROR] Invalid parameters for on");
        be_pushbool(vm, false);
    }
    if (be_top(vm) != initial_top+1) {
    ESP_LOGE(TAG, "[ws_server_on-ERROR] Stack imbalance detected: %d (expected %d)", be_top(vm), initial_top);
    }
    be_return (vm);    
}

static int w_wsserver_is_connected(bvm *vm) {
    int initial_top = be_top(vm);
    if (be_top(vm) >= 1 && be_isint(vm, 1)) {
        int client_slot = be_toint(vm, 1);
        bool valid = is_client_valid(client_slot);
        ESP_LOGI(TAG, "Checking if client %d is connected: %s", client_slot, valid ? "yes" : "no");
        be_pushbool(vm, valid);
    } else {    
        ESP_LOGE(TAG, "Invalid parameters for is_connected");
        be_pushbool(vm, false);
    }
    if ((be_top(vm) != initial_top+1)) {
        ESP_LOGE(TAG, "[is_connected-ERROR] Stack imbalance detected: %d (expected %d)", be_top(vm), initial_top);
    }
    be_return (vm);
}

static int w_wsserver_stop(bvm *vm) {
    if (!wsserver_running) {
        ESP_LOGI(TAG, "WebSocket server not running");
        be_pushbool(vm, true);
        be_return (vm);
    }
    
    ESP_LOGI(TAG, "Stopping WebSocket server");
    

    for (int i = 0; i < 3; i++) {
        if (wsserver_callbacks[i].active) {
            if (be_isgcobj(&wsserver_callbacks[i].func)) {
                ESP_LOGI(TAG, "Unmarking callback for event %d from GC protection", i);
                be_gc_fix_set(vm, wsserver_callbacks[i].func.v.gc, bfalse);
            }
        }
    }
    
    int client_count = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].active) {
            client_count++;
            
            // Send close frame
            httpd_ws_frame_t ws_pkt = {0};
            ws_pkt.type = HTTPD_WS_TYPE_CLOSE;
            ws_pkt.len = 0;
            
            ESP_LOGI(TAG, "Sending close frame to client %d (socket: %d)", 
                    i, ws_clients[i].sockfd);
            
            httpd_ws_send_frame_async(ws_server, ws_clients[i].sockfd, &ws_pkt);
            
            // Trigger session close
            handle_client_disconnect(i);
        }
    }
    
    // Log how many clients were closed
    ESP_LOGI(TAG, "Closed connections to %d client(s)", client_count);
    
    if (ping_timer) {
        esp_timer_stop(ping_timer); // Stop it first
        esp_timer_delete(ping_timer);
        ping_timer = NULL; // Set handle to NULL
        ESP_LOGD(TAG, "Ping timer stopped and deleted.");
    }

    // We don't stop the HTTP server since we're using an existing one
    // Just clear our reference to it
    ws_server = NULL;
    wsserver_running = false;
    
    be_pushbool(vm, true);
    be_return (vm);
}

// Berry: `wsserver.set_repl_mode(client_id:int, is_repl:bool) -> bool`
// Allows Berry script to transition a client into or out of WebREPL mode.
static int w_wsserver_set_repl_mode(bvm *vm) {
    int initial_top = be_top(vm);
    bool success = false;

    // Expect client_id (int) at index 1 and is_repl (bool) at index 2
    if (be_top(vm) >= 2 && be_isint(vm, 1) && be_isbool(vm, 2)) {
        int client_id = be_toint(vm, 1);
        bool is_repl = be_tobool(vm, 2);

        // Validate client ID and activity
        if (is_client_valid(client_id)) {
            ws_client_state_t old_state = ws_clients[client_id].state;
            ws_client_state_t new_state = is_repl ? WS_STATE_REPL : WS_STATE_NORMAL_APP;

            if (old_state != new_state) {
                 ESP_LOGI(TAG, "Berry set client %d mode: %s (state %d -> %d)", 
                         client_id, 
                         is_repl ? "REPL" : "Normal App", 
                         old_state, new_state);
                 ws_clients[client_id].state = new_state;
                 ws_clients[client_id].raw_repl_mode = false; // Always start WebREPL in friendly mode
                 
                 // If entering REPL, clear any old command buffer
                 if (is_repl) {
                     ws_clients[client_id].command_len = 0;
                     ws_clients[client_id].command_buffer[0] = '\0';
                 }
                 success = true;
            } else {
                 ESP_LOGI(TAG, "Berry set client %d mode: No change needed (already state %d)", 
                         client_id, old_state);
                 success = true; // Still considered success
            }
        } else {
            ESP_LOGE(TAG, "set_repl_mode: Invalid or inactive client ID: %d", client_id);
        }
    } else {
        ESP_LOGE(TAG, "set_repl_mode: Invalid arguments (expected client_id:int, is_repl:bool)");
    }

    be_pushbool(vm, success);
    if (be_top(vm) != initial_top + 1) {
        ESP_LOGE(TAG, "[set_repl_mode-ERROR] Stack imbalance detected: %d (expected %d)", be_top(vm), initial_top + 1);
    }
    be_return(vm);
}

// Deinitialize callbacks for VM shutdown
void be_wsserver_cb_deinit(bvm *vm) {
    ESP_LOGI(TAG, "[DEINIT] Starting callback deinitialization for VM %p", vm);
    
    // Track stats
    int count_active = 0;
    int count_gc_protected = 0;
    
    // Clear all callbacks associated *specifically* with this VM instance
    for (int i = 0; i < 3; i++) {
        // Check if the callback belongs to the VM being deinitialized
        if (wsserver_callbacks[i].vm == vm && wsserver_callbacks[i].active) {
            const char* event_name = 
                i == WSSERVER_EVENT_CONNECT ? "connect" :
                i == WSSERVER_EVENT_DISCONNECT ? "disconnect" : "message";
            
            ESP_LOGI(TAG, "[DEINIT] Found active callback for event %d (%s)", i, event_name);
            count_active++;
            
            // Check if it's GC protected
            if (wsserver_callbacks[i].active && be_isgcobj(&wsserver_callbacks[i].func)) {
                ESP_LOGI(TAG, "[DEINIT-GC] Callback for event %d is GC protected, unmarking", i);
                count_gc_protected++;
                
                // Log the type of the function before unmarking
                int func_type = wsserver_callbacks[i].func.type;
                const char *type_str = 
                    func_type == BE_FUNCTION ? "function" :
                    func_type == BE_CLOSURE ? "closure" :
                    func_type == BE_NTVCLOS ? "native_closure" : "other";
                ESP_LOGI(TAG, "[DEINIT-GC] Callback type: %s (%d)", type_str, func_type);
                
                // Unmark from garbage collection
                be_gc_fix_set(vm, wsserver_callbacks[i].func.v.gc, bfalse);
                ESP_LOGI(TAG, "[DEINIT-GC] Successfully unmarked callback for event %d", i);
            } else if (wsserver_callbacks[i].active) {
                ESP_LOGI(TAG, "[DEINIT-GC] Callback for event %d is not a GC object, no need to unmark", i);
            }
            
            // Mark as inactive
            wsserver_callbacks[i].active = false;
            ESP_LOGI(TAG, "[DEINIT] Deactivated callback for event %d", i);
        }
    }
    
    ESP_LOGI(TAG, "[DEINIT] Completed callback deinitialization for VM %p: %d active callbacks processed, %d GC protected", 
             vm, count_active, count_gc_protected);
}

// Module definition
/* @const_object_info_begin
module wsserver (scope: global, strings: weak) {
    CONNECT, int(WSSERVER_EVENT_CONNECT)
    DISCONNECT, int(WSSERVER_EVENT_DISCONNECT)
    MESSAGE, int(WSSERVER_EVENT_MESSAGE)
    
    start, func(w_wsserver_start)
    stop, func(w_wsserver_stop)
    send, func(w_wsserver_send)
    close, func(w_wsserver_close)
    on, func(w_wsserver_on)
    is_connected, func(w_wsserver_is_connected)
    set_repl_mode, func(w_wsserver_set_repl_mode)

    // Constants for supported frame types
    TEXT, int(HTTPD_WS_TYPE_TEXT)
    BINARY, int(HTTPD_WS_TYPE_BINARY)
    
    // Constants for maximum clients
    MAX_CLIENTS, int(MAX_WS_CLIENTS)
}
@const_object_info_end */
#include "be_fixed_wsserver.h"

#endif // USE_BERRY_WSSERVER
