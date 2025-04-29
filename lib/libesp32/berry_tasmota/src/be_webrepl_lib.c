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


#include <string.h>

// ESP-IDF / FreeRTOS includes 
#include "esp_log.h"
#include "esp_err.h" 
#include "freertos/FreeRTOS.h" 

// Berry includes
#include "be_vm.h"
#include "be_exec.h"


#include "be_webrepl.h" 

#include "include/tasmota_version.h"        // Tasmota version information


#define TAG "WEBREPL"


// --- WebREPL Specific Constants ---
#define WEBREPL_PROMPT "> "
#define WEBREPL_CONTINUATION_PROMPT "... "
#define WEBREPL_PASSWORD_PROMPT "Password: "
static const char* webrepl_password = "password"; // CHANGE THIS!

// --- Initialize WebREPL-specific client data ---
void webrepl_init_client(int client_slot) {
    if (client_slot < 0 || client_slot >= MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid client slot %d in webrepl_init_client", client_slot);
        return;
    }
    
    ws_clients[client_slot].command_buffer = NULL;    // Initialized to NULL
    ws_clients[client_slot].command_len = 0;          // Initialized to 0
    ws_clients[client_slot].buffer_capacity = 0;      // Initialized to 0
    ws_clients[client_slot].line_buffer = NULL;       // Initialize line buffer for char-by-char input
    ws_clients[client_slot].line_len = 0;             // Initialize line length
    ws_clients[client_slot].line_capacity = 0;        // Initialize line buffer capacity
    memset(&ws_clients[client_slot].binop, 0, sizeof(ws_clients[client_slot].binop)); // Initialize binary op state
    ESP_LOGD(TAG, "Initialized WebREPL-specific data for client %d", client_slot);
}

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
     // webREPL client might send a single byte (often 0x00) after receiving
     // a data chunk to signal readiness for the next one. current send logic
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


// cf be_repl.c's is_multline function
static bbool is_multline(bvm *vm) {
    const char *msg = be_tostring(vm, -1);
    ESP_LOGD(TAG, "Error message: '%s'", msg);
    
    // All incomplete statements will have "unexpected 'EOF'" in the error message
    // This is more robust than checking for specific constructs
    if (strstr(msg, "unexpected 'EOF'") != NULL) {
        return btrue;
    }
    
    // Original EOS check as a fallback
    size_t len = strlen(msg);
    if (len > 5 && !strcmp(msg + len - 5, "'EOS'")) {
        return btrue;
    }
    
    return bfalse;
}

// be_repl.c's try_return function
static int try_return(bvm *vm, const char *line) {
    int res, idx;
    line = be_pushfstring(vm, "return (%s)", line);
    idx = be_absindex(vm, -1);  // Get the source text absolute index
    res = be_loadbuffer(vm, "webrepl", line, strlen(line));  // Compile line
    be_remove(vm, idx);  // Remove source string
    return res;
}

// buffer management (WebREPL specific)
static bool ensure_command_buffer(ws_client_t *client, size_t additional_len) {
    if (!client) return false;
    
    // Calculate required capacity
    size_t required_capacity = client->command_len + additional_len;
    
    // Check if we need to allocate or resize
    if (!client->command_buffer) {
        // First allocation, provide reasonable starting size
        size_t initial_capacity = required_capacity > 256 ? required_capacity : 256;
        client->command_buffer = malloc(initial_capacity);
        if (!client->command_buffer) return false;
        client->buffer_capacity = initial_capacity;
        client->command_len = 0;
        client->command_buffer[0] = '\0';
        client->in_multiline = false;
    } else if (client->buffer_capacity < required_capacity) {
        // Need to resize
        size_t new_capacity = client->buffer_capacity * 2;
        while (new_capacity < required_capacity) new_capacity *= 2;
        
        char *new_buffer = realloc(client->command_buffer, new_capacity);
        if (!new_buffer) return false;
        
        client->command_buffer = new_buffer;
        client->buffer_capacity = new_capacity;
    }
    
    return true;
}

