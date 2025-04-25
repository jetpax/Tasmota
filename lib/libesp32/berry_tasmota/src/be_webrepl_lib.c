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
#include <stdio.h> // For FILE*, fopen, etc.

#include "be_vm.h"
#include "be_exec.h"

#define TAG "WEBREPL"

// Max number of concurrent clients
#define MAX_WS_CLIENTS 5

// --- BEGIN ADDED Binary Protocol definitions ---
// Mirroring MicroPython's WebREPL binary protocol header
typedef struct __attribute__((packed)) { // Use packed to match potential uPy layout
    char sig[2];        // Should be 'W', 'A'
    uint8_t op;         // 1=PUT_FILE, 2=GET_FILE
    uint8_t flags;      // Currently unused?
    uint64_t offset;    // File offset for PUT/GET (Little Endian)
    uint32_t size;      // File size for PUT (Little Endian)
    uint16_t fname_len; // Length of filename (Little Endian)
    // Filename follows immediately
} webrepl_binhdr_t;

#define WEBREPL_HDR_SIG "WA"
#define WEBREPL_OP_PUT_FILE 1
#define WEBREPL_OP_GET_FILE 2
#define WEBREPL_RESP_OK 0
#define WEBREPL_RESP_ERROR 1

// State for an ongoing binary operation for a specific client
typedef struct {
    bool active; // Is a binary operation in progress?
    webrepl_binhdr_t hdr;
    uint32_t hdr_bytes_received;
    uint32_t data_bytes_expected; // Filename len OR file size
    uint32_t data_bytes_received;
    FILE *fp;
    char filename[128]; // Max filename length + safety margin
} webrepl_binop_state_t;
// --- END ADDED Binary Protocol definitions ---

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
    webrepl_binop_state_t binop; // <<< ADDED
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

// --- BEGIN ADDED Binary Handling Functions ---

// Helper to send binary responses (status codes)
static void webrepl_send_bin_resp(int client_id, uint16_t code) {
    if (!is_client_valid(client_id)) return;
    int sockfd = ws_clients[client_id].sockfd;
    if (sockfd < 0) return;

    // Response is WB + 16-bit code (Little Endian)
    char buf[4] = {'W', 'B', (uint8_t)(code & 0xFF), (uint8_t)(code >> 8)};

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.payload = (uint8_t*)buf;
    frame.len = sizeof(buf);
    frame.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send bin response %d to client %d: %s", code, client_id, esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Sent bin response %d to client %d", code, client_id);
    }
}

// Helper function to send file chunk for GET requests
static bool webrepl_send_file_chunk(int client_id) {
     if (!is_client_valid(client_id)) return false;
     webrepl_binop_state_t *op_state = &ws_clients[client_id].binop;
     if (!op_state->active || op_state->fp == NULL || op_state->hdr.op != WEBREPL_OP_GET_FILE) return false;

     int sockfd = ws_clients[client_id].sockfd;
     FILE *fp = op_state->fp;
     uint8_t chunk_buf[256 + 2]; // Max chunk size + 2 bytes length prefix
     size_t bytes_read;

     // Protect against reading past intended size if specified (though GET usually doesn't specify size)
     uint32_t max_read = 256;
     // if (op_state->hdr.size > 0 && op_state->data_bytes_received + max_read > op_state->hdr.size) {
     //     max_read = op_state->hdr.size - op_state->data_bytes_received;
     // }

     if (max_read == 0) { // Should not happen unless size was 0?
         bytes_read = 0;
     } else {
        bytes_read = fread(chunk_buf + 2, 1, max_read, fp);
     }

     ESP_LOGD(TAG,"GET File: Read %d bytes from '%s'", (int)bytes_read, op_state->filename);

     // Check for read error or end of file
     if (bytes_read == 0) {
         if (ferror(fp)) {
             ESP_LOGE(TAG, "Error reading file '%s' for GET client %d", op_state->filename, client_id);
             webrepl_send_bin_resp(client_id, WEBREPL_RESP_ERROR);
         } else {
             ESP_LOGI(TAG, "Finished sending GET file '%s' to client %d", op_state->filename, client_id);
             webrepl_send_bin_resp(client_id, WEBREPL_RESP_OK); // Final OK
         }
         fclose(fp);
         op_state->fp = NULL;
         op_state->active = false; // Operation finished
         return false; // Indicate finished
     }

     // Prepend chunk length (Little Endian)
     chunk_buf[0] = (uint8_t)(bytes_read & 0xFF);
     chunk_buf[1] = (uint8_t)(bytes_read >> 8);

     ESP_LOGD(TAG, "Sending %d bytes file chunk to client %d", bytes_read, client_id);
     httpd_ws_frame_t frame;
     memset(&frame, 0, sizeof(frame));
     frame.payload = chunk_buf;
     frame.len = bytes_read + 2; // Length prefix + data
     frame.type = HTTPD_WS_TYPE_BINARY;

     esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &frame);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "Failed to send file chunk to client %d: %s", client_id, esp_err_to_name(ret));
         fclose(fp);
         op_state->fp = NULL;
         op_state->active = false; // Operation failed
         // Don't send another response here, error already logged
         return false;
     }

     op_state->data_bytes_received += bytes_read; // Track bytes sent for GET
     return true; // Chunk sent, more expected
}

