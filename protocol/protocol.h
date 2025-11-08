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
#define FILE_BUFFER_SIZE 4096
#define MAX_WORD_CONTENT 100 // For WRITE updates

// --- 2. MESSAGE TYPES ---
typedef enum {
    // Phase 1
    REQ_CLIENT_REGISTER,
    REQ_SS_REGISTER,
    REQ_SS_FILE_ITEM,
    
    RES_OK,
    RES_ERROR,
    
    // Phase 2: CREATE
    REQ_CREATE,
    REQ_SS_CREATE,
    RES_ERROR_FILE_EXISTS,
    
    // Phase 2: READ
    REQ_READ,
    RES_READ_LOCATION,
    REQ_CLIENT_READ,
    RES_ERROR_NOT_FOUND,
    RES_SS_FILE_OK,

    // --- Phase 3: WRITE ---
    REQ_WRITE,              // Client -> NM (like REQ_READ)
    REQ_CLIENT_WRITE,       // Client -> SS (to request lock)
    RES_OK_LOCKED,          // SS -> Client
    RES_ERROR_LOCKED,       // SS -> Client
    REQ_WRITE_UPDATE,       // Client -> SS (sends one word update)
    REQ_ETIRW               // Client -> SS (ends write session)

} MessageType;

// --- 3. HEADER STRUCT ---
typedef struct {
    MessageType type;
    int payload_size;
} Header;

// --- 4. MESSAGE PAYLOADS (Structs) ---

// (Phase 1 Structs)
typedef struct { char username[MAX_USERNAME]; } Msg_Client_Register;
typedef struct { char ss_ip[MAX_IP_LEN]; int client_port; int file_count; } Msg_SS_Register;
typedef struct { char filename[MAX_FILENAME]; } Msg_File_Item;

// (Phase 2 Structs)
typedef struct { char filename[MAX_FILENAME]; } Msg_Filename_Request;
typedef struct { char ss_ip[MAX_IP_LEN]; int ss_port; } Msg_Read_Response;

// --- Phase 3 Structs ---

// Client -> SS: Initial request to lock a file for writing
typedef struct {
    char filename[MAX_FILENAME];
    int sentence_num;
} Msg_Client_Write;

// Client -> SS: A single update during a write session
typedef struct {
    int word_index;
    char content[MAX_WORD_CONTENT];
} Msg_Write_Update;


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