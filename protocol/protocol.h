#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// --- 1. GLOBAL CONSTANTS ---
#define NM_PORT 8080
#define MAX_USERNAME 50
#define MAX_IP_LEN 16
#define MAX_FILENAME 100
#define MAX_FILES_PER_SS 100
#define MAX_FILES_IN_SYSTEM 1024
#define FILE_BUFFER_SIZE 4096 // For sending file data

// --- 2. MESSAGE TYPES ---
typedef enum {
    // Phase 1
    REQ_CLIENT_REGISTER,
    REQ_SS_REGISTER,
    REQ_SS_FILE_ITEM,
    
    RES_OK,
    RES_ERROR, // Generic error
    
    // --- Phase 2: CREATE ---
    REQ_CREATE,            // Client -> NM
    REQ_SS_CREATE,         // NM -> SS
    RES_ERROR_FILE_EXISTS, // NM -> Client
    
    // --- Phase 2: READ ---
    REQ_READ,              // Client -> NM
    RES_READ_LOCATION,     // NM -> Client
    REQ_CLIENT_READ,       // Client -> SS
    RES_ERROR_NOT_FOUND,   // NM -> Client OR SS -> Client
    
    // --- Handshake for READ ---
    RES_SS_FILE_OK         // SS -> Client (Confirms file is found and data is coming)

} MessageType;

// --- 3. HEADER STRUCT ---
typedef struct {
    MessageType type;
    int payload_size;
} Header;

// --- 4. MESSAGE PAYLOADS (Structs) ---

// --- Phase 1 Structs ---
typedef struct {
    char username[MAX_USERNAME];
} Msg_Client_Register;

typedef struct {
    char ss_ip[MAX_IP_LEN];
    int client_port;
    int file_count;
} Msg_SS_Register;

typedef struct {
    char filename[MAX_FILENAME];
} Msg_File_Item;

// --- Phase 2 Structs ---

// A generic message for any request that just needs a filename
// Used for: REQ_CREATE, REQ_READ, REQ_SS_CREATE, REQ_CLIENT_READ
typedef struct {
    char filename[MAX_FILENAME];
} Msg_Filename_Request;


// The response from NM for a READ request
// Used for: RES_READ_LOCATION
typedef struct {
    char ss_ip[MAX_IP_LEN];
    int ss_port;
} Msg_Read_Response;


// --- 5. HELPER FUNCTION ---
static inline void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// Helper to send just a header
static inline void send_simple_header(int sock, MessageType type) {
    Header header;
    header.type = type;
    header.payload_size = 0;
    if (send(sock, &header, sizeof(Header), 0) < 0) {
        perror("send_simple_header");
    }
}

#endif //PROTOCOL_H