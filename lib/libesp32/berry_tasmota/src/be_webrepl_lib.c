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
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif


#include <string.h>

// ESP-IDF / FreeRTOS includes 
#include "esp_log.h"
#include "esp_err.h" 
// #include "esp_http_server.h" // Provided by be_webrepl.h
#include "freertos/FreeRTOS.h" 

// Berry includes
#include "be_vm.h"
#include "be_exec.h"


#include "be_webrepl.h" // <<< Include the new header



#define TAG "WEBREPL"


// --- WebREPL Specific Constants ---
#define WEBREPL_PASSWORD_PROMPT "Password: "
static const char* webrepl_password = "password"; // CHANGE THIS!


// --- WebREPL Helper Functions ---

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

// Helper function to send file chunk for GET requests - Implementation stays here
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
// Process incoming binary data (called from main task handler) - Implementation stays here
void be_webrepl_handle_binary(bvm *vm, int client_id, const uint8_t* data, size_t len) {
    if (!is_client_valid(client_id)) return;

    webrepl_binop_state_t *op_state = &ws_clients[client_id].binop;

    // <<< MODIFIED: Handle GET confirmations - check for specific 0x00 byte >>>
    if (op_state->active && op_state->hdr.op == WEBREPL_OP_GET_FILE && op_state->fp != NULL) {
        // Check if the received packet is exactly the 1-byte confirmation (0x00)
        if (len == 1 && data[0] == 0x00) { 
            ESP_LOGD(TAG,"Client %d GET: Received confirmation byte 0x00, sending next chunk.", client_id);
            if (!webrepl_send_file_chunk(client_id)) {
                // File send finished or failed, state already cleaned up by helper
                ESP_LOGD(TAG,"Client %d GET: Send finished/failed after confirmation.", client_id);
            }
        } else {
             ESP_LOGW(TAG,"Client %d GET: Received unexpected binary data (len %d, data[0]=0x%02x) during active GET. Ignoring.", 
                      client_id, (int)len, (len > 0 ? data[0] : 0xFF));
        }
        return; // Done handling this packet (either confirmation or unexpected data)
    }
    // <<< END MODIFICATION >>>

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
             op_state->filename[0] = '\0';
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
            op_state->filename[op_state->data_bytes_received] = '\0'; // Terminate filename
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
                    ESP_LOGD(TAG,"GET ready for client %d, waiting for confirmation (0x00) before sending '%s'", client_id, op_state->filename);
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

// <<< NEW FUNCTION: Attempts to load and execute Berry code >>> - Implementation stays here
static int _be_webrepl_attempt_execute(bvm *vm, const char* code, size_t len) {
    int initial_top = be_top(vm);
    int result;
    bool loaded = false;

    // 1. Try as statement
    ESP_LOGD(TAG, "Attempting to load code as statement");
    result = be_loadbuffer(vm, "webrepl", code, len);
    
    if (result == BE_OK) {
        // Statement loaded OK
        loaded = true;
    } else if (be_getexcept(vm, result) == BE_SYNTAX_ERROR) {
        // Statement syntax error -> try as expression
        ESP_LOGD(TAG, "Load as statement failed (Syntax Error), trying as expression");
        be_pop(vm, 2); // Pop statement syntax error items (assuming 2 based on CmndBrRun)

        // Format expression: return (...)
        size_t expr_len = len + 10; // "return ()"
        char *expr = malloc(expr_len);
        if (expr) {
            // Strip trailing newline(s) for the expression body
            int expr_code_len = len;
            if (expr_code_len > 0 && code[expr_code_len - 1] == '\n') {
                expr_code_len--;
                if (expr_code_len > 0 && code[expr_code_len - 1] == '\r') {
                    expr_code_len--;
                }
            }

            int written = snprintf(expr, expr_len, "return (%.*s)", expr_code_len, code);
            if (written > 0 && written < expr_len) {
                ESP_LOGD(TAG, "Trying as expression: '%s'", expr);
                result = be_loadbuffer(vm, "webrepl", expr, written); // Try loading expression
                if (result == BE_OK) {
                    loaded = true; // Expression loaded OK
                } else {
                    // Expression load failed (Syntax or other error)
                    ESP_LOGE(TAG, "Load as expression failed: %d", result);
                    // Error object is on stack. Leave it for the caller.
                    loaded = false;
                }
            } else {
                 result = BE_EXEC_ERROR; // Indicate failure if snprintf failed
                 loaded = false;
                 ESP_LOGE(TAG, "Failed to format expression string");
            }
            free(expr);
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for expression");
            result = BE_MALLOC_FAIL; 
            loaded = false;
        }
    } else {
        // Statement load failed (Non-syntax error, e.g., memory)
        ESP_LOGE(TAG, "Load as statement failed (Non-syntax): %d", result);
        // Error object is on stack. Leave it for the caller.
        loaded = false;
    }
    
    // 2. Execute if loaded successfully
    if (loaded) {
        ESP_LOGD(TAG, "Code loaded successfully, executing");
        result = be_pcall(vm, 0); // Can return BE_OK or BE_EXCEPTION
        // If BE_OK, result value is on stack.
        // If BE_EXCEPTION, error object (presumably 2 items) is on stack.
    }

    // 3. Final result determination
    // The stack should contain: result value (if BE_OK), or error object(s) otherwise.
    // This is handled by _be_webrepl_send_result.
    // No explicit stack cleanup needed here anymore.

    return result; // Return the final status (BE_OK, BE_EXCEPTION, or Load Error code)
}

// <<< NEW FUNCTION: Formats and sends REPL response/error >>> - Implementation stays here
static void _be_webrepl_send_result(bvm *vm, int client_id, int client_sockfd, int exec_result) {
    char response_buffer[1024] = {0};
    const char* prompt = ">>> ";
    const char* newline = "\r\n";
    int original_top = be_top(vm); // Capture top BEFORE processing result/error
    int items_to_pop = 0; // How many items to pop at the end

    if (exec_result == BE_OK) {
        // Handle successful execution result
        if (original_top > 0) { // Check if stack has at least one item (the result)
            int result_index = -1; // Use relative index from top
            if (be_isnil(vm, result_index)) {
                snprintf(response_buffer, sizeof(response_buffer), "%s%s", newline, prompt);
            } else {
                const char *result_str = be_tostring(vm, result_index); 
                if (result_str) {
                    snprintf(response_buffer, sizeof(response_buffer), "%s%s%s%s", 
                             newline, result_str, newline, prompt);
                } else {
                    ESP_LOGW(TAG, "Failed to convert result to string for client %d", client_id);
                    snprintf(response_buffer, sizeof(response_buffer),  "%s%s", newline, prompt);
                }
            }
            items_to_pop = 1; // Pop the single result value
        } else {
             ESP_LOGW(TAG, "Stack unexpectedly empty after BE_OK in _be_webrepl_send_result for client %d!", client_id);
             snprintf(response_buffer, sizeof(response_buffer), "%s%s", newline, prompt);
             items_to_pop = 0;
        }
    } else { // Any error (BE_SYNTAX_ERROR, BE_EXCEPTION, etc.)
        ESP_LOGD(TAG, "SendResult Error (%d): Original Top: %d", exec_result, original_top);
        if (original_top >= 1) { // Assume at least one error item (message)
             const char *error_str = be_tostring(vm, -1); // Get message from top
             // Optional: Log the item below the top if expected
             // if (original_top >= 2) { const char *prev_str = be_tostring(vm, -2); ESP_LOGD(TAG, "Item below top: %s", prev_str?prev_str:"<nil>"); }
             snprintf(response_buffer, sizeof(response_buffer), "%sError: %s%s%s", 
                     newline, error_str ? error_str : "Unknown error", newline, prompt);
             items_to_pop = (original_top >= 2) ? 2 : 1; // Pop 2 if available based on reference, else 1
             ESP_LOGD(TAG, "SendResult Error: Popping %d items", items_to_pop);
        } else {
             ESP_LOGW(TAG, "Stack unexpectedly empty after ERROR (%d) in _be_webrepl_send_result for client %d!", exec_result, client_id);
             snprintf(response_buffer, sizeof(response_buffer), "%sError: Unknown error%s%s", newline, newline, prompt);
             items_to_pop = 0;
        }
    }

    // Send response
    if (client_sockfd >= 0) {
        ESP_LOGD(TAG, "Sending response to client %d (socket %d): '%s'", 
                client_id, client_sockfd, response_buffer);
        send_ws_text_frame(client_sockfd, response_buffer);
    } else {
         ESP_LOGE(TAG, "Invalid socket (%d) for client %d, can't send result", client_sockfd, client_id);
    }

    // Pop result/error items from stack
    if (items_to_pop > 0) {
        be_pop(vm, items_to_pop);
    }

    // Sanity check stack - Log error if mismatch, but don't try to fix it.
    int expected_final_top = original_top - items_to_pop;
    if (be_top(vm) != expected_final_top) { 
         ESP_LOGE(TAG, "Stack imbalance after sending result for client %d! Top: %d, Expected: %d (Original Top: %d, Popped: %d)",
                 client_id, be_top(vm), expected_final_top, original_top, items_to_pop);
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

    ws_client_t *client = &ws_clients[client_id]; // Get client struct pointer
    int sockfd = client->sockfd;
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Invalid sockfd for client %d in be_webrepl_handle_input", client_id);
        return; // Can't proceed without a valid socket
    }

    ESP_LOGD(TAG, "REPL Input Handler Start: Client %d, Socket %d, State = %d, Len = %d", 
             client_id, sockfd, client->state, (int)len);

    // --- Handle Ctrl+C --- 
    if (len == 1 && data[0] == 0x03) { // Ctrl+C
        ESP_LOGI(TAG, "Client %d: Interrupt received (^C)", client_id);
        if (client->command_buffer) { // Clear buffer if allocated
            client->command_len = 0;
            client->command_buffer[0] = '\0';
        }
        send_ws_text_frame(sockfd, "\r\n>>> "); // Send prompt
        ESP_LOGD(TAG, "REPL Input Handler End (Ctrl+C): Client %d", client_id);
        return;
    }

    // === Character by Character Processing ===
    size_t command_start_index = 0; // Start index of pending chars in current `data` frame

    for (size_t i = 0; i < len; ++i) {
        char current_char = data[i];
        bool is_newline = (current_char == '\r' || current_char == '\n');
        bool is_backspace = (current_char == '\b' || current_char == 0x7f);

        if (is_backspace) {
            // Echo effect: Send backspace-space-backspace
            send_ws_text_frame(sockfd, "\b \b"); 
            // Handle buffer
            if (client->command_len > 0) {
                client->command_len--;
                // Ensure buffer is null-terminated after backspace
                if(client->command_buffer) client->command_buffer[client->command_len] = '\0';
            } else {
                 // Maybe beep or ignore if buffer already empty?
            }
            command_start_index = i + 1; // Discard the character for accumulation purposes
        } else if (is_newline) {
            // Echo newline
            send_ws_text_frame(sockfd, "\r\n"); 

            // 1. Append pending characters before the newline
            size_t chars_to_append = i - command_start_index;
            if (chars_to_append > 0) {
                size_t needed_len = client->command_len + chars_to_append + 1; // +1 for null terminator
                // Resize buffer if needed
                if (client->buffer_capacity < needed_len) {
                    size_t new_capacity = (client->buffer_capacity == 0) ? 256 : client->buffer_capacity * 2;
                    while (new_capacity < needed_len) new_capacity *= 2;
                    char *new_buffer = realloc(client->command_buffer, new_capacity);
                    if (!new_buffer) {
                        ESP_LOGE(TAG, "Failed to realloc command buffer (append) for client %d", client_id);
                        // Consider sending error, clearing state? For now, just log.
                        client->command_len = 0; // Reset length
                        if (client->command_buffer) client->command_buffer[0] = '\0';
                        command_start_index = i + 1;
                        continue; // Skip execution attempt
                    }
                    client->command_buffer = new_buffer;
                    client->buffer_capacity = new_capacity;
                }
                // Append the actual data
                memcpy(client->command_buffer + client->command_len, &data[command_start_index], chars_to_append);
                client->command_len += chars_to_append;
                client->command_buffer[client->command_len] = '\0'; // Null terminate
            }
            
            // 2. Execute or send prompt
            if (client->command_len > 0) {
                 ESP_LOGD(TAG, "Executing accumulated command (len %d):\n---BEGIN---\n%s\n---END---", 
                          (int)client->command_len, client->command_buffer ? client->command_buffer : "<NULL>");
                 
                 // ===> Set stream for execution <===
                 int original_stream_sockfd = g_stream_sockfd;
                 g_stream_sockfd = sockfd;
                 
                 int exec_result = _be_webrepl_attempt_execute(vm, client->command_buffer, client->command_len);
                 
                 // ===> Restore stream sockfd <===
                 g_stream_sockfd = original_stream_sockfd;

                 ESP_LOGI(TAG, "Execution attempt result for client %d: %d", client_id, exec_result);
                 _be_webrepl_send_result(vm, client_id, sockfd, exec_result); // Send result/error/prompt

                 // Clear buffer after execution attempt
                 client->command_len = 0;
                 if (client->command_buffer) client->command_buffer[0] = '\0';
            } else {
                 // Empty line entered, just send prompt
                 send_ws_text_frame(sockfd, ">>> ");
            }

            // Handle CRLF sequence
            if (current_char == '\r' && (i + 1 < len) && data[i + 1] == '\n') {
                i++; // Skip the following LF
            }
            command_start_index = i + 1; // Next chunk starts after the newline(s)

        } else { // Regular character
             // Echo back ONLY if the frame contained just this single character
             if (len == 1) { 
                 char echo_buf[2] = { current_char, '\0' };
                 send_ws_text_frame(sockfd, echo_buf);
             }
             // Accumulation will happen when newline is hit or at end of frame
        }
    }

    // === Append any remaining characters after the loop (no newline in this frame) ===
    size_t remaining_chars = len - command_start_index;
    if (remaining_chars > 0) {
        size_t needed_len = client->command_len + remaining_chars + 1; // +1 for null terminator
        // Resize buffer if needed
        if (client->buffer_capacity < needed_len) {
             size_t new_capacity = (client->buffer_capacity == 0) ? 256 : client->buffer_capacity * 2;
             while (new_capacity < needed_len) new_capacity *= 2;
             char *new_buffer = realloc(client->command_buffer, new_capacity);
             if (!new_buffer) {
                  ESP_LOGE(TAG, "Failed to realloc command buffer (tail append) for client %d", client_id);
                  client->command_len = 0; // Reset length
                  if (client->command_buffer) client->command_buffer[0] = '\0';
                  // Skip append if realloc fails
                  return; 
             }
             client->command_buffer = new_buffer;
             client->buffer_capacity = new_capacity;
        }
        // Append remaining data
        memcpy(client->command_buffer + client->command_len, &data[command_start_index], remaining_chars);
        client->command_len += remaining_chars;
        client->command_buffer[client->command_len] = '\0'; // Null terminate
        ESP_LOGD(TAG, "Appended %d tail chars. Buffer len %d: '%s'", 
                 (int)remaining_chars, (int)client->command_len, client->command_buffer ? client->command_buffer : "<NULL>");
    }

    ESP_LOGD(TAG, "REPL Input Handler End: Client %d", client_id);
}

#endif // USE_BERRY_WEBREPL 