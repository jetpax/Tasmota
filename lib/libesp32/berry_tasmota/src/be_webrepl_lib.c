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
        ws_clients[client_slot].state = WS_STATE_REPL; // Transition state
        send_ws_text_frame_repl(ws_clients[client_slot].sockfd, "\r\nWebREPL connected\r\n>>> ");
        return true; // Password OK
    } else {
        ESP_LOGW(TAG, "Client %d WebREPL authentication failed.", client_slot);
        send_ws_text_frame(ws_clients[client_slot].sockfd, "Wrong password\r\n");
        return false; // Password WRONG
    }
}

// Called when ws_handler receives BINARY (Phase 2 - File Ops)
void be_webrepl_handle_binary(int client_slot, const uint8_t* data, size_t len) {
     ESP_LOGW(TAG, "Received unexpected BINARY message for client %d in REPL phase 1. Ignoring.", client_slot);
     // TODO: Implement file operations based on MicroPython binary protocol
}


void be_webrepl_execute_code(bvm *vm, int client_id, const char* code) {
    // Validate inputs
    if (!vm || !code || !is_client_valid(client_id)) {
        ESP_LOGE(TAG, "Invalid parameters in be_webrepl_execute_code");
        return;
    }
    
    ESP_LOGI(TAG, "REPL (Client %d): Executing '%s'", client_id, code);
    
    // Save initial stack position
    int initial_top = be_top(vm);
    int result;
    
    // First try to treat it as an expression by wrapping it in "return ()"
    char *expr_code = NULL;
    size_t expr_len = strlen(code) + 20; // Extra space for "return ()" and null terminator
    expr_code = malloc(expr_len);
    
    if (expr_code) {
        snprintf(expr_code, expr_len, "return (%s)", code);
        
        // Try as expression first
        result = be_loadbuffer(vm, "webrepl", expr_code, strlen(expr_code));
        free(expr_code);
        
        if (result != BE_OK) {
            // If that fails, try as regular statement
            be_pop(vm, 1); // Pop the error
            result = be_loadbuffer(vm, "webrepl", code, strlen(code));
        }
    } else {
        // If malloc fails, fall back to direct execution
        result = be_loadbuffer(vm, "webrepl", code, strlen(code));
    }
    
    // Execute the compiled code
    if (result == BE_OK) {
        result = be_pcall(vm, 0);
    }
    
    // Prepare response
    char response_buffer[1024] = {0};
    
    if (result == BE_OK) {
        if (be_top(vm) > initial_top) {
            // We have a result value - check if it's nil
            if (be_isnil(vm, -1)) {
                // Don't display nil values
                snprintf(response_buffer, sizeof(response_buffer), ">>> ");
            } else {
                // Non-nil value, display it
                const char *result_str = be_tostring(vm, -1);
                if (result_str) {
                    snprintf(response_buffer, sizeof(response_buffer), "%s\n>>> ", result_str);
                } else {
                    snprintf(response_buffer, sizeof(response_buffer), ">>> ");
                }
            }
        } else {
            snprintf(response_buffer, sizeof(response_buffer), ">>> ");
        }
    } else {
        // Error occurred
        const char *error_str = be_tostring(vm, -1);
        snprintf(response_buffer, sizeof(response_buffer), "Error: %s\n>>> ", 
                 error_str ? error_str : "Unknown error");
        be_pop(vm, 1); // Pop the error
    }
    
    // Reset stack to initial position
    be_pop(vm, be_top(vm) - initial_top);
    
    // Send response back to client
    if (ws_clients[client_id].active) {
        send_ws_text_frame(ws_clients[client_id].sockfd, response_buffer);
    }
}

// Called by w_wsserver_send when the password prompt is detected
void be_webrepl_activate_password_mode(int client_slot) {
    if (!is_client_valid(client_slot)) return;
    ESP_LOGI(TAG, "Activating WebREPL Password Mode for client %d", client_slot);
    ws_clients[client_slot].state = WS_STATE_PASSWORD;
    ws_clients[client_slot].password_len = 0;
    // Send the actual prompt from C
    send_ws_text_frame(ws_clients[client_slot].sockfd, WEBREPL_PASSWORD_PROMPT);
}


#endif // USE_BERRY_WEBREPL 