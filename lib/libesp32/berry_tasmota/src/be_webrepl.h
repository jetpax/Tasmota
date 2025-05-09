/*
  be_webrepl_lib.h - WebREPL implementation for Berry using the WebSocket server

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

#ifndef BE_WEBREPL_H_
#define BE_WEBREPL_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h> // For size_t
#include <stdio.h>  // For FILE*

#include "esp_http_server.h" // For httpd_handle_t

// Max number of concurrent clients (shared constant)
#define MAX_WS_CLIENTS 5

// REPL Mode constants
#define REPL_OFF 0
#define REPL_FRIENDLY 1
#define REPL_RAW 2

// --- Binary Protocol definitions ---
// Mirroring MicroPython's WebREPL binary protocol header
typedef struct __attribute__((packed)) { // Use packed to match potential uPy layout
    char sig[2];        // Should be 'W', 'B'
    uint8_t op;         // 1=PUT_FILE, 2=GET_FILE
    uint8_t flags;      // Currently unused?
    uint64_t offset;    // File offset for PUT/GET (Little Endian)
    uint32_t size;      // File size for PUT (Little Endian)
    uint16_t fname_len; // Length of filename (Little Endian)
    // Filename follows immediately
} webrepl_binhdr_t;

#define WEBREPL_HDR_SIG "WB"
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
    bool get_streaming_started; // True if initial 0x00 for GET received and server is streaming
} webrepl_binop_state_t;

// --- Client Tracking Structure (Shared) ---
typedef struct {
    int client_id;              // equivalent to the slot
    int sockfd;                 // socket assigned by sysyem
    bool active;
    int64_t last_activity;      // Timestamp of any client activity in milliseconds
    int repl_state;             // REPL mode: 0=OFF, 1=FRIENDLY, 2=RAW
    char *command_buffer;       // Dynamic buffer
    size_t command_len;         // Current length in buffer
    size_t buffer_capacity;     // Allocated capacity
    bool in_multiline;          // Flag indicating multi-line code block being processed
    char *line_buffer;          // Buffer for handling character-by-character input
    size_t line_len;            // Current length of line buffer
    size_t line_capacity;       // Total capacity of line buffer
    webrepl_binop_state_t binop;
} ws_client_t;

// --- Extern Declarations for Shared Globals/Functions (defined in be_wsserver_lib.c) ---

// Defined in be_wsserver_lib.c
extern ws_client_t ws_clients[MAX_WS_CLIENTS];
extern httpd_handle_t ws_server;
extern volatile int g_stream_sockfd;
extern volatile bool g_streamed;

// Prototypes for functions defined in be_wsserver_lib.c
extern bool is_client_valid(int client_id);
extern void handle_client_disconnect(int client_slot);
extern void send_ws_text_frame(int sockfd, const char* text);
extern void send_ws_friendly_frame(int client_id, const char* text);

// Prototypes for functions defined in be_webrepl_lib.c
extern void webrepl_init_client(int client_slot);
extern void be_webrepl_handle_input(bvm *vm, int client_id, const char* code, size_t len);
extern void be_webrepl_handle_binary(bvm *vm, int client_id, const uint8_t* data, size_t len);

#define WEBREPL_REQ_S_PASS "Password:" // "Password:\r\n"
#define WEBREPL_PROMPT ">>> "
#define WEBREPL_CONTINUATION_PROMPT "... "

#define MAX_LINE_LENGTH 256  // Maximum length for a single line input
#define MAX_COMMAND_LENGTH 1024 // Maximum length for accumulated multi-line command

// REPL State enum

#endif // BE_WEBREPL_H_