// Called when ws_handler receives BINARY
// Process incoming binary data (called from main task handler)
void be_webrepl_handle_binary(bvm *vm, int client_id, const uint8_t* data, size_t len) {
    if (!is_client_valid(client_id)) return;

    webrepl_binop_state_t *op_state = &ws_clients[client_id].binop;
    const uint8_t* p_data = data;
    size_t remaining_len = len;

    ESP_LOGD(TAG, "Binary Handle: Client %d, len %d, OpActive: %d, HdrRec: %u, DataRec: %u, DataExp: %u",
            client_id, (int)len, op_state->active, op_state->hdr_bytes_received,
            op_state->data_bytes_received, op_state->data_bytes_expected);

    // --- 1. Start new operation or continue existing ---
    if (!op_state->active) {
        // Expecting header for a new operation
        memset(op_state, 0, sizeof(webrepl_binop_state_t)); // Reset state
        op_state->active = true;
        op_state->hdr_bytes_received = 0;
        op_state->data_bytes_received = 0;
        op_state->data_bytes_expected = 0; // Set later
        op_state->fp = NULL;
        ESP_LOGD(TAG,"Binary Handle: Client %d starting new binary op.", client_id);
    }

    // --- 2. Receive Header ---
    if (op_state->hdr_bytes_received < sizeof(webrepl_binhdr_t)) {
        size_t needed = sizeof(webrepl_binhdr_t) - op_state->hdr_bytes_received;
        size_t to_copy = (remaining_len < needed) ? remaining_len : needed;
        memcpy((uint8_t*)&op_state->hdr + op_state->hdr_bytes_received, p_data, to_copy);
        op_state->hdr_bytes_received += to_copy;
        p_data += to_copy;
        remaining_len -= to_copy;

        // Header complete?
        if (op_state->hdr_bytes_received == sizeof(webrepl_binhdr_t)) {
             // Validate Signature
             if (strncmp(op_state->hdr.sig, WEBREPL_HDR_SIG, 2) != 0) {
                 ESP_LOGE(TAG, "Client %d: Invalid bin op signature: %c%c", client_id, op_state->hdr.sig[0], op_state->hdr.sig[1]);
                 op_state->active = false; // Abort op
                 // Consider closing connection?
                 return;
             }
             // Validate Filename Length
             if (op_state->hdr.fname_len >= sizeof(op_state->filename)) {
                  ESP_LOGE(TAG, "Client %d: Filename too long (%u)", client_id, op_state->hdr.fname_len);
                  op_state->active = false; // Abort op
                  return;
             }
             ESP_LOGD(TAG, "Binary Handle: Client %d Header received. Op: %d, FNL:%u, Size:%u",
                      client_id, op_state->hdr.op, op_state->hdr.fname_len, op_state->hdr.size);
             op_state->data_bytes_expected = op_state->hdr.fname_len; // Now expect filename
             op_state->data_bytes_received = 0;
             op_state->filename[0] = ' ';
        }
    }

    // --- 3. Receive Filename ---
    if (op_state->hdr_bytes_received == sizeof(webrepl_binhdr_t) &&
        op_state->data_bytes_expected == op_state->hdr.fname_len && // Expecting filename
        op_state->data_bytes_received < op_state->data_bytes_expected) {

        size_t needed = op_state->data_bytes_expected - op_state->data_bytes_received;
        size_t to_copy = (remaining_len < needed) ? remaining_len : needed;
        memcpy(op_state->filename + op_state->data_bytes_received, p_data, to_copy);
        op_state->data_bytes_received += to_copy;
        p_data += to_copy;
        remaining_len -= to_copy;

        // Filename complete?
        if (op_state->data_bytes_received == op_state->data_bytes_expected) {
            op_state->filename[op_state->data_bytes_received] = ' '; // Terminate filename
            ESP_LOGI(TAG, "Binary Handle: Client %d Op %d, Filename '%s' received.", client_id, op_state->hdr.op, op_state->filename);

            // --- Prepare for File Data or Action ---
            bool op_ok = false;
            const char* file_mode = NULL;
            // TODO: Sanitize filename - VERY IMPORTANT
            // e.g., check for '/', '..', non-printable chars

            if (op_state->hdr.op == WEBREPL_OP_PUT_FILE) {
                file_mode = "wb";
                // TODO: Handle offset with "r+b" or "ab"? fopen("wb") truncates.
                // Micropython WebREPL PUT seems to always truncate/overwrite.
            } else if (op_state->hdr.op == WEBREPL_OP_GET_FILE) {
                file_mode = "rb";
            }

            if (file_mode) {
                // IMPORTANT: Assume files are relative to a base path, e.g., "/"
                char full_path[sizeof(op_state->filename) + 10]; 
                snprintf(full_path, sizeof(full_path), "/%s", op_state->filename); 
                // TODO: Add better path sanitation (e.g., disallow '..')
                
                op_state->fp = fopen(full_path, file_mode);
                if (op_state->fp) {
                    op_ok = true;
                    ESP_LOGI(TAG,"Opened '%s' (%s) for client %d", full_path, file_mode, client_id);
                    // TODO: Handle fseek for offset if needed
                } else {
                    ESP_LOGE(TAG, "Failed to open '%s' (%s) for client %d", full_path, file_mode, client_id);
                }
            } else {
                 ESP_LOGW(TAG, "Client %d: Unsupported binary op: %d", client_id, op_state->hdr.op);
                 // Treat as error for now
            }

            // Send initial response
            webrepl_send_bin_resp(client_id, op_ok ? WEBREPL_RESP_OK : WEBREPL_RESP_ERROR);

            if (!op_ok) {
                op_state->active = false; // Abort failed op
            } else {
                // Setup for next data phase
                if (op_state->hdr.op == WEBREPL_OP_PUT_FILE) {
                    op_state->data_bytes_expected = op_state->hdr.size; // Now expect file content
                    op_state->data_bytes_received = 0;
                    ESP_LOGD(TAG,"Expecting %u bytes for PUT '%s'", op_state->data_bytes_expected, op_state->filename);
                } else if (op_state->hdr.op == WEBREPL_OP_GET_FILE) {
                    op_state->data_bytes_expected = 0; // Not expecting client data
                    op_state->data_bytes_received = 0; // Will track bytes *sent*
                    ESP_LOGD(TAG,"Starting send for GET '%s'", op_state->filename);
                    if (!webrepl_send_file_chunk(client_id)) {
                        // File send finished immediately or failed
                        // State already cleaned up by helper
                    }
                }
            }
        }
    }

    // --- 4. Receive/Process File Data (PUT) ---
    if (op_state->active &&
        op_state->hdr_bytes_received == sizeof(webrepl_binhdr_t) &&
        op_state->data_bytes_expected == op_state->hdr.size && // Expecting file content
        op_state->hdr.op == WEBREPL_OP_PUT_FILE && op_state->fp != NULL) {

        if (remaining_len > 0) {
            size_t bytes_to_write = remaining_len;
            // Check if received data exceeds expected size
            if (op_state->data_bytes_received + bytes_to_write > op_state->data_bytes_expected) {
                 ESP_LOGW(TAG, "Client %d PUT: Received more data (%d) than expected (%u). Truncating.",
                          client_id, (int)bytes_to_write, op_state->data_bytes_expected - op_state->data_bytes_received);
                 bytes_to_write = op_state->data_bytes_expected - op_state->data_bytes_received;
            }

            if (bytes_to_write > 0) {
                 size_t written = fwrite(p_data, 1, bytes_to_write, op_state->fp);
                 ESP_LOGD(TAG, "PUT '%s': Wrote %d / %d bytes", op_state->filename, (int)written, (int)bytes_to_write);
                 if (written != bytes_to_write) {
                     ESP_LOGE(TAG, "Client %d PUT: Error writing to file '%s'", client_id, op_state->filename);
                     webrepl_send_bin_resp(client_id, WEBREPL_RESP_ERROR);
                     fclose(op_state->fp);
                     op_state->fp = NULL;
                     op_state->active = false; // Abort
                     return;
                 }
                 op_state->data_bytes_received += written;
                 p_data += written; // Should not be needed
                 remaining_len -= written;
            }
        }

        // Check if PUT complete
        if (op_state->data_bytes_received == op_state->data_bytes_expected) {
            ESP_LOGI(TAG, "Client %d PUT: Finished receiving '%s' (%u bytes)",
                     client_id, op_state->filename, op_state->data_bytes_received);
            fclose(op_state->fp);
            op_state->fp = NULL;
            webrepl_send_bin_resp(client_id, WEBREPL_RESP_OK); // Final OK
            op_state->active = false; // Operation finished
        }
    }

     // --- 5. Handle GET Confirmation (Not standard WebREPL, but maybe useful) ---
     // MicroPython's client might send a single byte (often 0x00) after receiving
     // a data chunk to signal readiness for the next one. Our current send logic
     // doesn't wait for this, it just sends chunks. If we needed to wait:
     /*
     if (op_state->active && op_state->hdr.op == WEBREPL_OP_GET_FILE && op_state->fp != NULL) {
         if (remaining_len > 0) {
             // Assume it's the confirmation byte
             ESP_LOGD(TAG,"Client %d GET: Received confirmation byte 0x%02x", client_id, *p_data);
             if (!webrepl_send_file_chunk(client_id)) {
                 // File send finished or failed
             }
             remaining_len--; // Consumed byte
         }
     }
     */

    if (remaining_len > 0 && op_state->active) {
         ESP_LOGW(TAG, "Binary Handle: Client %d, %d bytes remaining in chunk after processing phase?", client_id, (int)remaining_len);
    }
    ESP_LOGD(TAG,"Binary Handle End: Client %d", client_id);
}
// --- END ADDED Binary Handling Functions ---

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