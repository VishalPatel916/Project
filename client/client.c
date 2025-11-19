#include "protocol.h"
#include <netinet/in.h>

// --- Helper: Send a request with just a filename ---
void send_filename_request(int sock, MessageType type, char* filename) {
    Header header;
    header.type = type;
    header.payload_size = sizeof(Msg_Filename_Request);
    Msg_Filename_Request req; strncpy(req.filename, filename, MAX_FILENAME);
    if (send(sock, &header, sizeof(header), 0) < 0) error_exit("send header");
    if (send(sock, &req, sizeof(req), 0) < 0) error_exit("send payload");
}

// --- Helper: Print metadata ---
void print_metadata(Msg_Full_Metadata* meta) {
    char time_buf[50];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&meta->last_modified));
    printf("| %-20s | %-12s | %-6ld | %-6d | %-6d | %s |\n",
        meta->filename, meta->owner, meta->file_size,
        meta->word_count, meta->char_count, time_buf);
}
void print_metadata_header() {
    printf("-------------------------------------------------------------------------------------------\n");
    printf("| Filename             | Owner        | Size   | Words  | Chars  | Last Modified       |\n");
    printf("-------------------------------------------------------------------------------------------\n");
}
void print_metadata_footer() {
    printf("-------------------------------------------------------------------------------------------\n");
}

// --- Helper: Handle READ ---
void handle_read_from_ss(char* filename, char* ss_ip, int ss_port) {
    int ss_sock; struct sockaddr_in ss_addr;
    printf("  -> Connecting directly to Storage Server at %s:%d...\n", ss_ip, ss_port);
    ss_sock = socket(AF_INET, SOCK_STREAM, 0); if (ss_sock < 0) error_exit("ss socket");
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET; ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) error_exit("ss inet_pton");
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) error_exit("ss connect");
    send_filename_request(ss_sock, REQ_CLIENT_READ, filename);
    Header ss_header;
    if (recv(ss_sock, &ss_header, sizeof(Header), 0) <= 0) { printf("Error: Storage Server disconnected unexpectedly.\n"); close(ss_sock); return; }
    if (ss_header.type == RES_SS_FILE_OK) {
        printf("--- Start of file '%s' ---\n", filename);
        char buffer[FILE_BUFFER_SIZE]; int bytes_recvd;
        while ((bytes_recvd = recv(ss_sock, buffer, FILE_BUFFER_SIZE, 0)) > 0) {
            write(STDOUT_FILENO, buffer, bytes_recvd);
        }
        printf("\n--- End of file '%s' ---\n", filename);
    } else if (ss_header.type == RES_ERROR_NOT_FOUND) { printf("Error: File '%s' not found on the Storage Server.\n", filename); }
    else { printf("Error: Unknown response %d from Storage Server.\n", ss_header.type); }
    close(ss_sock);
}

// --- Helper: Handle STREAM ---
void handle_stream_from_ss(char* filename, char* ss_ip, int ss_port) {
    int ss_sock; struct sockaddr_in ss_addr;
    printf("  -> Connecting directly to Storage Server at %s:%d...\n", ss_ip, ss_port);
    ss_sock = socket(AF_INET, SOCK_STREAM, 0); if (ss_sock < 0) error_exit("ss socket");
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET; ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) error_exit("ss inet_pton");
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) error_exit("ss connect");
    send_filename_request(ss_sock, REQ_CLIENT_STREAM, filename);
    Header ss_header;
    if (recv(ss_sock, &ss_header, sizeof(Header), 0) <= 0) { printf("Error: Storage Server disconnected unexpectedly.\n"); close(ss_sock); return; }
    if (ss_header.type == RES_SS_FILE_OK) {
        printf("--- Start of stream '%s' ---\n", filename);
        char buffer[FILE_BUFFER_SIZE]; int bytes_recvd;
        while ((bytes_recvd = recv(ss_sock, buffer, FILE_BUFFER_SIZE, 0)) > 0) {
            write(STDOUT_FILENO, buffer, bytes_recvd);
            fflush(stdout);
        }
        printf("\n--- End of stream '%s' ---\n", filename);
    } else if (ss_header.type == RES_ERROR_NOT_FOUND) { printf("Error: File '%s' not found on the Storage Server.\n", filename); }
    else { printf("Error: Unknown response %d from Storage Server.\n", ss_header.type); }
    close(ss_sock);
}

