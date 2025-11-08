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
    
    ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) error_exit("ss socket");

    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) error_exit("ss inet_pton");

    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) error_exit("ss connect");

    send_filename_request(ss_sock, REQ_CLIENT_READ, filename);
    
    Header ss_header;
    if (recv(ss_sock, &ss_header, sizeof(Header), 0) <= 0) {
        printf("Error: Storage Server disconnected unexpectedly.\n");
        close(ss_sock);
        return;
    }

    if (ss_header.type == RES_SS_FILE_OK) {
        printf("--- Start of file '%s' ---\n", filename);
        char buffer[FILE_BUFFER_SIZE];
        int bytes_recvd;
        while ((bytes_recvd = recv(ss_sock, buffer, FILE_BUFFER_SIZE, 0)) > 0) {
            write(STDOUT_FILENO, buffer, bytes_recvd);
        }
        printf("\n--- End of file '%s' ---\n", filename);
    } else if (ss_header.type == RES_ERROR_NOT_FOUND) {
        printf("Error: File '%s' not found on the Storage Server.\n", filename);
    } else {
        printf("Error: Unknown response %d from Storage Server.\n", ss_header.type);
    }

    close(ss_sock);
}

// --- Helper: Handle direct-to-SS WRITE operation ---
void handle_write_to_ss(char* filename, int sentence_num, char* ss_ip, int ss_port) {
    int ss_sock;
    struct sockaddr_in ss_addr;

    printf("  -> Connecting directly to Storage Server at %s:%d...\n", ss_ip, ss_port);
    
    // 1. Connect to SS
    ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) error_exit("ss socket");
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) error_exit("ss inet_pton");
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) error_exit("ss connect");

    // 2. Send lock request (REQ_CLIENT_WRITE)
    Header header;
    header.type = REQ_CLIENT_WRITE;
    header.payload_size = sizeof(Msg_Client_Write);
    
    Msg_Client_Write req;
    strncpy(req.filename, filename, MAX_FILENAME);
    req.sentence_num = sentence_num;
    
    if (send(ss_sock, &header, sizeof(header), 0) < 0) error_exit("send write header");
    if (send(ss_sock, &req, sizeof(req), 0) < 0) error_exit("send write payload");

    // 3. Wait for lock response
    if (recv(ss_sock, &header, sizeof(Header), 0) <= 0) {
        printf("Error: SS disconnected while waiting for lock.\n");
        close(ss_sock);
        return;
    }
    
    if (header.type == RES_OK_LOCKED) {
        printf("Lock acquired for '%s' (sent %d). Enter <word_index> <content>.\n", filename, sentence_num);
        printf("Type 'ETIRW' to save and exit.\n");

        // 4. --- Enter the INNER write loop ---
        char write_buffer[1024];
        while(1) {
            printf("(writing)> ");
            fflush(stdout);

            if (fgets(write_buffer, sizeof(write_buffer), stdin) == NULL) {
                break; // EOF
            }

            char* command = strtok(write_buffer, " \n");
            if (command == NULL) continue;
            
            if (strcmp(command, "ETIRW") == 0) {
                // Send ETIRW, wait for final OK, then exit loop
                send_simple_header(ss_sock, REQ_ETIRW);
                recv(ss_sock, &header, sizeof(header), 0);
                if(header.type == RES_OK) {
                    printf("Write successful!\n");
                } else {
                    printf("Error: Write failed to commit.\n");
                }
                break; // Exit inner write loop
            } else {
                // This is a word update
                char* content = strtok(NULL, "\n"); // Get the rest of the line
                if (content == NULL) {
                    printf("Usage: <word_index> <content>\n");
                    continue;
                }
                int word_index = atoi(command);

                // Send REQ_WRITE_UPDATE
                header.type = REQ_WRITE_UPDATE;
                header.payload_size = sizeof(Msg_Write_Update);
                
                Msg_Write_Update update_req;
                update_req.word_index = word_index;
                strncpy(update_req.content, content, MAX_WORD_CONTENT);

                if (send(ss_sock, &header, sizeof(header), 0) < 0) break;
                if (send(ss_sock, &update_req, sizeof(update_req), 0) < 0) break;
            }
        }
        
    } else if (header.type == RES_ERROR_LOCKED) {
        printf("Error: Sentence is locked by another user.\n");
    } else {
        printf("Error: Failed to acquire lock (unknown response %d).\n", header.type);
    }
    
    // 5. Close connection to SS
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

        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) break; // EOF

        char* command = strtok(line_buffer, " \n");
        if (command == NULL) continue;

        if (strcmp(command, "create") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: create <filename>\n"); }
            else {
                send_filename_request(sock, REQ_CREATE, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) printf("File '%s' created successfully!\n", filename);
                else if (header.type == RES_ERROR_FILE_EXISTS) printf("Error: File '%s' already exists.\n", filename);
                else printf("Error: Could not create file.\n");
            }
        } 
        else if (strcmp(command, "read") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: read <filename>\n"); }
            else {
                send_filename_request(sock, REQ_READ, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_READ_LOCATION) {
                    Msg_Read_Response resp;
                    recv(sock, &resp, sizeof(resp), 0);
                    handle_read_from_ss(filename, resp.ss_ip, resp.ss_port);
                } else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File '%s' not found.\n", filename);
                else printf("Error: Could not read file.\n");
            }
        }
        // --- NEW FOR PHASE 3 ---
        else if (strcmp(command, "write") == 0) {
            char* filename = strtok(NULL, " \n");
            char* sentence_str = strtok(NULL, " \n");
            if (filename == NULL || sentence_str == NULL) {
                printf("Usage: write <filename> <sentence_number>\n");
            } else {
                int sentence_num = atoi(sentence_str);
                // Send REQ_WRITE to NM
                send_filename_request(sock, REQ_WRITE, filename);
                
                // Wait for response from NM
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_READ_LOCATION) {
                    Msg_Read_Response resp;
                    recv(sock, &resp, sizeof(resp), 0);
                    // Got location, now start the SS write session
                    handle_write_to_ss(filename, sentence_num, resp.ss_ip, resp.ss_port);
                } else if (header.type == RES_ERROR_NOT_FOUND) {
                    printf("Error: File '%s' not found.\n", filename);
                } else {
                    printf("Error: Could not write to file (NM error).\n");
                }
            }
        }
        // --- END PHASE 3 ---
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