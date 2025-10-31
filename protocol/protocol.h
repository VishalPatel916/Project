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

// --- 2. MESSAGE TYPES ---
// This enum defines *every* command in our system
typedef enum {
    REQ_CLIENT_REGISTER, // Client -> NM
    REQ_SS_REGISTER,     // SS -> NM
    RES_OK,              // NM -> Client/SS (Generic success)
    RES_ERROR            // NM -> Client/SS (Generic failure)
} MessageType;

// --- 3. HEADER STRUCT ---
// EVERY message starts with this header.
// It tells the receiver what's coming next.
typedef struct {
    MessageType type;
    int payload_size; // Size of the struct (if any) that follows
} Header;

// --- 4. MESSAGE PAYLOADS (Structs) ---
// The data for a client registration
typedef struct {
    char username[MAX_USERNAME];
} Msg_Client_Register;

// The data for a storage server registration
typedef struct {
    char ss_ip[MAX_IP_LEN];
    int client_port; // The port this SS will open for clients
} Msg_SS_Register;

// --- 5. HELPER FUNCTION ---
// A simple error handler we can use in all files
static inline void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

#endif //PROTOCOL_H