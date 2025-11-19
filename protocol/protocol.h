#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

// --- 1. GLOBAL CONSTANTS ---
#define NM_PORT 8080
#define MAX_USERNAME 50
#define MAX_IP_LEN 16
#define MAX_FILENAME 100
#define MAX_FILES_PER_SS 100
#define MAX_FILES_IN_SYSTEM 1024
#define FILE_BUFFER_SIZE 4096
#define MAX_WORD_CONTENT 100
#define MAX_PERMISSIONS_PER_FILE 50

// --- 2. PERMISSION LEVELS ---
typedef enum { NO_PERM, READ_ONLY, READ_WRITE } PermissionLevel;

// --- 3. MESSAGE TYPES ---
typedef enum {
    // Phase 1
    REQ_CLIENT_REGISTER, REQ_SS_REGISTER, REQ_SS_FILE_ITEM,
    RES_OK, RES_ERROR,
    // Phase 2
    REQ_CREATE, REQ_SS_CREATE, RES_ERROR_FILE_EXISTS,
    REQ_READ, RES_READ_LOCATION, REQ_CLIENT_READ,
    RES_ERROR_NOT_FOUND, RES_SS_FILE_OK,
    // Phase 3
    REQ_WRITE, REQ_CLIENT_WRITE, RES_OK_LOCKED,
    RES_ERROR_LOCKED, REQ_WRITE_UPDATE, REQ_ETIRW,
    RES_ERROR_INVALID_SENTENCE, RES_ERROR_INVALID_WORD,
    // Phase 4
    REQ_LIST, RES_LIST_HDR, RES_LIST_ITEM,
    REQ_DELETE, REQ_SS_DELETE, REQ_UNDO, REQ_SS_UNDO,
    REQ_STREAM, REQ_CLIENT_STREAM,
    RES_ERROR_ACCESS_DENIED,
    // Phase 5
    REQ_UPDATE_METADATA, REQ_VIEW, RES_VIEW_HDR,
    RES_VIEW_ITEM_SHORT, RES_VIEW_ITEM_LONG, REQ_INFO,
    RES_INFO, REQ_ADD_ACCESS, REQ_REM_ACCESS,
    REQ_SS_ADD_ACCESS, REQ_SS_REM_ACCESS,  // NM -> SS for access control updates

    // --- NEW: Exec ---
    REQ_EXEC,           // Client -> NM
    RES_EXEC_OUTPUT,    // NM -> Client (sends one line of output)
    RES_EXEC_DONE,      // NM -> Client (signals end of output)

    // --- NEW: Folder Operations ---
    REQ_CREATEFOLDER,   // Client -> NM
    REQ_SS_CREATEFOLDER, // NM -> SS
    REQ_MOVE,           // Client -> NM
    REQ_SS_MOVE,        // NM -> SS
    REQ_VIEWFOLDER,     // Client -> NM
    REQ_SS_CHECKFOLDER,  // NM -> SS (check if folder exists)

    // --- NEW: Checkpoint Operations ---
    REQ_CHECKPOINT,      // Client -> NM
    REQ_SS_CHECKPOINT,   // NM -> SS
    REQ_VIEWCHECKPOINT,  // Client -> NM
    REQ_SS_VIEWCHECKPOINT, // NM -> SS
    REQ_REVERT,          // Client -> NM
    REQ_SS_REVERT,       // NM -> SS
    REQ_LISTCHECKPOINTS, // Client -> NM
    REQ_SS_LISTCHECKPOINTS, // NM -> SS
    RES_CHECKPOINT_LIST,  // SS -> NM -> Client

    // --- NEW: Access Request Operations ---
    REQ_REQUEST_ACCESS,  // Client -> NM (request access to a file)
    REQ_CHECK_REQUESTS,  // Client -> NM (owner checks pending requests)
    RES_REQUEST_LIST,    // NM -> Client (list of pending requests)
    REQ_APPROVE_REQUEST, // Client -> NM (approve a request)
    REQ_DENY_REQUEST     // Client -> NM (deny/remove a request)

} MessageType;

