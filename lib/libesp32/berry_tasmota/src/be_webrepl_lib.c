/*
  be_webrepl_lib.c - WebREPL implementation for Berry using the WebSocket server

  Copyright (C) 2025  Jonathan E. Peace

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

#ifdef USE_BERRY_WEBREPL

#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h" // If using queues specific to REPL

#include "be_vm.h"
#include "be_exec.h"

#define TAG "WEBREPL"

typedef enum {
    WS_STATE_INIT,       // Just connected, before prompt/trigger check
    WS_STATE_PASSWORD,   // Sent prompt, awaiting password
    WS_STATE_REPL,       // Password OK, processing REPL commands
    WS_STATE_NORMAL_APP  // Determined not to be REPL
} ws_client_state_t;

// Client tracking structure
typedef struct {
    int sockfd;
    bool active;
    int64_t last_activity;  // Timestamp of any client activity in milliseconds
    ws_client_state_t state; // Client state for WebREPL/Normal App
    char password_buffer[20]; // Buffer for password input
    uint8_t password_len;
} ws_client_t;

extern ws_client_t ws_clients[]; // Direct access or provide accessor

extern httpd_handle_t ws_server; // Assumed global server handle

extern ws_client_t ws_clients[]; // Direct access or provide accessor
extern bool is_client_valid(int client_id); // Need this function
extern void handle_client_disconnect(int client_slot); // Need this
extern volatile int g_stream_sockfd; // For print streaming
extern httpd_handle_t g_stream_server_handle; // For print streaming

// --- WebREPL Specific Constants ---
#define WEBREPL_PASSWORD_PROMPT "Password: "
static const char* webrepl_password = "password"; // CHANGE THIS!


// --- WebREPL Helper Functions ---

extern void send_ws_text_frame(int sockfd, const char* text) ;
extern void send_ws_text_frame_to_client(int client_id, const char* text);

// Returns true if password was correct, false if incorrect (caller should disconnect)
bool be_webrepl_handle_password(int client_slot, const char* received_password, size_t len) {
    if (!is_client_valid(client_slot)) return false;

    // Trim trailing CR/LF
    while (len > 0 && (received_password[len - 1] == '\r' || received_password[len - 1] == '\n')) {
        len--;
    }

    ESP_LOGD(TAG, "Checking password for client %d (len %d)", client_slot, len);

    // Compare directly (ensure null termination if needed, though len is known)
    if (strncmp(received_password, webrepl_password, len) == 0 && webrepl_password[len] == '\0') {
        ESP_LOGI(TAG, "Client %d authenticated for WebREPL.", client_slot);
        
        // Directly modify the client state to ensure consistency
        ws_clients[client_slot].state = WS_STATE_REPL;
        
        // Debug logging to verify state
        ESP_LOGI(TAG, "Client %d state changed to REPL (state=%d)", client_slot, ws_clients[client_slot].state);
        
        // Use the client ID-based function for more reliable sending
        send_ws_text_frame_to_client(client_slot, "\r\nWebREPL connected\r\n>>> ");
        return true; // Password OK
    } else {
        ESP_LOGW(TAG, "Client %d WebREPL authentication failed.", client_slot);
        // Use the client ID-based function 
        send_ws_text_frame_to_client(client_slot, "Wrong password\r\n");
        return false; // Password WRONG
    }
}

// Called when ws_handler receives BINARY (Phase 2 - File Ops)
void be_webrepl_handle_binary(int client_slot, const uint8_t* data, size_t len) {
     ESP_LOGW(TAG, "Received unexpected BINARY message for client %d in REPL phase 1. Ignoring.", client_slot);
     // TODO: Implement file operations based on MicroPython binary protocol
}


void be_webrepl_execute_code(bvm *vm, int client_id, const char* code, size_t len) {
    // Validate inputs and check client state before proceeding
    if (!vm || !code || !is_client_valid(client_id)) {
        ESP_LOGE(TAG, "Invalid parameters in be_webrepl_execute_code: vm=%p, code=%p, client_id=%d valid=%d", 
                 vm, code, client_id, is_client_valid(client_id));
        return;
    }
    
    // Double-check client is in REPL state
    if (ws_clients[client_id].state != WS_STATE_REPL) {
        ESP_LOGE(TAG, "Client %d is not in REPL state (state=%d, expected %d)", 
                 client_id, ws_clients[client_id].state, WS_STATE_REPL);
        
        // Emergency state correction if mismatch
        if (is_client_valid(client_id)) {
            ESP_LOGW(TAG, "Fixing state for client %d (setting to REPL state)", client_id);
            ws_clients[client_id].state = WS_STATE_REPL;
        } else {
            return; // Client not valid, can't proceed
        }
    }
    
    // Store client socket early, as client state might change during execution
    int client_sockfd = ws_clients[client_id].sockfd;
    
    // Just log what we're about to execute
    ESP_LOGI(TAG, "REPL (Client %d, socket %d): Executing %d bytes of code", 
             client_id, client_sockfd, (int)len);
    
    // Save initial stack position
    int initial_top = be_top(vm);
    int result;
    
    // Try as regular statement first (most common case)
    ESP_LOGI(TAG, "Client %d: Attempting to load code as statement", client_id);
    result = be_loadbuffer(vm, "webrepl", code, len);
    
    // If that fails and it looks like an expression, try with "return ()"
    if (result != BE_OK) {
        ESP_LOGI(TAG, "Client %d: Load as statement failed, trying as expression", client_id);
        // Pop the error from the failed attempt
        be_pop(vm, 1);
        
        // Allocate buffer only if needed for expression wrapping
        size_t expr_len = len + 10; // "return ()" + null terminator
        char *expr = malloc(expr_len);
        
        if (expr) {
            // Create expression by wrapping in "return ()"
            int written = snprintf(expr, expr_len, "return (%.*s)", (int)len, code);
            
            // Try again as an expression
            if (written > 0 && written < expr_len) {
                ESP_LOGI(TAG, "Client %d: Trying as expression: '%s'", client_id, expr);
                result = be_loadbuffer(vm, "webrepl", expr, written);
            }
            
            free(expr);
        } else {
            ESP_LOGE(TAG, "Client %d: Failed to allocate memory for expression", client_id);
        }
    }
    
    // Execute the compiled code if loading succeeded
    if (result == BE_OK) {
        ESP_LOGI(TAG, "Client %d: Code loaded successfully, executing", client_id);
        result = be_pcall(vm, 0);
        ESP_LOGI(TAG, "Client %d: Code execution %s", client_id, 
                result == BE_OK ? "succeeded" : "failed");
    } else {
        ESP_LOGE(TAG, "Client %d: Failed to load code: %d", client_id, result);
    }
    
    // Prepare response
    char response_buffer[1024] = {0};
    
    if (result == BE_OK) {
        if (be_top(vm) > initial_top) {
            // We have a result value - check if it's nil
            if (be_isnil(vm, -1)) {
                // Don't display nil values
                ESP_LOGI(TAG, "Client %d: Result is nil, sending empty result", client_id);
                snprintf(response_buffer, sizeof(response_buffer), "\r\n>>> ");
            } else {
                // Non-nil value, display it
                const char *result_str = be_tostring(vm, -1);
                if (result_str) {
                    ESP_LOGI(TAG, "Client %d: Result value: '%s'", client_id, result_str);
                    snprintf(response_buffer, sizeof(response_buffer), "\r\n%s\r\n>>> ", result_str);
                } else {
                    ESP_LOGI(TAG, "Client %d: Result conversion to string failed", client_id);
                    snprintf(response_buffer, sizeof(response_buffer),  "\r\n>>> ");
                }
            }
        } else {
            ESP_LOGI(TAG, "Client %d: No result value", client_id);
            snprintf(response_buffer, sizeof(response_buffer), "\r\n>>> ");
        }
    } else {
        // Error occurred
        const char *error_str = be_tostring(vm, -1);
        ESP_LOGE(TAG, "Client %d: Execution error: %s", client_id, 
                error_str ? error_str : "Unknown error");
        snprintf(response_buffer, sizeof(response_buffer), "\r\nError: %s\r\n>>> ", 
                 error_str ? error_str : "Unknown error");
        be_pop(vm, 1); // Pop the error
    }
    
    // Reset stack to initial position
    be_pop(vm, be_top(vm) - initial_top);
    
    // Send response back to client using the stored socket
    if (client_sockfd >= 0) {
        ESP_LOGI(TAG, "Sending response to client %d (socket %d): '%s'", 
                client_id, client_sockfd, response_buffer);
        
        // Try multiple ways to ensure the message gets through
        
        // 1. Using the new client-based helper function
        if (is_client_valid(client_id)) {
            ESP_LOGI(TAG, "Client %d is valid, using send_ws_text_frame_to_client", client_id);
            send_ws_text_frame_to_client(client_id, response_buffer);
        } else {
            ESP_LOGW(TAG, "Client %d is no longer valid, using direct socket approach", client_id);
            // 2. First try direct send using the stored socket
            send_ws_text_frame(client_sockfd, response_buffer);
        }
        
        // Also check if the socket is still valid in the client array for diagnostic purposes
        if (is_client_valid(client_id)) {
            if (ws_clients[client_id].sockfd == client_sockfd) {
                ESP_LOGI(TAG, "Client %d socket %d is still valid", client_id, client_sockfd);
            } else {
                ESP_LOGW(TAG, "Client %d socket changed from %d to %d", 
                        client_id, client_sockfd, ws_clients[client_id].sockfd);
            }
        } else {
            ESP_LOGW(TAG, "Client %d is no longer valid, but we're using saved socket %d", 
                    client_id, client_sockfd);
        }
    } else {
        ESP_LOGE(TAG, "Invalid socket for client %d, can't send response", client_id);
    }
}

// Called by w_wsserver_send when the password prompt is detected
void be_webrepl_activate_password_mode(int client_slot) {
    if (!is_client_valid(client_slot)) return;
    ESP_LOGI(TAG, "Activating WebREPL Password Mode for client %d", client_slot);
    ws_clients[client_slot].state = WS_STATE_PASSWORD;
    ws_clients[client_slot].password_len = 0;
    // Send the actual prompt from C using client ID
    send_ws_text_frame_to_client(client_slot, WEBREPL_PASSWORD_PROMPT);
}


#endif // USE_BERRY_WEBREPL 