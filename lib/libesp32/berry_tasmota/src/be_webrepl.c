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

// --- Initialize WebREPL-specific client data ---
void webrepl_init_client(int client_slot) {
    if (client_slot < 0 || client_slot >= MAX_WS_CLIENTS) {
        ESP_LOGE(TAG, "Invalid client slot %d in webrepl_init_client", client_slot);
        return;
    }
    
    ESP_LOGD(TAG, "Initializing/Resetting WebREPL-specific data for client slot %d", client_slot);

    // --- Explicitly free existing buffers FIRST ---
    if (ws_clients[client_slot].command_buffer) {
        ESP_LOGW(TAG, "webrepl_init_client: Freeing existing command_buffer for slot %d", client_slot);
        free(ws_clients[client_slot].command_buffer);
    }
    if (ws_clients[client_slot].line_buffer) {
         ESP_LOGW(TAG, "webrepl_init_client: Freeing existing line_buffer for slot %d", client_slot);
        free(ws_clients[client_slot].line_buffer);
    }
    // --- End Free ---

    // Initialize/Reset all WebREPL fields
    ws_clients[client_slot].command_buffer = NULL;
    ws_clients[client_slot].command_len = 0;         
    ws_clients[client_slot].buffer_capacity = 0;      
    ws_clients[client_slot].line_buffer = NULL;       
    ws_clients[client_slot].line_len = 0;             
    ws_clients[client_slot].line_capacity = 0;        
    memset(&ws_clients[client_slot].binop, 0, sizeof(ws_clients[client_slot].binop));
    ws_clients[client_slot].in_multiline = false;
    // NOTE: repl_state is initialized in add_client
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
     uint8_t chunk_buf[1024 + 2]; // Increased to 1KB chunk size + 2 bytes length prefix
     size_t bytes_read;

     // Protect against reading past intended size if specified (though GET usually doesn't specify size)
     uint32_t max_read = 1024;    // Increased to 1KB
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

    // <<< MODIFIED: Handle GET confirmations - check for specific 0x00 byte >>>
    // --- Handle GET_FILE data streaming ---
    if (op_state->active && op_state->hdr.op == WEBREPL_OP_GET_FILE && op_state->fp != NULL) {
        // This block is entered when the client sends the *initial* 0x00 confirmation
        // after the server has sent its first "WB OK" for the GET request.
        if (len == 1 && data[0] == 0x00 && !op_state->get_streaming_started) { // Check a new flag
            ESP_LOGI(TAG,"Client %d GET: Received initial 0x00 confirmation. Starting file stream for '%s'.", client_id, op_state->filename);
            op_state->get_streaming_started = true; // Mark that streaming has begun

            // Loop to send all file chunks
            bool send_success = true;
            while (send_success) {
                send_success = webrepl_send_file_chunk(client_id);
                if (!send_success) {
                    // webrepl_send_file_chunk handles fclose, final WB OK/ERROR, and op_state->active = false
                    ESP_LOGI(TAG,"Client %d GET: Finished streaming or error occurred for '%s'.", client_id, op_state->filename);
                    break; // Exit loop, operation is complete or failed
                }
                // Optional: Small yield if necessary for very large files and slow clients.
                // if (op_state->active) { // Only delay if op is still active (not finished by last send_file_chunk)
                //    vTaskDelay(pdMS_TO_TICKS(1)); // Minimal delay to allow other tasks, esp. network stack
                // }
            }
        } else if (op_state->get_streaming_started) {
            // If streaming has started, we don't expect any more binary data from the client for this GET op
            // until the server has signaled completion (which it does via webrepl_send_file_chunk sending a final WB response).
            ESP_LOGW(TAG,"Client %d GET: Received unexpected binary data (len %d) while streaming '%s'. Ignoring.",
                     client_id, (int)len, op_state->filename);
        } else {
            // Initial 0x00 not yet received, or unexpected data before it.
             ESP_LOGW(TAG,"Client %d GET: Waiting for initial 0x00 or received unexpected binary before it (len %d, data[0]=0x%02x) for '%s'. Ignoring.",
                      client_id, (int)len, (len > 0 ? data[0] : 0xFF), op_state->filename);
        }
        return; // Done handling this packet for GET_FILE
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
        op_state->get_streaming_started = false; // <<< INITIALIZE NEW FLAG
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
                 bytes_to_write = op_state->data_bytes_received - op_state->data_bytes_expected;
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

void send_ws_text_frame(int sockfd, const char* text) {
    ESP_LOGD(TAG, "Sending frame to socket %d: '%s'", sockfd, text);  
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.payload = (uint8_t*)text;
    frame.len = strlen(text);
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_send_frame_async(ws_server, sockfd, &frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send frame to sockfd %d: %s (%d)", 
                 sockfd, esp_err_to_name(ret), ret);
    } 
}

void send_ws_friendly_frame(int client_id, const char* text) {
    if (!is_client_valid(client_id) || !text) {
        ESP_LOGE(TAG, "Invalid parameters in send_ws_friendly_frame: client_id=%d, valid=%d, text=%p", 
                 client_id, is_client_valid(client_id), text);
        return;
    }

    // If the client is in RAW REPL mode, don't send friendly messages.
    if (ws_clients[client_id].repl_state == REPL_RAW) {
        ESP_LOGD(TAG, "Client %d in RAW mode, suppressing friendly msgs: '%s'", client_id, text);
        return; // Suppress output in RAW mode
    }

    int sockfd = ws_clients[client_id].sockfd;    
    send_ws_text_frame(sockfd, text);

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
static int webrepl_compile(bvm *vm, int client_id) {

    ws_client_t *client = &ws_clients[client_id];
    int original_stream_sockfd = g_stream_sockfd; // Store stream state

    // First try as an expression
    int res = try_return(vm, client->command_buffer);

    // Handle syntax errors (may be multi-line statements)
    if (be_getexcept(vm, res) == BE_SYNTAX_ERROR) {
        be_pop(vm, 2);  // Pop exception values
        be_pushstring(vm, client->command_buffer);
        // Attempt to compile the current source
        const char *src = be_tostring(vm, -1);  // Get source code
        int idx = be_absindex(vm, -1);  // Get source text absolute index
        /* compile source line */
        res = be_loadbuffer(vm, "webrepl", src, strlen(src));
        // Check if compilation succeeded or it's a non-multi-line error
        if (!res || !is_multline(vm)) {
            be_remove(vm, idx);  // Remove source code
            // If there's an error and it's not multi-line, handle RAW REPL error response
            if (res) {
                 // Temporarily disable streaming for error output
                g_stream_sockfd = -1;
                const char *error_msg = NULL;
                if (be_isstring(vm, -1)) { // Error message is on top
                    error_msg = be_tostring(vm, -1);
                }
                // Send empty stdout
                // send_ws_text_frame(client->sockfd, "", 0); // Not strictly needed if frontend handles missing output
                // Send first terminator (end of stdout)
                send_ws_text_frame(client->sockfd, "\x04");
                // Send stderr message
                if (error_msg) {
                    send_ws_text_frame(client->sockfd, error_msg);
                }
                // Send second terminator (end of stderr/command)
                send_ws_text_frame(client->sockfd, "\x04");

                be_pop(vm, 1); // Pop the error message/object
                g_stream_sockfd = original_stream_sockfd; // Restore stream state
            }
            // Reset multi-line state - either completed successfully or has error
            client->in_multiline = false;
            // Reset the command buffer
            client->command_len = 0;
            client->command_buffer[0] = '\0';
            return res; // Return the compilation result code
        }

        // This is a multi-line statement needing more input
        be_pop(vm, 2);  // Pop exception values

        // Set multi-line flag
        client->in_multiline = true;

        // Send continuation prompt (ONLY for multi-line, not an error)
        send_ws_friendly_frame(client_id, WEBREPL_CONTINUATION_PROMPT);

        // Remove the source code from stack - it's now in client->command_buffer
        be_remove(vm, idx);

        // Return special OK value to indicate waiting for more input
        return BE_OK; // Indicate multi-line continuation
    }

    // Expression evaluation succeeded or non-syntax error occurred
    if (res == BE_OK) {
        // If compilation succeeded, we don't print anything yet.
        // The compiled closure is on the stack, ready for execution.
        // The actual execution happens in the be_pcall block below.
    } else {
        // Non-syntax error (e.g., from try_return)
        // Temporarily disable streaming for error output
        g_stream_sockfd = -1;
        const char *error_msg = NULL;
        if (be_isstring(vm, -1)) { // Error message is on top
            error_msg = be_tostring(vm, -1);
        }
        // Send empty stdout
        // send_ws_text_frame(client->sockfd, "", 0); // Not strictly needed
        // Send first terminator (end of stdout)
        send_ws_text_frame(client->sockfd, "\x04");
        // Send stderr message
        if (error_msg) {
            send_ws_text_frame(client->sockfd, error_msg);
        }
        // Send second terminator (end of stderr/command)
        send_ws_text_frame(client->sockfd, "\x04");

        be_pop(vm, 1); // Pop the error message/object
        g_stream_sockfd = original_stream_sockfd; // Restore stream state

        // Reset buffer and multi-line state
        client->command_len = 0;
        client->command_buffer[0] = '\0';
        client->in_multiline = false;
    }

    return (res == BE_OK) ? 0 : res; // Return 0 for overall success or multi-line, else the error code from compile/exec
}

// Call the compiled script if it's on top of the stack
static int webrepl_call_script(bvm *vm, int client_id) {
    ws_client_t *client = &ws_clients[client_id];
    int res;

    // Store current stream sockfd, set it for this client during execution, then restore
    int original_stream_sockfd = g_stream_sockfd;
    g_stream_sockfd = client->sockfd; // Redirect VM output to this client's socket

    res = be_pcall(vm, 0);

    // If a runtime exception occurred, format and print it via the VM's output handler
    if (res == BE_EXCEPTION) {
        be_dumpexcept(vm); // Dumps the exception traceback using the configured output func
        // Exception object is still on the stack after dumping
        if (!be_isnil(vm, -1)) { // Make sure something is on the stack before popping
            be_pop(vm, 1); // Pop the exception object
        }
    } else if (res == BE_OK) {
        // If successful, check the result value
        // Note: Actual output from print() or expression eval was already streamed by the output handler
        if (!be_isnil(vm, -1)) {
            // Result is not nil, implicitly print it via the VM's output function
            const char* result_str = be_tostring(vm, -1); // Get string representation
            if (result_str) { // Check if conversion was successful
                 be_writestring(result_str); // Call the Berry API macro to write the string
                 be_writenewline();      // Call the Berry API macro to write a newline
            }
            be_pop(vm, 1); // Pop the result value
        }
        // If result was nil, do nothing (don't print nil)
    } else {
        // Other errors?
        ESP_LOGE(TAG, "Unexpected be_pcall result in webrepl_call_script: %d", res);
        // Potentially pop something here too? Need to know what state be_pcall leaves.
    }

    g_stream_sockfd = original_stream_sockfd; // Restore original stream redirection

    // Always send the terminators after execution/exception handling
    // Send first terminator (end of stdout/stderr stream)
    send_ws_text_frame(client->sockfd, "\x04");
    // Send second terminator (end of command)
    send_ws_text_frame(client->sockfd, "\x04");

    return res; // Return the original result code
}

// Helper function to compile and execute a Berry command
static int compile_and_execute_command(bvm *vm, int client_id)
{
    ws_client_t *client = &ws_clients[client_id];
    int res;

    // Try to compile the command buffer
    res = webrepl_compile(vm, client_id);

    // Only execute if compilation succeeded and it's not a multi-line statement
    // webrepl_compile handles prompts/errors/terminators otherwise
    if (res == BE_OK && !client->in_multiline) {
        // Call the script. It will handle its own output/error and terminators.
        res = webrepl_call_script(vm, client_id);
        // webrepl_call_script returns 0 for handled OK/Exception, or error code
        if (res != 0 && res != BE_OK && res != BE_EXCEPTION) {
            ESP_LOGE(TAG, "Execution error: %d", res);
            // Note: Terminators were already sent by webrepl_call_script
        }
        // Reset the command buffer after successful execution
        client->command_len = 0;
        client->command_buffer[0] = '\0';

    } else { // Compilation failed or multi-line
        // webrepl_compile handled prompts/output/terminators
    }

    return (res == BE_OK) ? 0 : res; // Return 0 for overall success or multi-line, else the error code from compile/exec
}

// Handles all incoming data for a client in REPL mode
void be_webrepl_handle_input(bvm *vm, int client_id, const char* data, size_t len) {

    if (!is_client_valid(client_id)) {
        ESP_LOGE(TAG, "Invalid client %d in be_webrepl_handle_input", client_id);
        return;
    }

    ws_client_t *client = &ws_clients[client_id];
    ESP_LOGD(TAG, "REPL Input Handler Start: Client %d, Mode: %s, len: %d", 
             client_id, (client->repl_state == REPL_RAW ? "RAW" : "Friendly"), len);

    if (client->repl_state == REPL_RAW) {
        // --- RAW REPL Mode Handling --- 
        
        // Append ALL incoming data to client->command_buffer.
        if (!ensure_command_buffer(client, client->command_len + len + 1)) { // +1 for null terminator
            ESP_LOGE(TAG, "RAW Mode: Failed to allocate command buffer for client %d", client_id);
            // Potentially send an error back or disconnect?
            return;
        }
        memcpy(client->command_buffer + client->command_len, data, len);
        client->command_len += len;
        client->command_buffer[client->command_len] = '\0'; // Ensure null termination

        ESP_LOGD(TAG, "RAW Mode: Buffer after append (len=%d): '%s'", client->command_len, client->command_buffer);

        // Check if command_buffer ends with "\n\x04".
        if (client->command_len >= 2 && 
            client->command_buffer[client->command_len - 2] == '\n' &&
            client->command_buffer[client->command_len - 1] == '\x04') 
        {
            ESP_LOGD(TAG, "RAW Mode: End sequence '\\n\\x04' detected for client %d", client_id);
            
            send_ws_text_frame(client->sockfd, "OK"); 

            client->command_buffer[client->command_len - 2] = '\0';
            client->command_len -= 2; // Adjust length

            ESP_LOGD(TAG, "RAW Mode: Executing command (len=%d): '%s'", client->command_len, client->command_buffer);
            
            compile_and_execute_command(vm, client_id);
            
            client->command_len = 0;
            client->command_buffer[0] = '\0';

        } else {
            // Sequence not found, keep accumulating
            ESP_LOGD(TAG, "RAW Mode: End sequence not found, accumulating...");
        }

    } else { 
        // --- Friendly REPL Mode Handling (Existing Logic) ---
        int initial_top = be_top(vm); // Stack check for friendly mode

        // Check for single-character control codes *first*
        if (len == 1) {
            char c = data[0];
            bool handled = true; 
            switch (c) {
                case 0x01: // Ctrl+A: Enter RAW REPL
                    ESP_LOGI(TAG, "Client %d: Entering RAW REPL mode (^A)", client_id);
                    client->repl_state = REPL_RAW;
                    // Clear buffers when switching mode
                    client->command_len = 0;
                    if (client->command_buffer) client->command_buffer[0] = '\0';
                    client->line_len = 0;
                    if (client->line_buffer) client->line_buffer[0] = '\0';
                    client->in_multiline = false;
                    send_ws_friendly_frame(client_id, "raw REPL; CTRL-B to exit\r\n"); 
                    break;
                case 0x02: // Ctrl+B: Enter Friendly REPL (already in it, but maybe reset state?)
                    ESP_LOGI(TAG, "Client %d: Resetting Friendly REPL mode (^B)", client_id);
                    client->repl_state = REPL_FRIENDLY; // Ensure state
                    // Clear buffers
                    client->command_len = 0;
                    if (client->command_buffer) client->command_buffer[0] = '\0';
                    client->line_len = 0;
                    if (client->line_buffer) client->line_buffer[0] = '\0';
                    client->in_multiline = false;
                    send_ws_friendly_frame(client_id, "\r\nOK\r\n" WEBREPL_PROMPT);
                    break;
                case 0x03: // Ctrl+C: Interrupt
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
                    send_ws_friendly_frame(client_id, "\r\n" WEBREPL_PROMPT);
                    break;
                case 0x04: // Ctrl+D: Soft reset / End of input (Friendly mode)
                    ESP_LOGI(TAG, "Client %d: Soft Reset / EOF received (^D)", client_id);
                if (client->repl_state == REPL_FRIENDLY) {
                    send_ws_friendly_frame(client_id, "\r\n" WEBREPL_PROMPT);
                }       
                

                // TODO: Add soft reset logic?
       
                    break;
                case 0x08:  // backspace
                case 0x7f:  // delete left
                    if (client->line_len > 0) {
                        client->line_len--;
                        client->line_buffer[client->line_len] = '\0';
                        // Echo backspace sequence to erase the last character
                        send_ws_friendly_frame(client_id, "\b \b");
                    }
                    break;
                default:
                    handled = false; 
                    break;
            }
            if (handled) {
                ESP_LOGD(TAG, "Friendly REPL Input Handler End (Control Char): Client %d", client_id);
                return; // Done handling control char
            }
        }
        
        // If len == 1 and not a handled control char, it's a regular char
        if (len == 1) { 
            char c = data[0];

            // Handle line termination (CR or LF)
            if (c == '\r' || c == '\n') {
                // Echo newline
                send_ws_friendly_frame(client_id, "\r\n");
                
                // Process the line if we have accumulated content or if we're in multi-line mode
                if (client->line_len > 0 || client->in_multiline) {
                    // Check for multi-line triggering or continuation
                    int indent_level = 0;
                    bool is_multiline_trigger = false;
                    if (client->line_len > 0) {
                        // Check indentation for multi-line start/continuation
                        for (int i = 0; i < client->line_len && (client->line_buffer[i] == ' ' || client->line_buffer[i] == '\t'); ++i) {
                            indent_level++;
                        }
                        // Basic trigger: ends with ':' or is indented
                        is_multiline_trigger = (client->line_buffer[client->line_len - 1] == ':') || (indent_level > 0);
                    }
                    
                    // Special handling for multi-line input
                    if (client->in_multiline) {
                        // Append the new line with a newline character
                        if (!ensure_command_buffer(client, client->command_len + client->line_len + 2)) { /* ... error handling ... */ return; }
                        if (client->command_len > 0) client->command_buffer[client->command_len++] = '\n';
                        if (client->line_len > 0) {
                             memcpy(client->command_buffer + client->command_len, client->line_buffer, client->line_len);
                            client->command_len += client->line_len;
                        }
                         client->command_buffer[client->command_len] = '\0';

                         if (client->line_len == 0 || indent_level == 0) {
                             client->in_multiline = false;
                            compile_and_execute_command(vm, client_id );
                        } else {
                             send_ws_friendly_frame(client_id, WEBREPL_CONTINUATION_PROMPT);
                        }
                    } else if (is_multiline_trigger) {
                         client->in_multiline = true;
                        if (!ensure_command_buffer(client, client->line_len + 2)) { /* ... error handling ... */ return; }
                        memcpy(client->command_buffer, client->line_buffer, client->line_len);
                        client->command_len = client->line_len;
                        client->command_buffer[client->command_len] = '\0';
                        send_ws_friendly_frame(client_id, WEBREPL_CONTINUATION_PROMPT);
                    } else {
                        // Normal single-line processing
                        // Copy line buffer to command buffer
                        if (!ensure_command_buffer(client, client->line_len + 1)) { /* ... error handling ... */ return; }
                        memcpy(client->command_buffer, client->line_buffer, client->line_len);
                        client->command_len = client->line_len;
                        client->command_buffer[client->command_len] = '\0';
                        
                        // Execute single line command
                        compile_and_execute_command(vm, client_id );
                    }
                    
                    // Reset line buffer for next line/command
                    client->line_len = 0;
                    client->line_buffer[0] = '\0';

                } else { // Empty line received
                     if (client->in_multiline) {
                        client->in_multiline = false;
                        compile_and_execute_command(vm, client_id);
                    } else {
                         send_ws_friendly_frame(client_id, WEBREPL_PROMPT);
                    }
                }
                
                return; // Handled line termination
            }
            
            // Regular character, append to line buffer and echo
            if (client->line_len < (MAX_LINE_LENGTH - 1)) {
                client->line_buffer[client->line_len++] = c;
                client->line_buffer[client->line_len] = '\0';
                
                // Echo the character back
                char echo[2] = {c, '\0'};
                send_ws_friendly_frame(client_id, echo);
            } else {
                 ESP_LOGW(TAG, "Line buffer overflow for client %d", client_id);
                 // Optionally send a bell character or error?
            }
            
            return; // Handled single regular char append
        }
        
        // --- Handle normal multi-byte line input (pasted text) --- 
        // This part remains largely the same, processing line-by-line based on \r\n
        const char *current = data;
        const char *end = data + len;
        
        while (current < end) {
            // Find the next line terminator
            const char *line_end = current;
            while (line_end < end && *line_end != '\r' && *line_end != '\n') {
                line_end++;
            }
            
            size_t current_line_len = line_end - current;
            
            // Append the current segment to the line buffer
            if (client->line_len + current_line_len < (MAX_LINE_LENGTH - 1)) {
                memcpy(client->line_buffer + client->line_len, current, current_line_len);
                client->line_len += current_line_len;
                client->line_buffer[client->line_len] = '\0';
            } else {
                ESP_LOGW(TAG, "Line buffer overflow during paste for client %d", client_id);
                 // Truncate and proceed?
                 size_t space_left = (MAX_LINE_LENGTH - 1) - client->line_len;
                 if (space_left > 0) {
                     memcpy(client->line_buffer + client->line_len, current, space_left);
                     client->line_len += space_left;
                     client->line_buffer[client->line_len] = '\0';
                 }
                 // How to handle the rest of the pasted data? Discard for now.
                 current = end; // Skip rest of the pasted data in this chunk
                 // Maybe send an error message?
            }

            // If we found a line terminator, process the line buffer
            if (line_end < end && (*line_end == '\r' || *line_end == '\n')) {
                // Echo newline
                send_ws_friendly_frame(client_id, "\r\n");
                
                 // Process the line (similar logic as single char line termination)
                if (client->line_len > 0 || client->in_multiline) {
                     int indent_level = 0;
                    bool is_multiline_trigger = false;
                     if (client->line_len > 0) {
                        for (int i = 0; i < client->line_len && (client->line_buffer[i] == ' ' || client->line_buffer[i] == '\t'); ++i) {
                            indent_level++;
                        }
                        is_multiline_trigger = (client->line_buffer[client->line_len - 1] == ':') || (indent_level > 0);
                    }

                    if (client->in_multiline) {
                        if (!ensure_command_buffer(client, client->command_len + client->line_len + 2)) { /* ... error handling ... */ return; }
                        if (client->command_len > 0) client->command_buffer[client->command_len++] = '\n';
                        if (client->line_len > 0) {
                             memcpy(client->command_buffer + client->command_len, client->line_buffer, client->line_len);
                            client->command_len += client->line_len;
                        }
                         client->command_buffer[client->command_len] = '\0';

                         if (client->line_len == 0 || indent_level == 0) {
                             client->in_multiline = false;
                            compile_and_execute_command(vm, client_id );
                        } else {
                             send_ws_friendly_frame(client_id, WEBREPL_CONTINUATION_PROMPT);
                        }
                    } else if (is_multiline_trigger) {
                         client->in_multiline = true;
                        if (!ensure_command_buffer(client, client->line_len + 2)) { /* ... error handling ... */ return; }
                        memcpy(client->command_buffer, client->line_buffer, client->line_len);
                        client->command_len = client->line_len;
                        client->command_buffer[client->command_len] = '\0';
                        send_ws_friendly_frame(client_id, WEBREPL_CONTINUATION_PROMPT);
                    } else {
                        if (!ensure_command_buffer(client, client->line_len + 1)) { /* ... error handling ... */ return; }
                        memcpy(client->command_buffer, client->line_buffer, client->line_len);
                        client->command_len = client->line_len;
                        client->command_buffer[client->command_len] = '\0';
                        compile_and_execute_command(vm, client_id);
                    }
                } else { // Empty line received
                     if (client->in_multiline) {
                        client->in_multiline = false;
                        compile_and_execute_command(vm, client_id);
                    } else {
                         send_ws_friendly_frame(client_id, WEBREPL_PROMPT);
                    }
                }
                
                // Reset line buffer for next line
                client->line_len = 0;
                client->line_buffer[0] = '\0';
                
                // Skip past line terminator(s)
                current = line_end + 1;
                if (current < end && *line_end == '\r' && *current == '\n') {
                    current++;  // Skip LF in CRLF sequence
                }
            } else {
                // No line terminator found in this chunk, advance current
                current = line_end;
            }
        } // End while (current < end)

        // Check for stack balance after friendly mode processing
        if (be_top(vm) != initial_top) {
            ESP_LOGE(TAG, "Stack imbalance after Friendly REPL handling! Top: %d, Expected: %d",
                     be_top(vm), initial_top);
            be_pop(vm, be_top(vm) - initial_top);  // Restore stack balance
        }
    } // End else (Friendly REPL Mode)
}

static void webrepl_log_output_handler(const char* output) {
    if (g_stream_sockfd >= 0) {
        ESP_LOGI(TAG, "webrepl_log_output_handler: Sending to sockfd %d: '%s'", g_stream_sockfd, output ? output : "(null)"); // Log handler activity
        send_ws_text_frame(g_stream_sockfd, output);
    } else {
        // ESP_LOGD(TAG, "webrepl_log_output_handler: Stream sockfd not set, discarding: %s", output); // Optional: Log discarded output
    }
}

#endif // USE_BERRY_WEBREPL