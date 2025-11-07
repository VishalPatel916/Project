#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// --- 1. GLOBAL CONSTANTS ---
#define NM_PORT 8080       // Public, well-known port for the Name Server
#define MAX_USERNAME 50
#define MAX_IP_LEN 16      // e.g., "192.168.100.100\0"
#define MAX_FILENAME 100
#define MAX_FILES_PER_SS 100 // Max files a single SS can report
#define MAX_FILES_IN_SYSTEM 1024 // Max files in the entire NM catalog

// --- 2. MESSAGE TYPES ---
typedef enum {
    REQ_CLIENT_REGISTER, // Client -> NM
    REQ_SS_REGISTER,     // SS -> NM
    REQ_SS_FILE_ITEM,    // SS -> NM (Sent N times after REQ_SS_REGISTER)
    
    RES_OK,              // NM -> Client/SS (Generic success)
    RES_ERROR            // NM -> Client/SS (Generic failure)
} MessageType;

// --- 3. HEADER STRUCT ---
typedef struct {
    MessageType type;
    int payload_size;
} Header;

// --- 4. MESSAGE PAYLOADS (Structs) ---
typedef struct {
    char username[MAX_USERNAME];
} Msg_Client_Register;

typedef struct {
    char ss_ip[MAX_IP_LEN];
    int client_port; // The port this SS will open for clients
    int file_count;  // How many files this SS is about to send
} Msg_SS_Register;

// The data for a single file item (sent by SS)
typedef struct {
    char filename[MAX_FILENAME];
} Msg_File_Item;


// --- 5. HELPER FUNCTION ---
static inline void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

#endif //PROTOCOL_H