// --- Helper: Handle WRITE ---
void handle_write_to_ss(char* filename, int sentence_num, char* ss_ip, int ss_port) {
    int ss_sock; struct sockaddr_in ss_addr;
    printf("  -> Connecting directly to Storage Server at %s:%d...\n", ss_ip, ss_port);
    ss_sock = socket(AF_INET, SOCK_STREAM, 0); if (ss_sock < 0) error_exit("ss socket");
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET; ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) error_exit("ss inet_pton");
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) error_exit("ss connect");
    Header header; header.type = REQ_CLIENT_WRITE; header.payload_size = sizeof(Msg_Client_Write);
    Msg_Client_Write req; strncpy(req.filename, filename, MAX_FILENAME); req.sentence_num = sentence_num;
    if (send(ss_sock, &header, sizeof(header), 0) < 0) error_exit("send write header");
    if (send(ss_sock, &req, sizeof(req), 0) < 0) error_exit("send write payload");
    if (recv(ss_sock, &header, sizeof(Header), 0) <= 0) { printf("Error: SS disconnected while waiting for lock.\n"); close(ss_sock); return; }
    if (header.type == RES_OK_LOCKED) {
        printf("Lock acquired for '%s' (sent %d). Enter <word_index> <content>.\n", filename, sentence_num);
        printf("Type 'ETIRW' to save and exit.\n");
        char write_buffer[1024];
        while(1) {
            printf("(writing)> "); fflush(stdout);
            if (fgets(write_buffer, sizeof(write_buffer), stdin) == NULL) break;
            char* command = strtok(write_buffer, " \n");
            if (command == NULL) continue;
            if (strcmp(command, "ETIRW") == 0) {
                send_simple_header(ss_sock, REQ_ETIRW);
                recv(ss_sock, &header, sizeof(header), 0);
                if(header.type == RES_OK) printf("Write successful!\n");
                else if(header.type == RES_ERROR) printf("Error: Write failed to commit. Check word indices are valid.\n");
                else printf("Error: Write failed to commit (response %d).\n", header.type);
                break; 
            } else {
                char* content = strtok(NULL, "\n");
                if (content == NULL) { printf("Usage: <word_index> <content>\n"); continue; }
                int word_index = atoi(command);
                header.type = REQ_WRITE_UPDATE; header.payload_size = sizeof(Msg_Write_Update);
                Msg_Write_Update update_req; update_req.word_index = word_index; strncpy(update_req.content, content, MAX_WORD_CONTENT);
                if (send(ss_sock, &header, sizeof(header), 0) < 0) break;
                if (send(ss_sock, &update_req, sizeof(update_req), 0) < 0) break;
                
                // Receive response for this update
                if (recv(ss_sock, &header, sizeof(header), 0) < 0) break;
                if (header.type == RES_ERROR) {
                    printf("Error: Word index %d is out of bounds. Please try again.\n", word_index);
                } else if (header.type == RES_ERROR_INVALID_WORD) {
                    printf("Error: Word index out of range.\n");
                } else if (header.type != RES_OK) {
                    printf("Error: Update failed (response %d).\n", header.type);
                } else {
                    printf("Update recorded.\n");
                }
            }
        }
    } else if (header.type == RES_ERROR_LOCKED) { printf("Error: Sentence is locked by another user.\n"); }
    else if (header.type == RES_ERROR_INVALID_SENTENCE) { printf("Error: Sentence index out of range.\n"); }
    else { printf("Error: Failed to acquire lock (unknown response %d).\n", header.type); }
    close(ss_sock);
}


