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

// Max number of concurrent clients
#define MAX_WS_CLIENTS 5

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
    char command_buffer[256]; // Buffer for accumulating REPL commands
    uint8_t command_len;     // Current length of command in buffer
    bool raw_repl_mode;      // Flag for RAW REPL mode
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

// Called when ws_handler receives BINARY (Phase 2 - File Ops)
void be_webrepl_handle_binary(int client_slot, const uint8_t* data, size_t len) {
     ESP_LOGW(TAG, "Received unexpected BINARY message for client %d in REPL phase 1. Ignoring.", client_slot);
     // TODO: Implement file operations based on MicroPython binary protocol
}

// Internal helper to actually run Berry code and format response
static void _be_webrepl_run_code(bvm *vm, int client_id, int client_sockfd, const char* code, size_t len) {
    ESP_LOGI(TAG, "_be_webrepl_run_code: Client %d (socket %d): Executing %d bytes of code", 
             client_id, client_sockfd, (int)len);

    int initial_top = be_top(vm);
    int result;
    
    // Try as regular statement first
    ESP_LOGD(TAG, "Client %d: Attempting to load code as statement", client_id);
    result = be_loadbuffer(vm, "webrepl", code, len);
    
    // Try as expression if statement loading failed
    if (result != BE_OK) {
        ESP_LOGD(TAG, "Client %d: Load as statement failed (%d), trying as expression", client_id, result);
        be_pop(vm, 1); // Pop error from failed load
        
        size_t expr_len = len + 10; // "return ()"
        char *expr = malloc(expr_len);
        if (expr) {
            int written = snprintf(expr, expr_len, "return (%.*s)", (int)len, code);
            if (written > 0 && written < expr_len) {
                ESP_LOGD(TAG, "Client %d: Trying as expression: '%s'", client_id, expr);
                result = be_loadbuffer(vm, "webrepl", expr, written);
            } else {
                 result = BE_EXEC_ERROR; // Indicate failure if snprintf failed
            }
            free(expr);
        } else {
            ESP_LOGE(TAG, "Client %d: Failed to allocate memory for expression", client_id);
            result = BE_MALLOC_FAIL; 
        }
    }
    
    // Execute if loaded successfully
    if (result == BE_OK) {
        ESP_LOGD(TAG, "Client %d: Code loaded successfully, executing", client_id);
        result = be_pcall(vm, 0);
        ESP_LOGI(TAG, "Client %d: Code execution result: %s (%d)", client_id, 
                result == BE_OK ? "succeeded" : "failed", result);
    } else {
        ESP_LOGE(TAG, "Client %d: Failed to load code: %d", client_id, result);
    }
    
    // Prepare and send response (uses client_id to check raw_repl_mode)
    char response_buffer[1024] = {0};
    const char* prompt = ws_clients[client_id].raw_repl_mode ? "" : ">>> ";
    const char* newline = ws_clients[client_id].raw_repl_mode ? "" : "\r\n";
    
    if (result == BE_OK) {
        if (be_top(vm) > initial_top) {
             if (be_isnil(vm, -1)) {
                snprintf(response_buffer, sizeof(response_buffer), "%s%s", newline, prompt);
             } else {
                const char *result_str = be_tostring(vm, -1);
                if (result_str) {
                    const char* result_prefix = ws_clients[client_id].raw_repl_mode ? "" : "\r\n";
                    snprintf(response_buffer, sizeof(response_buffer), "%s%s%s%s", 
                            result_prefix, result_str, newline, prompt);
                } else {
                     snprintf(response_buffer, sizeof(response_buffer),  "%s%s", newline, prompt);
                }
             }
        } else {
             snprintf(response_buffer, sizeof(response_buffer), "%s%s", newline, prompt);
        }
    } else {
        const char *error_str = be_tostring(vm, -1);
        const char* error_prefix = ws_clients[client_id].raw_repl_mode ? "" : "\r\n";        
        snprintf(response_buffer, sizeof(response_buffer), "%sError: %s%s%s", 
                 error_prefix, error_str ? error_str : "Unknown error", newline, prompt);
        be_pop(vm, 1); // Pop the error
    }
    
    be_pop(vm, be_top(vm) - initial_top); // Reset stack
    
    // Send response (using original sockfd passed in)
    if (client_sockfd >= 0) {
        ESP_LOGD(TAG, "Sending response to client %d (socket %d): '%s'", 
                client_id, client_sockfd, response_buffer);
        send_ws_text_frame(client_sockfd, response_buffer);
    } else {
         ESP_LOGE(TAG, "Invalid socket (%d) for client %d, can't send response", client_sockfd, client_id);
    }
}

