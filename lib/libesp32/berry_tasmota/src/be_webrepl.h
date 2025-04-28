#ifndef BE_WEBREPL_H_
#define BE_WEBREPL_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h> // For size_t
#include <stdio.h>  // For FILE*

#include "esp_http_server.h" // For httpd_handle_t

// Max number of concurrent clients (shared constant)
#define MAX_WS_CLIENTS 5

// --- Binary Protocol definitions ---
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

// --- Client State Enum ---
typedef enum {
    WS_STATE_INIT,       // Just connected, before prompt/trigger check
    WS_STATE_PASSWORD,   // Sent prompt, awaiting password
    WS_STATE_REPL,       // Password OK, processing WebREPL commands
    WS_STATE_NORMAL_APP  // Determined not to be WebREPL
} ws_client_state_t;

// --- Client Tracking Structure (Shared) ---
typedef struct {
    int sockfd;
    bool active;
    int64_t last_activity;  // Timestamp of any client activity in milliseconds
    ws_client_state_t state; // Client state for WebREPL/Normal App
    char *command_buffer;       // Dynamic buffer
    size_t command_len;         // Current length in buffer
    size_t buffer_capacity;     // Allocated capacity
    webrepl_binop_state_t binop;
} ws_client_t;

// --- Extern Declarations for Shared Globals/Functions (defined in be_wsserver_lib.c) ---

// Defined in be_wsserver_lib.c
extern ws_client_t ws_clients[MAX_WS_CLIENTS];
extern httpd_handle_t ws_server;
extern volatile int g_stream_sockfd;

// Prototypes for functions defined in be_wsserver_lib.c
extern bool is_client_valid(int client_id);
extern void handle_client_disconnect(int client_slot);
extern void send_ws_text_frame(int sockfd, const char* text);
extern void send_ws_text_frame_to_client(int client_id, const char* text);


#endif // BE_WEBREPL_H_ 