// Initialize client line buffer for character-by-character input
static bool ensure_line_buffer(ws_client_t *client, size_t additional_len) {
    if (!client) return false;
    
    // Calculate required capacity
    size_t required_capacity = client->line_len + additional_len;
    
    // Check if we need to allocate or resize
    if (!client->line_buffer) {
        // First allocation, provide reasonable starting size
        size_t initial_capacity = required_capacity > 128 ? required_capacity : 128;
        client->line_buffer = malloc(initial_capacity);
        if (!client->line_buffer) return false;
        client->line_capacity = initial_capacity;
        client->line_len = 0;
        client->line_buffer[0] = '\0';
    } else if (client->line_capacity < required_capacity) {
        // Need to resize
        size_t new_capacity = client->line_capacity * 2;
        while (new_capacity < required_capacity) new_capacity *= 2;
        
        char *new_buffer = realloc(client->line_buffer, new_capacity);
        if (!new_buffer) return false;
        
        client->line_buffer = new_buffer;
        client->line_capacity = new_capacity;
    }
    
    return true;
}

// cf be_repl.c's compile function
static int webrepl_compile(bvm *vm, ws_client_t *client, int sockfd) {
    // First try as an expression
    int res = try_return(vm, client->command_buffer);
    
    // Handle syntax errors (may be multi-line statements)
    if (be_getexcept(vm, res) == BE_SYNTAX_ERROR) {
        be_pop(vm, 2);  // Pop exception values
        be_pushstring(vm, client->command_buffer);
        // Attempt to compile the current source
        const char *src = be_tostring(vm, -1);  // Get source code
        int idx = be_absindex(vm, -1);  // Get source text absolute index
        res = be_loadbuffer(vm, "webrepl", src, strlen(src));
        // Check if compilation succeeded or it's a non-multi-line error
        if (!res || !is_multline(vm)) {
            be_remove(vm, idx);  // Remove source code
            // If there's an error and it's not multi-line, dump it
            if (res) {
                be_dumpexcept(vm);
            }
            // Reset multi-line state - either completed successfully or has error
            client->in_multiline = false;
            // Reset the command buffer
            client->command_len = 0;
            client->command_buffer[0] = '\0';          
            return res;
        }
        
        // This is a multi-line statement needing more input
        be_pop(vm, 2);  // Pop exception values
        
        // Set multi-line flag
        client->in_multiline = true;
        
        // Send continuation prompt
        send_ws_text_frame(sockfd, WEBREPL_CONTINUATION_PROMPT);
        
        // Remove the source code from stack - it's now in client->command_buffer
        be_remove(vm, idx);
        
        // Return special OK value to indicate waiting for more input
        return BE_OK;
    }
    
    // Expression evaluation succeeded or non-syntax error occurred
    if (res == BE_OK) {
        // Reset buffer and multi-line state
        client->command_len = 0;
        client->command_buffer[0] = '\0';
        client->in_multiline = false;
        
    } else {
        // Non-syntax error
        be_dumpexcept(vm);
        
        // Reset buffer and multi-line state
        client->command_len = 0;
        client->command_buffer[0] = '\0';
        client->in_multiline = false;
        
        // Send normal prompt
        send_ws_text_frame(sockfd, WEBREPL_PROMPT);
    }
    
    return res;
}

// cf be_repl.c's call_script function
static int webrepl_call_script(bvm *vm, int sockfd) {

    int res = be_pcall(vm, 0);  // Call the main function

    // if function printed anything, need to add a newline
    if (g_streamed){
        g_streamed = false;
        send_ws_text_frame(sockfd, "\r\n");
    }

    switch (res) {
        case BE_OK: /* execution succeed */
            // First check the actual result value which is on the stack for expressions
            if (!be_isnil(vm, -1)) { // if output from command, eg 3+4
                const char *result = be_tostring(vm, -1);
                if (result && *result) {
                    // send_ws_text_frame(sockfd, "  ");
                    send_ws_text_frame(sockfd, result);
                    send_ws_text_frame(sockfd, "\r\n");
                }
            } 
            be_pop(vm, 1);  // Pop result value
            break;
        case BE_EXCEPTION: /* vm run error */
            // Maybe a 'return' expression error
            be_dumpexcept(vm);
            be_pop(vm, 1); /* pop the function value */
            break;
        default: /* BE_EXIT or BE_MALLOC_FAIL */
            return res;
    }   
    // Send prompt immediately after execution
    send_ws_text_frame(sockfd, WEBREPL_PROMPT);
    return 0;
}