// Handles all incoming data for a client in REPL mode
void be_webrepl_handle_input(bvm *vm, int client_id, const char* data, size_t len) {
    // Basic validation
    if (!vm || !data || client_id < 0 || client_id >= MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid parameters in be_webrepl_handle_input: vm=%p, data=%p, client_id=%d", 
                 vm, data, client_id);
        return;
    }

    int sockfd = ws_clients[client_id].sockfd;
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Invalid sockfd for client %d in be_webrepl_handle_input", client_id);
        return; // Can't proceed without a valid socket
    }

    ESP_LOGD(TAG, "REPL Input Handler Start: Client %d, Socket %d, State = %d, RawMode = %d, Len = %d", 
             client_id, sockfd, ws_clients[client_id].state, ws_clients[client_id].raw_repl_mode, (int)len);

    // Check for single-character control codes *first*
    if (len == 1) {
        char ctrl_char = data[0];
        bool handled = true; 
        switch (ctrl_char) {
            case 0x01: // Ctrl+A: Enter RAW REPL
                ESP_LOGI(TAG, "Client %d: Entering RAW REPL mode (^A)", client_id);
                ws_clients[client_id].raw_repl_mode = true;
                send_ws_text_frame(sockfd, "raw REPL; CTRL-B to exit\r\n"); 
                break;
            case 0x02: // Ctrl+B: Enter Friendly REPL
                ESP_LOGI(TAG, "Client %d: Entering Friendly REPL mode (^B)", client_id);
                ws_clients[client_id].raw_repl_mode = false;
                send_ws_text_frame(sockfd, "OK\r\n>>> ");
                break;
            case 0x03: // Ctrl+C: Interrupt
                ESP_LOGI(TAG, "Client %d: Interrupt received (^C)", client_id);
                ws_clients[client_id].command_len = 0; // Clear buffer
                ws_clients[client_id].command_buffer[0] = '\0';
                // TODO: Add VM interrupt logic?
                if (!ws_clients[client_id].raw_repl_mode) {
                    send_ws_text_frame(sockfd, "\r\n>>> ");
                }
                break;
            case 0x04: // Ctrl+D: Soft reset / End of input
                ESP_LOGI(TAG, "Client %d: Soft Reset / EOF received (^D)", client_id);
                 ws_clients[client_id].command_len = 0; // Clear buffer
                ws_clients[client_id].command_buffer[0] = '\0';
                // TODO: Add soft reset logic?
                if (!ws_clients[client_id].raw_repl_mode) {
                     send_ws_text_frame(sockfd, "\r\n>>> ");
                }
                break;
            default:
                handled = false; 
                break;
        }
        if (handled) {
             ESP_LOGD(TAG, "REPL Input Handler End (Control Char): Client %d", client_id);
            return; // Done handling control char
        }
    }
    
    // === Normal Command Processing / Accumulation ===
    bool is_enter = false;
    bool has_command = false;
    size_t command_length = len;
    
    // Debug the incoming data hex
    char hex_debug[128] = {0};
    for (size_t i = 0; i < len && i < 32; i++) {
        snprintf(hex_debug + i*3, sizeof(hex_debug) - i*3, "%02x ", (unsigned char)data[i]);
    }
    ESP_LOGD(TAG, "REPL received %d bytes for processing: [%s]", (int)len, hex_debug);
    
    // Check for line endings (Enter key)
    if ((len == 1 && (data[0] == '\r' || data[0] == '\n')) || 
        (len == 2 && data[0] == '\r' && data[1] == '\n')) {
        is_enter = true;
        ESP_LOGD(TAG, "Detected standalone line ending");
    } else if (len > 0) {
        if (data[len-1] == '\n') {
            is_enter = true; command_length = len - 1; has_command = true;
            if (command_length > 0 && data[command_length-1] == '\r') { command_length--; }
        } else if (data[len-1] == '\r') {
            is_enter = true; command_length = len - 1; has_command = true;
        }
        // Heuristic for full command received at once (non-raw mode)
        else if (!ws_clients[client_id].raw_repl_mode && len > 1 && ws_clients[client_id].command_len == 0) {
             has_command = true; command_length = len; is_enter = true; 
             ESP_LOGD(TAG, "Treating as complete command: %d bytes", (int)command_length);
        } else if (ws_clients[client_id].raw_repl_mode) {
             // In raw mode, treat any non-control char input potentially part of a command block
             has_command = true; command_length = len; is_enter = true; // Execute directly in raw mode for now
             // TODO: Raw mode could accumulate until ^D before executing?
        } else {
            ESP_LOGD(TAG, "Single char or partial command - accumulating");
        }
    }
    
    // --- Action based on parsed input --- 
    if (is_enter && !has_command) {
        // Standalone Enter: Execute accumulated command
        if (ws_clients[client_id].command_len > 0) {
            ESP_LOGI(TAG, "Executing accumulated command: '%s' (len: %d)", 
                     ws_clients[client_id].command_buffer, ws_clients[client_id].command_len);
            
            // ===> Set stream for execution <===
            int original_stream_sockfd = g_stream_sockfd;
            g_stream_sockfd = sockfd;
            ESP_LOGD(TAG, "Set stream sockfd to %d for client %d execution", g_stream_sockfd, client_id);
            
            _be_webrepl_run_code(vm, client_id, sockfd, ws_clients[client_id].command_buffer, ws_clients[client_id].command_len);
            
            // ===> Restore stream sockfd <===
            g_stream_sockfd = original_stream_sockfd;
            ESP_LOGD(TAG, "Restored stream sockfd to %d after client %d execution", g_stream_sockfd, client_id);
            
            ws_clients[client_id].command_len = 0;
            ws_clients[client_id].command_buffer[0] = '\0';
        } else if (!ws_clients[client_id].raw_repl_mode) {
             send_ws_text_frame(sockfd, "\r\n>>> "); // Empty command, show prompt
        }
    } 
    else if (has_command) { // Includes case where is_enter is true for direct/raw execution
        // Command data received (might include line endings if is_enter is true)
        char* command_to_run = NULL;
        size_t run_len = 0;
        bool free_command = false;

        // Decide what to execute: accumulated + new, or just new?
        if (!is_enter || ws_clients[client_id].command_len == 0) { 
             // Execute the received data directly (raw mode or full command)
             command_to_run = (char*)data; // Use directly, NO free
             run_len = command_length;
             if (!ws_clients[client_id].raw_repl_mode) {
                send_ws_text_frame(sockfd, data); // Echo if not raw
             }
             ESP_LOGI(TAG, "Executing direct/raw command (len: %d)", (int)run_len);
        } else {
            // Append to buffer and execute (friendly mode with line ending)
            size_t available = sizeof(ws_clients[client_id].command_buffer) - ws_clients[client_id].command_len - 1;
            if (available >= command_length) {
                 strncat(ws_clients[client_id].command_buffer, data, command_length);
                 ws_clients[client_id].command_len += command_length;
                 ws_clients[client_id].command_buffer[ws_clients[client_id].command_len] = '\0';
                 command_to_run = ws_clients[client_id].command_buffer; // Use buffer, NO free
                 run_len = ws_clients[client_id].command_len;
                 ESP_LOGI(TAG, "Executing accumulated+new command (len: %d)", (int)run_len);
            } else {
                  ESP_LOGW(TAG, "Command buffer overflow for client %d", client_id);
                  send_ws_text_frame(sockfd, "\r\nCommand too long\r\n>>> ");
            }
        }

        // Execute if we have a command
        if (command_to_run && run_len > 0) {
             // ===> Set stream for execution <===
             int original_stream_sockfd = g_stream_sockfd;
             g_stream_sockfd = sockfd;
             ESP_LOGD(TAG, "Set stream sockfd to %d for client %d execution", g_stream_sockfd, client_id);
            
             _be_webrepl_run_code(vm, client_id, sockfd, command_to_run, run_len);
             
             // ===> Restore stream sockfd <===
             g_stream_sockfd = original_stream_sockfd;
             ESP_LOGD(TAG, "Restored stream sockfd to %d after client %d execution", g_stream_sockfd, client_id);
        }
        
        // Reset buffer after execution
        ws_clients[client_id].command_len = 0;
        ws_clients[client_id].command_buffer[0] = '\0';

    } 
    else {
        // Accumulate characters (friendly mode, no line ending yet)
        if (!ws_clients[client_id].raw_repl_mode) {
            send_ws_text_frame(sockfd, data); // Echo
        }
        size_t available = sizeof(ws_clients[client_id].command_buffer) - ws_clients[client_id].command_len - 1;
        if (available > 0) {
            size_t chars_to_copy = len < available ? len : available;
            strncat(ws_clients[client_id].command_buffer, data, chars_to_copy);
            ws_clients[client_id].command_len += chars_to_copy;
            ws_clients[client_id].command_buffer[ws_clients[client_id].command_len] = '\0';
            ESP_LOGD(TAG, "Accumulated command so far: '%s' (len: %d)", 
                   ws_clients[client_id].command_buffer, ws_clients[client_id].command_len);
        } else {
            ESP_LOGW(TAG, "Command buffer overflow for client %d", client_id);
            send_ws_text_frame(sockfd, "\r\nCommand too long\r\n>>> ");
            ws_clients[client_id].command_len = 0;
            ws_clients[client_id].command_buffer[0] = '\0';
        }
    }
    ESP_LOGD(TAG, "REPL Input Handler End: Client %d", client_id);
}

// REMOVED: Obsolete activation function
// Called by w_wsserver_send when the password prompt is detected
// void be_webrepl_activate_password_mode(int client_slot) { ... }


#endif // USE_BERRY_WEBREPL 