// --- 4. HEADER STRUCT ---
typedef struct { MessageType type; int payload_size; } Header;

// --- 5. MESSAGE PAYLOADS (Structs) ---
typedef struct { char username[MAX_USERNAME]; PermissionLevel permission; } AccessEntry;
typedef struct { char username[MAX_USERNAME]; } Msg_Client_Register;
typedef struct { char ss_ip[MAX_IP_LEN]; int client_port; int file_count; } Msg_SS_Register;
typedef struct { 
    char filename[MAX_FILENAME]; 
    char owner[MAX_USERNAME]; 
    int access_count;
    AccessEntry access_list[MAX_PERMISSIONS_PER_FILE];
} Msg_File_Item;
typedef struct { char filename[MAX_FILENAME]; } Msg_Filename_Request;
typedef struct { char filename[MAX_FILENAME]; char owner[MAX_USERNAME]; } Msg_SS_Create_Request;
typedef struct { char ss_ip[MAX_IP_LEN]; int ss_port; } Msg_Read_Response;
typedef struct { char filename[MAX_FILENAME]; int sentence_num; } Msg_Client_Write;
typedef struct { int word_index; char content[MAX_WORD_CONTENT]; } Msg_Write_Update;
typedef struct { int user_count; } Msg_List_Hdr;
typedef struct { char username[MAX_USERNAME]; } Msg_List_Item;
typedef struct { char filename[MAX_FILENAME]; long file_size; int word_count; int char_count; time_t last_modified; } Msg_Update_Metadata;
typedef struct { int flag_a; int flag_l; } Msg_View_Request;
typedef struct { int file_count; } Msg_View_Hdr;
typedef struct { char filename[MAX_FILENAME]; } Msg_View_Item_Short;
typedef struct { char filename[MAX_FILENAME]; char owner[MAX_USERNAME]; long file_size; int word_count; int char_count; time_t last_modified; int access_count; } Msg_Full_Metadata;
typedef struct { char filename[MAX_FILENAME]; char username[MAX_USERNAME]; PermissionLevel perm; } Msg_Access_Request;

// --- NEW: Exec Payload ---
typedef struct {
    char line[FILE_BUFFER_SIZE]; // Holds one line of shell output
} Msg_Exec_Output;

// --- NEW: Folder Payloads ---
typedef struct { char foldername[MAX_FILENAME]; } Msg_Folder_Request;
typedef struct { char filename[MAX_FILENAME]; char foldername[MAX_FILENAME]; } Msg_Move_Request;

// --- NEW: Checkpoint Payloads ---
#define MAX_CHECKPOINT_TAG 64
typedef struct { char filename[MAX_FILENAME]; char tag[MAX_CHECKPOINT_TAG]; } Msg_Checkpoint_Request;
typedef struct { char filename[MAX_FILENAME]; } Msg_ListCheckpoints_Request;
typedef struct { int checkpoint_count; } Msg_Checkpoint_List_Hdr;
typedef struct { char tag[MAX_CHECKPOINT_TAG]; time_t timestamp; } Msg_Checkpoint_Item;

// --- NEW: Access Request Payloads ---
typedef struct { 
    char filename[MAX_FILENAME]; 
    char requesting_user[MAX_USERNAME]; 
    PermissionLevel requested_perm; 
} Msg_Request_Access;

typedef struct { int request_count; } Msg_Request_List_Hdr;

typedef struct {
    int request_id;  // unique ID for each request
    char filename[MAX_FILENAME];
    char requesting_user[MAX_USERNAME];
    PermissionLevel requested_perm;
    time_t timestamp;
} Msg_Request_Item;

typedef struct { int request_id; } Msg_Request_Response;


// --- 6. HELPER FUNCTION ---
static inline void error_exit(const char *msg) { perror(msg); exit(EXIT_FAILURE); }
static inline void send_simple_header(int sock, MessageType type) {
    Header header; header.type = type; header.payload_size = 0;
    if (send(sock, &header, sizeof(Header), 0) < 0) { perror("send_simple_header"); }
}

#endif //PROTOCOL_H