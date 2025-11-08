#include "protocol.h"
#include <netinet/in.h>

// --- Helper: Send a request with just a filename ---
void send_filename_request(int sock, MessageType type, char* filename) {
    Header header;
    header.type = type;
    header.payload_size = sizeof(Msg_Filename_Request);
    
    Msg_Filename_Request req;
    strncpy(req.filename, filename, MAX_FILENAME);

    if (send(sock, &header, sizeof(header), 0) < 0) error_exit("send header");
    if (send(sock, &req, sizeof(req), 0) < 0) error_exit("send payload");
}


// --- Helper: Handle direct-to-SS READ operation (with handshake) ---
void handle_read_from_ss(char* filename, char* ss_ip, int ss_port) {
    int ss_sock;
    struct sockaddr_in ss_addr;

    printf("  -> Connecting directly to Storage Server at %s:%d...\n", ss_ip, ss_port);
    
    // 1. Create new socket
    ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) error_exit("ss socket");

    // 2. Set SS info
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        error_exit("ss inet_pton");
    }

    // 3. Connect to SS
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        error_exit("ss connect");
    }

    // 4. Send REQ_CLIENT_READ to SS
    send_filename_request(ss_sock, REQ_CLIENT_READ, filename);
    
    // --- 5. Wait for the handshake response from the SS ---
    Header ss_header;
    if (recv(ss_sock, &ss_header, sizeof(Header), 0) <= 0) {
        printf("Error: Storage Server disconnected unexpectedly.\n");
        close(ss_sock);
        return;
    }

    if (ss_header.type == RES_SS_FILE_OK) {
        // --- 6. OK! Now receive file data in a loop ---
        printf("--- Start of file '%s' ---\n", filename);
        char buffer[FILE_BUFFER_SIZE];
        int bytes_recvd;
        while ((bytes_recvd = recv(ss_sock, buffer, FILE_BUFFER_SIZE, 0)) > 0) {
            write(STDOUT_FILENO, buffer, bytes_recvd); // Write directly to terminal
        }
        printf("\n--- End of file '%s' ---\n", filename);

    } else if (ss_header.type == RES_ERROR_NOT_FOUND) {
        // The file was not on the SS, even though the NM thought it was
        printf("Error: File '%s' not found on the Storage Server.\n", filename);
    } else {
        printf("Error: Unknown response %d from Storage Server.\n", ss_header.type);
    }

    // 7. Close connection to SS
    close(ss_sock);
}


int main() {
    int sock; // This is the persistent connection to the Name Server
    struct sockaddr_in nm_addr;
    char username[MAX_USERNAME];

    // --- 1. Get username (Phase 1) ---
    printf("Enter your username: ");
    fgets(username, MAX_USERNAME, stdin);
    username[strcspn(username, "\n")] = 0;

    // --- 2. Create socket (Phase 1) ---
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) error_exit("socket");

    // --- 3. Set NM info (Phase 1) ---
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) error_exit("inet_pton");

    // --- 4. Connect to NM (Phase 1) ---
    if (connect(sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    printf("Connected to Name Server...\n");

    // --- 5. Register with NM (Phase 1) ---
    Header header;
    header.type = REQ_CLIENT_REGISTER;
    header.payload_size = sizeof(Msg_Client_Register);
    Msg_Client_Register reg_msg;
    strncpy(reg_msg.username, username, MAX_USERNAME);
    if (send(sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    
    // --- 6. Wait for OK (Phase 1) ---
    if (recv(sock, &header, sizeof(Header), 0) < 0 || header.type != RES_OK) {
        error_exit("Registration failed");
    }
    printf("Registration successful! Welcome, %s.\n", username);

    // --- 7. Main Command Loop (NEW) ---
    char line_buffer[1024];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) {
            break; // EOF (Ctrl+D)
        }

        // Parse command
        char* command = strtok(line_buffer, " \n");
        if (command == NULL) {
            continue; // Empty line
        }

        if (strcmp(command, "create") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) {
                printf("Usage: create <filename>\n");
            } else {
                // Send REQ_CREATE to NM
                send_filename_request(sock, REQ_CREATE, filename);
                
                // Wait for response from NM
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) {
                    printf("File '%s' created successfully!\n", filename);
                } else if (header.type == RES_ERROR_FILE_EXISTS) {
                    printf("Error: File '%s' already exists.\n", filename);
                } else {
                    printf("Error: Could not create file.\n");
                }
            }
        } 
        else if (strcmp(command, "read") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) {
                printf("Usage: read <filename>\n");
            } else {
                // Send REQ_READ to NM
                send_filename_request(sock, REQ_READ, filename);

                // Wait for response from NM
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_READ_LOCATION) {
                    Msg_Read_Response resp;
                    recv(sock, &resp, sizeof(resp), 0);
                    handle_read_from_ss(filename, resp.ss_ip, resp.ss_port);
                } else if (header.type == RES_ERROR_NOT_FOUND) {
                    printf("Error: File '%s' not found.\n", filename);
                } else {
                    printf("Error: Could not read file.\n");
                }
            }
        }
        else if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) {
            break;
        }
        else {
            printf("Unknown command: %s\n", command);
        }
    }

    printf("Disconnecting...\n");
    close(sock);
    return 0;
}