int main() {
    int sock;
    struct sockaddr_in nm_addr; char username[MAX_USERNAME];
    printf("Enter your username: ");
    fgets(username, MAX_USERNAME, stdin);
    username[strcspn(username, "\n")] = 0;
    sock = socket(AF_INET, SOCK_STREAM, 0); if (sock < 0) error_exit("socket");
    memset(&nm_addr, 0, sizeof(nm_addr)); nm_addr.sin_family = AF_INET; nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) error_exit("inet_pton");
    if (connect(sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    printf("Connected to Name Server...\n");
    Header header; header.type = REQ_CLIENT_REGISTER; header.payload_size = sizeof(Msg_Client_Register);
    Msg_Client_Register reg_msg; strncpy(reg_msg.username, username, MAX_USERNAME);
    if (send(sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    if (recv(sock, &header, sizeof(Header), 0) < 0 || header.type != RES_OK) error_exit("Registration failed");
    printf("Registration successful! Welcome, %s.\n", username);

    char line_buffer[1024];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) break; 

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
                if (header.type == RES_READ_LOCATION) { Msg_Read_Response resp; recv(sock, &resp, sizeof(resp), 0); handle_read_from_ss(filename, resp.ss_ip, resp.ss_port);
                } else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File '%s' not found.\n", filename);
                else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied.\n");
                else printf("Error: Could not read file.\n");
            }
        }
        else if (strcmp(command, "write") == 0) {
            char* filename = strtok(NULL, " \n");
            char* sentence_str = strtok(NULL, " \n");
            if (filename == NULL || sentence_str == NULL) { printf("Usage: write <filename> <sentence_number>\n"); }
            else {
                int sentence_num = atoi(sentence_str);
                send_filename_request(sock, REQ_WRITE, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_READ_LOCATION) { Msg_Read_Response resp; recv(sock, &resp, sizeof(resp), 0); handle_write_to_ss(filename, sentence_num, resp.ss_ip, resp.ss_port);
                } else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File '%s' not found.\n", filename);
                else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied.\n");
                else printf("Error: Could not write to file (NM error).\n");
            }
        }
        else if (strcmp(command, "list") == 0) {
            send_simple_header(sock, REQ_LIST);
            recv(sock, &header, sizeof(header), 0);
            if (header.type == RES_LIST_HDR) {
                Msg_List_Hdr list_hdr; recv(sock, &list_hdr, sizeof(list_hdr), 0);
                printf("--- Registered Users (%d) ---\n", list_hdr.user_count);
                for (int i = 0; i < list_hdr.user_count; i++) { Msg_List_Item item; recv(sock, &item, sizeof(item), 0); printf("  - %s\n", item.username); }
            }
        }
        else if (strcmp(command, "delete") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: delete <filename>\n"); }
            else {
                send_filename_request(sock, REQ_DELETE, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) printf("File '%s' deleted successfully.\n", filename);
                else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File not found.\n");
                else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied (you are not the owner).\n");
                else printf("Error: Could not delete file.\n");
            }
        }
        else if (strcmp(command, "undo") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: undo <filename>\n"); }
            else {
                send_filename_request(sock, REQ_UNDO, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) printf("Undo successful for '%s'.\n", filename);
                else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File not found.\n");
                else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied.\n");
                else printf("Error: Could not undo file.\n");
            }
        }
        else if (strcmp(command, "stream") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: stream <filename>\n"); }
            else {
                send_filename_request(sock, REQ_STREAM, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_READ_LOCATION) { Msg_Read_Response resp; recv(sock, &resp, sizeof(resp), 0); handle_stream_from_ss(filename, resp.ss_ip, resp.ss_port);
                } else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File '%s' not found.\n", filename);
                else printf("Error: Could not stream file.\n");
            }
        }
        else if (strcmp(command, "view") == 0) {
            char* flag = strtok(NULL, " \n");
            Msg_View_Request req;
            req.flag_a = 0; req.flag_l = 0;
            if (flag != NULL) {
                if (strstr(flag, "a")) req.flag_a = 1;
                if (strstr(flag, "l")) req.flag_l = 1;
            }
            header.type = REQ_VIEW; header.payload_size = sizeof(req);
            send(sock, &header, sizeof(header), 0); send(sock, &req, sizeof(req), 0);
            recv(sock, &header, sizeof(header), 0);
            Msg_View_Hdr view_hdr; recv(sock, &view_hdr, sizeof(view_hdr), 0);
            
            if(req.flag_l) print_metadata_header();
            else printf("--- Files (%d) ---\n", view_hdr.file_count);

            for(int i=0; i < view_hdr.file_count; i++) {
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_VIEW_ITEM_SHORT) {
                    Msg_View_Item_Short item; recv(sock, &item, sizeof(item), 0);
                    printf("  %s\n", item.filename);
                } else if (header.type == RES_VIEW_ITEM_LONG) {
                    Msg_Full_Metadata meta; recv(sock, &meta, sizeof(meta), 0);
                    print_metadata(&meta);
                    if (meta.access_count > 0) {
                        AccessEntry dummy_list[MAX_PERMISSIONS_PER_FILE];
                        recv(sock, &dummy_list, sizeof(AccessEntry) * meta.access_count, 0);
                    }
                }
            }
            if(req.flag_l) print_metadata_footer();
        }
        else if (strcmp(command, "info") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: info <filename>\n"); }
            else {
                send_filename_request(sock, REQ_INFO, filename);
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_INFO) {
                    Msg_Full_Metadata meta; recv(sock, &meta, sizeof(meta), 0);
                    print_metadata_header();
                    print_metadata(&meta);
                    print_metadata_footer();
                    if (meta.access_count > 0) {
                        printf("Access List:\n");
                        AccessEntry list[MAX_PERMISSIONS_PER_FILE];
                        recv(sock, &list, sizeof(AccessEntry) * meta.access_count, 0);
                        for(int i=0; i < meta.access_count; i++) {
                            printf("  - %s (%s)\n", list[i].username, list[i].permission == READ_ONLY ? "Read" : "Read/Write");
                        }
                    } else { printf("Access List: (empty)\n"); }
                } else if (header.type == RES_ERROR_NOT_FOUND) printf("Error: File not found.\n");
                else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied.\n");
            }
        }
        else if (strcmp(command, "addaccess") == 0) {
            char* flag = strtok(NULL, " \n");
            char* filename = strtok(NULL, " \n");
            // Capture the rest of the line as username (allow spaces)
            char* user = strtok(NULL, "\n");
            if (flag == NULL || filename == NULL || user == NULL) { printf("Usage: addaccess -R|-W <filename> <username>\n"); }
            else {
                // Trim leading/trailing spaces around username
                while (*user == ' ') user++;
                size_t ulen = strlen(user);
                while (ulen > 0 && user[ulen - 1] == ' ') { user[--ulen] = '\0'; }
                if (ulen == 0) { printf("Usage: addaccess -R|-W <filename> <username>\n"); }
                else {
                    Msg_Access_Request req;
                    strncpy(req.filename, filename, MAX_FILENAME);
                    strncpy(req.username, user, MAX_USERNAME);
                    if (strcmp(flag, "-W") == 0) req.perm = READ_WRITE;
                    else req.perm = READ_ONLY;
                    
                    header.type = REQ_ADD_ACCESS; header.payload_size = sizeof(req);
                    send(sock, &header, sizeof(header), 0); send(sock, &req, sizeof(req), 0);
                    
                    recv(sock, &header, sizeof(header), 0);
                    if (header.type == RES_OK) printf("Access granted.\n");
                    else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied (you are not the owner).\n");
                    else printf("Error: Could not add access.\n");
                }
            }
        }
        else if (strcmp(command, "remaccess") == 0) {
            char* filename = strtok(NULL, " \n");
            // Capture the rest of the line as username (allow spaces)
            char* user = strtok(NULL, "\n");
            if (filename == NULL || user == NULL) { printf("Usage: remaccess <filename> <username>\n"); }
            else {
                while (*user == ' ') user++;
                size_t ulen = strlen(user);
                while (ulen > 0 && user[ulen - 1] == ' ') { user[--ulen] = '\0'; }
                if (ulen == 0) { printf("Usage: remaccess <filename> <username>\n"); }
                else {
                    Msg_Access_Request req;
                    strncpy(req.filename, filename, MAX_FILENAME);
                    strncpy(req.username, user, MAX_USERNAME);
                    
                    header.type = REQ_REM_ACCESS; header.payload_size = sizeof(req);
                    send(sock, &header, sizeof(header), 0); send(sock, &req, sizeof(req), 0);
                    
                    recv(sock, &header, sizeof(header), 0);
                    if (header.type == RES_OK) printf("Access removed.\n");
                    else if (header.type == RES_ERROR_ACCESS_DENIED) printf("Error: Access denied (you are not the owner).\n");
                    else printf("Error: Could not remove access.\n");
                }
            }
        }
        
        // --- NEW FOR EXEC ---
        else if (strcmp(command, "exec") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) { printf("Usage: exec <filename>\n"); }
            else {
                send_filename_request(sock, REQ_EXEC, filename);
                printf("--- Executing '%s' ---\n", filename);
                // Enter loop to receive output stream
                while(1) {
                    if (recv(sock, &header, sizeof(header), 0) <= 0) {
                        printf("Error: Connection lost during exec.\n");
                        break;
                    }
                    if (header.type == RES_EXEC_OUTPUT) {
                        Msg_Exec_Output msg;
                        recv(sock, &msg, sizeof(msg), 0);
                        printf("%s", msg.line); // Print the line (already has newline)
                        fflush(stdout);
                    } else if (header.type == RES_EXEC_DONE) {
                        break; // Success
                    } else if (header.type == RES_ERROR_NOT_FOUND) {
                        printf("Error: File not found.\n"); break;
                    } else if (header.type == RES_ERROR_ACCESS_DENIED) {
                        printf("Error: Access denied.\n"); break;
                    } else {
                        printf("Error: Execution failed.\n"); break;
                    }
                }
                printf("--- End of execution ---\n");
            }
        }
        // --- END EXEC ---
        
        // --- FOLDER COMMANDS ---
        else if (strcmp(command, "createfolder") == 0) {
            char* foldername = strtok(NULL, " \n");
            if (foldername == NULL) {
                printf("Usage: createfolder <foldername>\n");
            } else {
                header.type = REQ_CREATEFOLDER;
                header.payload_size = sizeof(Msg_Folder_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_Folder_Request req;
                strncpy(req.foldername, foldername, MAX_FILENAME);
                send(sock, &req, sizeof(req), 0);
                
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) {
                    printf("Folder '%s' created successfully.\n", foldername);
                } else {
                    printf("Error: Failed to create folder.\n");
                }
            }
        }
        else if (strcmp(command, "move") == 0) {
            char* filename = strtok(NULL, " \n");
            char* foldername = strtok(NULL, " \n");
            if (filename == NULL || foldername == NULL) {
                printf("Usage: move <filename> <foldername>\n");
            } else {
                header.type = REQ_MOVE;
                header.payload_size = sizeof(Msg_Move_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_Move_Request req;
                strncpy(req.filename, filename, MAX_FILENAME);
                strncpy(req.foldername, foldername, MAX_FILENAME);
                send(sock, &req, sizeof(req), 0);
                
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) {
                    printf("File '%s' moved to folder '%s' successfully.\n", filename, foldername);
                } else if (header.type == RES_ERROR_NOT_FOUND) {
                    printf("Error: File not found.\n");
                } else if (header.type == RES_ERROR_ACCESS_DENIED) {
                    printf("Error: Access denied. Only owner can move files.\n");
                } else {
                    printf("Error: Failed to move file.\n");
                }
            }
        }
        else if (strcmp(command, "viewfolder") == 0) {
            char* foldername = strtok(NULL, " \n");
            if (foldername == NULL) {
                printf("Usage: viewfolder <foldername>\n");
            } else {
                header.type = REQ_VIEWFOLDER;
                header.payload_size = sizeof(Msg_Folder_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_Folder_Request req;
                strncpy(req.foldername, foldername, MAX_FILENAME);
                send(sock, &req, sizeof(req), 0);
                
                // Receive header
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_VIEW_HDR) {
                    Msg_View_Hdr view_hdr;
                    recv(sock, &view_hdr, sizeof(view_hdr), 0);
                    
                    printf("Files in folder '%s' (%d files):\n", foldername, view_hdr.file_count);
                    for (int i = 0; i < view_hdr.file_count; i++) {
                        recv(sock, &header, sizeof(header), 0);
                        if (header.type == RES_VIEW_ITEM_SHORT) {
                            Msg_View_Item_Short item;
                            recv(sock, &item, sizeof(item), 0);
                            printf("  - %s\n", item.filename);
                        }
                    }
                } else {
                    printf("Error: Failed to view folder.\n");
                }
            }
        }
        // --- END FOLDER COMMANDS ---
        
        // --- CHECKPOINT COMMANDS ---
        else if (strcmp(command, "checkpoint") == 0) {
            char* filename = strtok(NULL, " \n");
            char* tag = strtok(NULL, " \n");
            if (filename == NULL || tag == NULL) {
                printf("Usage: checkpoint <filename> <checkpoint_tag>\n");
            } else {
                header.type = REQ_CHECKPOINT;
                header.payload_size = sizeof(Msg_Checkpoint_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_Checkpoint_Request req;
                strncpy(req.filename, filename, MAX_FILENAME);
                strncpy(req.tag, tag, MAX_CHECKPOINT_TAG);
                send(sock, &req, sizeof(req), 0);
                
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) {
                    printf("Checkpoint '%s' created successfully for file '%s'.\n", tag, filename);
                } else {
                    printf("Error: Failed to create checkpoint (file not found, tag already exists, or permission denied).\n");
                }
            }
        }
        else if (strcmp(command, "viewcheckpoint") == 0) {
            char* filename = strtok(NULL, " \n");
            char* tag = strtok(NULL, " \n");
            if (filename == NULL || tag == NULL) {
                printf("Usage: viewcheckpoint <filename> <checkpoint_tag>\n");
            } else {
                header.type = REQ_VIEWCHECKPOINT;
                header.payload_size = sizeof(Msg_Checkpoint_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_Checkpoint_Request req;
                strncpy(req.filename, filename, MAX_FILENAME);
                strncpy(req.tag, tag, MAX_CHECKPOINT_TAG);
                send(sock, &req, sizeof(req), 0);
                
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_SS_FILE_OK) {
                    printf("\n=== Checkpoint '%s' content for '%s' ===\n", tag, filename);
                    char buffer[4096];
                    int remaining = header.payload_size;
                    while (remaining > 0) {
                        int to_read = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
                        int bytes = recv(sock, buffer, to_read, 0);
                        if (bytes <= 0) break;
                        fwrite(buffer, 1, bytes, stdout);
                        remaining -= bytes;
                    }
                    printf("\n=== End of checkpoint ===\n");
                } else {
                    printf("Error: Checkpoint not found or permission denied.\n");
                }
            }
        }
        else if (strcmp(command, "revert") == 0) {
            char* filename = strtok(NULL, " \n");
            char* tag = strtok(NULL, " \n");
            if (filename == NULL || tag == NULL) {
                printf("Usage: revert <filename> <checkpoint_tag>\n");
            } else {
                header.type = REQ_REVERT;
                header.payload_size = sizeof(Msg_Checkpoint_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_Checkpoint_Request req;
                strncpy(req.filename, filename, MAX_FILENAME);
                strncpy(req.tag, tag, MAX_CHECKPOINT_TAG);
                send(sock, &req, sizeof(req), 0);
                
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_OK) {
                    printf("File '%s' successfully reverted to checkpoint '%s'.\n", filename, tag);
                } else {
                    printf("Error: Failed to revert (checkpoint not found or permission denied).\n");
                }
            }
        }
        else if (strcmp(command, "listcheckpoints") == 0) {
            char* filename = strtok(NULL, " \n");
            if (filename == NULL) {
                printf("Usage: listcheckpoints <filename>\n");
            } else {
                header.type = REQ_LISTCHECKPOINTS;
                header.payload_size = sizeof(Msg_ListCheckpoints_Request);
                send(sock, &header, sizeof(header), 0);
                
                Msg_ListCheckpoints_Request req;
                strncpy(req.filename, filename, MAX_FILENAME);
                send(sock, &req, sizeof(req), 0);
                
                recv(sock, &header, sizeof(header), 0);
                if (header.type == RES_CHECKPOINT_LIST) {
                    Msg_Checkpoint_List_Hdr hdr;
                    recv(sock, &hdr, sizeof(hdr), 0);
                    
                    if (hdr.checkpoint_count == 0) {
                        printf("No checkpoints found for file '%s'.\n", filename);
                    } else {
                        printf("Checkpoints for '%s' (%d total):\n", filename, hdr.checkpoint_count);
                        for (int i = 0; i < hdr.checkpoint_count; i++) {
                            Msg_Checkpoint_Item item;
                            recv(sock, &item, sizeof(item), 0);
                            
                            char time_str[100];
                            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&item.timestamp));
                            printf("  - Tag: %s  (Created: %s)\n", item.tag, time_str);
                        }
                    }
                } else {
                    printf("Error: File not found or permission denied.\n");
                }
            }
        }
        // --- END CHECKPOINT COMMANDS ---
        
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