// Helper function to compile and execute a Berry command
static int compile_and_execute_command(bvm *vm, ws_client_t *client, int sockfd) {
    if (!vm || !client || sockfd < 0) {
        ESP_LOGE(TAG, "Invalid parameters in compile_and_execute_command");
        return -1;
    }
    g_stream_sockfd = sockfd;   // enable print streaming to ws
    g_streamed = false;

    // Compile the command - this will update client->in_multiline as needed
    int res = webrepl_compile(vm, client, sockfd);
    
    // Only execute if compilation succeeded and not in multi-line mode
    if (res == BE_OK && !client->in_multiline) {
        res = webrepl_call_script(vm, sockfd);
        if (res) {
            ESP_LOGE(TAG, "Execution error: %d", res);
        }
    }
    g_stream_sockfd = -1;

    return res;
}

// Handles all incoming data for a client in REPL mode
void be_webrepl_handle_input(bvm *vm, int client_id, const char* data, size_t len) {
    // Basic validation
    if (!vm || !data || client_id < 0 || client_id >= MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid parameters in be_webrepl_handle_input: vm=%p, data=%p, client_id=%d", 
                 vm, data, client_id);
        return;
    }
    if (!is_client_valid(client_id)) {
        ESP_LOGE(TAG, "Invalid client %d in be_webrepl_handle_input", client_id);
        return;
    }
    int sockfd = ws_clients[client_id].sockfd;
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Invalid sockfd for client %d in be_webrepl_handle_input", client_id);
        return;
    }

    ESP_LOGD(TAG, "REPL Input Handler: Client %d, Socket %d, Data len = %d", 
             client_id, sockfd, (int)len);

    ws_client_t *client = &ws_clients[client_id];
    
    // --- Handle special control characters ---
    if (len == 1 && data[0] == 0x03) { // Ctrl+C
        ESP_LOGI(TAG, "Client %d: Interrupt received (^C)", client_id);
        
        // Reset the client's command buffer if it exists
        if (client->command_buffer) {
            client->command_len = 0;
            client->command_buffer[0] = '\0';
            client->in_multiline = false;
        }
        
        // Reset line buffer too
        if (client->line_buffer) {
            client->line_len = 0;
            client->line_buffer[0] = '\0';
        }
        
        // Send prompt
        send_ws_text_frame(sockfd, "\r\n" WEBREPL_PROMPT);
        return;
    }
    
    // --- Handle character-by-character input ---
    // If we received a single character (typical of interactive WebREPL clients)
    if (len == 1) {
        // Ensure we have a line buffer
        if (!ensure_line_buffer(client, 2)) {  // +2 for the char and null terminator
            ESP_LOGE(TAG, "Failed to allocate line buffer for client %d", client_id);
            return;
        }
        
        char c = data[0];
        
        // Handle backspace/delete
        if (c == 0x08 || c == 0x7F) {
            if (client->line_len > 0) {
                client->line_len--;
                client->line_buffer[client->line_len] = '\0';
                // Echo backspace sequence to erase the last character
                send_ws_text_frame(sockfd, "\b \b");
            }
            return;
        }
        
        // Handle line termination (CR or LF)
        if (c == '\r' || c == '\n') {
            // Echo newline
            send_ws_text_frame(sockfd, "\r\n");
            
            // Process the line if we have accumulated content or if we're in multi-line mode
            if (client->line_len > 0 || client->in_multiline) {
                // Special handling for multi-line input
                if (client->in_multiline) {
                    // For multi-line, we need to append the new line with a newline character
                    if (!ensure_command_buffer(client, client->command_len + client->line_len + 2)) {
                        ESP_LOGE(TAG, "Failed to allocate command buffer for client %d", client_id);
                        return;
                    }
                    
                    // Append a newline first if we have existing content in command buffer
                    if (client->command_len > 0) {
                        client->command_buffer[client->command_len++] = '\n';
                    }
                    
                    // Then append the accumulated line
                    if (client->line_len > 0) {
                        memcpy(client->command_buffer + client->command_len, client->line_buffer, client->line_len);
                        client->command_len += client->line_len;
                    }
                    client->command_buffer[client->command_len] = '\0';
                } else {
                    // Normal single-line processing
                    // Copy line buffer to command buffer
                    if (!ensure_command_buffer(client, client->line_len + 1)) {
                        ESP_LOGE(TAG, "Failed to allocate command buffer for client %d", client_id);
                        return;
                    }
                    
                    memcpy(client->command_buffer, client->line_buffer, client->line_len);
                    client->command_len = client->line_len;
                    client->command_buffer[client->command_len] = '\0';
                }
                
                // Reset line buffer for next line
                client->line_len = 0;
                client->line_buffer[0] = '\0';
                
                // Use the common helper function to compile and execute
                compile_and_execute_command(vm, client, sockfd);
            } else {
                // Empty line, just show prompt if not in multi-line mode
                if (!client->in_multiline) {
                    send_ws_text_frame(sockfd, WEBREPL_PROMPT);
                } else {
                    send_ws_text_frame(sockfd, WEBREPL_CONTINUATION_PROMPT);
                }
            }
            
            return;
        }
        
        // Regular character, append to line buffer and echo
        client->line_buffer[client->line_len++] = c;
        client->line_buffer[client->line_len] = '\0';
        
        // Echo the character back
        char echo[2] = {c, '\0'};
        send_ws_text_frame(sockfd, echo);
        
        return;
    }
    
    // --- Handle normal line-by-line input (not character-by-character) ---
    const char *current = data;
    const char *end = data + len;
    int initial_top = be_top(vm);
    
    while (current < end) {
        // Find the next line terminator
        const char *line_end = current;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        
        // Process this line if it's not empty
        size_t line_len = line_end - current;
        
        // Special handling for multi-line continuation
        if (client->in_multiline) {
            // For multi-line, we need to append the new line with a newline character
            if (!ensure_command_buffer(client, client->command_len + line_len + 2)) {
                ESP_LOGE(TAG, "Failed to allocate command buffer for client %d", client_id);
                return;
            }
            
            // Append a newline first if we have existing content
            if (client->command_len > 0) {
                client->command_buffer[client->command_len++] = '\n';
            }
            
            // Then append the new line
            if (line_len > 0) {
                memcpy(client->command_buffer + client->command_len, current, line_len);
                client->command_len += line_len;
            }
            client->command_buffer[client->command_len] = '\0';
            
            // Echo newline
            send_ws_text_frame(sockfd, "\r\n");
        } else {
            // Normal single-line processing
            if (line_len > 0 || (line_end < end)) {  // Non-empty line or empty line with terminator
                // Ensure command buffer has enough space
                if (!ensure_command_buffer(client, line_len + 2)) {  // +2 for newline and null terminator
                    ESP_LOGE(TAG, "Failed to allocate command buffer for client %d", client_id);
                    return;
                }
                
                // Append line to buffer
                if (line_len > 0) {
                    memcpy(client->command_buffer + client->command_len, current, line_len);
                    client->command_len += line_len;
                    client->command_buffer[client->command_len] = '\0';
                }
                
                // Echo newline
                send_ws_text_frame(sockfd, "\r\n");
            }
        }
        
        // If we have a line terminator, process the command
        if (line_end < end && (*line_end == '\r' || *line_end == '\n')) {
            // Use the common helper function to compile and execute
            compile_and_execute_command(vm, client, sockfd);
            
            // Skip past line terminator(s)
            current = line_end + 1;
            if (current < end && *line_end == '\r' && *current == '\n') {
                current++;  // Skip LF in CRLF sequence
            }
        } else {
            // No line terminator, means we've processed all data
            current = line_end;
        }
    }
    
    // Check for stack balance
    if (be_top(vm) != initial_top) {
        ESP_LOGE(TAG, "Stack imbalance after REPL handling! Top: %d, Expected: %d",
                 be_top(vm), initial_top);
        be_pop(vm, be_top(vm) - initial_top);  // Restore stack balance
    }
}

#endif // USE_BERRY_WEBREPL 