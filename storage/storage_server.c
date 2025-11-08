#include "protocol.h"
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/select.h>
#include <fcntl.h> // For open()
// Add these includes for memory allocation and string manipulation
#include <malloc.h> 
#include <ctype.h>

#define MY_IP_FOR_CLIENTS "127.0.0.1"
#define MY_PORT_FOR_CLIENTS 8082
#define MY_STORAGE_PATH "./ss_storage"

#define MAX_CONNECTIONS FD_SETSIZE
#define MAX_LOCKS 100

// --- 1. Global State for SS ---
// (Same as your Phase 3 code)
typedef struct { int active; char filename[MAX_FILENAME]; int sentence_num; int sock_fd; } LockInfo;
LockInfo global_locks[MAX_LOCKS];
typedef struct { int active; char filename[MAX_FILENAME]; int sentence_num; char original_path[256]; char temp_path[256]; char backup_path[256]; FILE* temp_file; } WriteSession;
WriteSession write_sessions[MAX_CONNECTIONS];

// --- 2. Lock Helper Functions ---
// (Same as your Phase 3 code)
void init_locks() { for (int i = 0; i < MAX_LOCKS; i++) global_locks[i].active = 0; }
int find_lock(char* f, int s) { for (int i = 0; i < MAX_LOCKS; i++) if (global_locks[i].active && !strcmp(global_locks[i].filename, f) && global_locks[i].sentence_num == s) return i; return -1; }
int create_lock(char* f, int s, int sock) { if (find_lock(f, s) != -1) return 0; for (int i = 0; i < MAX_LOCKS; i++) if (!global_locks[i].active) { global_locks[i].active = 1; strncpy(global_locks[i].filename, f, MAX_FILENAME); global_locks[i].sentence_num = s; global_locks[i].sock_fd = sock; printf("  -> Lock CREATED for '%s' (sent %d) by socket %d\n", f, s, sock); return 1; } return -1; }
void release_lock(char* f, int s) { int i = find_lock(f, s); if (i != -1) { global_locks[i].active = 0; printf("  -> Lock RELEASED for '%s' (sent %d)\n", f, s); } }

// --- 3. File Operation Helpers ---
// (Same as your Phase 3 code)
void handle_create_file(char* filename) { char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename); FILE* f = fopen(file_path, "w"); if (f) fclose(f); }
void handle_send_file(int client_sock, char* filename) { char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename); int fd = open(file_path, O_RDONLY); if (fd < 0) { send_simple_header(client_sock, RES_ERROR_NOT_FOUND); return; } send_simple_header(client_sock, RES_SS_FILE_OK); char buffer[FILE_BUFFER_SIZE]; int bytes_read; while ((bytes_read = read(fd, buffer, FILE_BUFFER_SIZE)) > 0) if (send(client_sock, buffer, bytes_read, 0) < 0) break; close(fd); }

// --- 4. Disconnect Helper ---
// (Same as your Phase 3 code)
void handle_client_disconnect(int sock_fd, fd_set* master_set) { printf("Client on socket %d disconnected\n", sock_fd); if (write_sessions[sock_fd].active) { WriteSession* session = &write_sessions[sock_fd]; printf("  -> Client was in a write session! Rolling back changes.\n"); release_lock(session->filename, session->sentence_num); fclose(session->temp_file); remove(session->temp_path); session->active = 0; } close(sock_fd); FD_CLR(sock_fd, master_set); }

// --- 5. CORRECTED Write Update Helper Function (with INSERT logic) ---
void handle_write_update(WriteSession* session, Msg_Write_Update* req) {
    // 1. Read the *entire* current temp file into memory
    rewind(session->temp_file);
    fseek(session->temp_file, 0, SEEK_END);
    long file_size = ftell(session->temp_file);
    rewind(session->temp_file);

    char* mem_buffer = (char*)malloc(file_size + 1);
    char* file_content_orig = (char*)malloc(file_size + 1);
    if (!mem_buffer || !file_content_orig) {
        perror("malloc update buffer");
        return;
    }
    fread(mem_buffer, 1, file_size, session->temp_file);
    mem_buffer[file_size] = '\0';
    strcpy(file_content_orig, mem_buffer);

    // 2. Prepare a new output buffer
    long new_buf_size = file_size + strlen(req->content) + 1024;
    char* new_mem_buffer = (char*)malloc(new_buf_size);
    char* write_ptr = new_mem_buffer;
    if (!new_mem_buffer) {
        perror("malloc new buffer");
        free(mem_buffer);
        free(file_content_orig);
        return;
    }
    *write_ptr = '\0';

    // 3. Tokenize by sentence, modify the target, and rebuild in new_mem_buffer
    char* sent_context = NULL;
    char* sentence = strtok_r(mem_buffer, ".!?", &sent_context);
    int sent_count = 0;

    while (sentence != NULL) {
        // Find the original delimiter
        long sent_start_offset = sentence - mem_buffer;
        long orig_sent_len = strlen(sentence);
        char delimiter = file_content_orig[sent_start_offset + orig_sent_len];

        // Trim leading whitespace
        while (isspace((unsigned char)*sentence)) {
            *write_ptr++ = *sentence++;
        }
        
        long sent_len = strlen(sentence);

        // 4. Check if this is the target sentence
        if (sent_count == session->sentence_num) {
            printf("  -> Modifying sentence %d\n", sent_count);
            char* word_context = NULL;
            char* word = strtok_r(sentence, " ", &word_context);
            int word_count = 0;

            // --- NEW INSERT LOGIC ---

            // 5. Loop until we hit the insertion index
            while (word != NULL && word_count < req->word_index) {
                strcpy(write_ptr, word);
                write_ptr += strlen(word);
                *write_ptr++ = ' '; // Add space
                
                word_count++;
                word = strtok_r(NULL, " ", &word_context);
            }
            
            // 6. Now, insert the new content
            strcpy(write_ptr, req->content);
            write_ptr += strlen(req->content);
            
            // 7. Add the rest of the old words
            if (word != NULL) {
                // If we stopped before the end, add a space
                *write_ptr++ = ' '; 
                
                // Add the word we stopped on
                strcpy(write_ptr, word);
                write_ptr += strlen(word);
                *write_ptr++ = ' '; // Add space
                
                // Add all remaining words
                word = strtok_r(NULL, " ", &word_context);
                while (word != NULL) {
                    strcpy(write_ptr, word);
                    write_ptr += strlen(word);
                    *write_ptr++ = ' '; // Add space
                    word = strtok_r(NULL, " ", &word_context);
                }
            }
            
            // Remove the last trailing space
            if (write_ptr > new_mem_buffer && *(write_ptr - 1) == ' ') {
                 write_ptr--;
            }
            // --- END INSERT LOGIC ---

        } else {
            // Not the target sentence, just copy it
            strcpy(write_ptr, sentence);
            write_ptr += sent_len;
        }

        // 8. Add the delimiter back
        if (delimiter != '\0') {
            *write_ptr++ = delimiter;
        }

        sent_count++;
        sentence = strtok_r(NULL, ".!?", &sent_context);
    }
    *write_ptr = '\0';

    // 9. Write the new buffer back to the temp file (truncating it)
    rewind(session->temp_file);
    fputs(new_mem_buffer, session->temp_file);
    fflush(session->temp_file);
    ftruncate(fileno(session->temp_file), ftell(session->temp_file));

    // 10. Cleanup
    free(mem_buffer);
    free(file_content_orig);
    free(new_mem_buffer);
}

// --- 6. Main Function ---
int main() {
    int nm_sock, client_listener_sock, new_client_sock;
    struct sockaddr_in nm_addr, client_listen_addr, client_addr;
    socklen_t client_len;
    fd_set master_set, read_set;
    int fdmax;

    // --- Init State ---
    init_locks();
    for(int i = 0; i < MAX_CONNECTIONS; i++) {
        write_sessions[i].active = 0;
    }

    // --- 1. Scan storage directory ---
    // (Same as Phase 2)
    char my_files[MAX_FILES_PER_SS][MAX_FILENAME];
    int file_count = 0;
    printf("Scanning storage directory: %s\n", MY_STORAGE_PATH);
    mkdir(MY_STORAGE_PATH, 0777);
    DIR *d = opendir(MY_STORAGE_PATH);
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL && file_count < MAX_FILES_PER_SS) {
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            strncpy(my_files[file_count], dir->d_name, MAX_FILENAME);
            file_count++;
        }
        closedir(d);
    }
    printf("Found %d files.\n", file_count);

    // --- 2. Connect to Name Server ---
    // (Same as Phase 2)
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) error_exit("socket");
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) error_exit("inet_pton");
    if (connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    printf("Storage Server connected to Name Server...\n");

    // --- 3. Register with Name Server ---
    // (Same as Phase 2)
    Header header;
    header.type = REQ_SS_REGISTER;
    header.payload_size = sizeof(Msg_SS_Register);
    Msg_SS_Register reg_msg;
    strncpy(reg_msg.ss_ip, MY_IP_FOR_CLIENTS, MAX_IP_LEN);
    reg_msg.client_port = MY_PORT_FOR_CLIENTS;
    reg_msg.file_count = file_count;
    if (send(nm_sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(nm_sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    for (int i = 0; i < file_count; i++) {
        Msg_File_Item item;
        strncpy(item.filename, my_files[i], MAX_FILENAME);
        if (send(nm_sock, &item, sizeof(item), 0) < 0) error_exit("send file item");
    }
    if (recv(nm_sock, &header, sizeof(Header), 0) < 0 || header.type != RES_OK) {
        error_exit("Registration failed");
    }
    printf("Registration successful!\n");

    // --- 4. Setup Client Listener Socket ---
    // (Same as Phase 2)
    client_listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_listener_sock < 0) error_exit("client listener socket");
    int yes = 1;
    if (setsockopt(client_listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) error_exit("setsockopt");
    memset(&client_listen_addr, 0, sizeof(client_listen_addr));
    client_listen_addr.sin_family = AF_INET;
    client_listen_addr.sin_addr.s_addr = INADDR_ANY;
    client_listen_addr.sin_port = htons(MY_PORT_FOR_CLIENTS);
    if (bind(client_listener_sock, (struct sockaddr *)&client_listen_addr, sizeof(client_listen_addr)) < 0) error_exit("client listener bind");
    if (listen(client_listener_sock, 10) < 0) error_exit("client listener listen");
    printf("Storage Server now listening for clients on port %d...\n", MY_PORT_FOR_CLIENTS);

    // --- 5. Main Server Loop ---
    FD_ZERO(&master_set);
    FD_SET(nm_sock, &master_set);
    FD_SET(client_listener_sock, &master_set);
    fdmax = (nm_sock > client_listener_sock) ? nm_sock : client_listener_sock;

    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) error_exit("select");

        for (int sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                // A) --- Activity from Name Server ---
                if (sock_fd == nm_sock) {
                    if (recv(nm_sock, &header, sizeof(Header), 0) <= 0) {
                        error_exit("Name Server disconnected!");
                    }
                    switch (header.type) {
                        case REQ_SS_CREATE: {
                            Msg_Filename_Request req;
                            recv(nm_sock, &req, sizeof(req), 0);
                            printf("Got REQ_SS_CREATE for '%s' from NM\n", req.filename);
                            handle_create_file(req.filename);
                            send_simple_header(nm_sock, RES_OK);
                            break;
                        }
                        default:
                            printf("Got unknown command %d from NM\n", header.type);
                    }
                }
                
                // B) --- New Client Connection ---
                else if (sock_fd == client_listener_sock) {
                    new_client_sock = accept(client_listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_client_sock < 0) { perror("accept new client"); }
                    else {
                        FD_SET(new_client_sock, &master_set);
                        if (new_client_sock > fdmax) fdmax = new_client_sock;
                        printf("New client connection on socket %d\n", new_client_sock);
                    }
                }
                
                // C) --- Activity from Existing Client ---
                else {
                    if (recv(sock_fd, &header, sizeof(Header), 0) <= 0) {
                        handle_client_disconnect(sock_fd, &master_set);
                    } else {
                        switch (header.type) {
                            case REQ_CLIENT_READ: {
                                Msg_Filename_Request req;
                                recv(sock_fd, &req, sizeof(req), 0);
                                printf("Got REQ_CLIENT_READ for '%s' from socket %d\n", req.filename, sock_fd);
                                handle_send_file(sock_fd, req.filename);
                                close(sock_fd); // READ is a one-shot operation
                                FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            
                            // --- NEW FOR PHASE 3 ---
                            case REQ_CLIENT_WRITE: {
                                Msg_Client_Write req;
                                recv(sock_fd, &req, sizeof(req), 0);
                                printf("Got REQ_CLIENT_WRITE for '%s' (sent %d) from socket %d\n", req.filename, req.sentence_num, sock_fd);

                                if (find_lock(req.filename, req.sentence_num) != -1) {
                                    printf("  -> Lock conflict! Sending error.\n");
                                    send_simple_header(sock_fd, RES_ERROR_LOCKED);
                                    close(sock_fd);
                                    FD_CLR(sock_fd, &master_set);
                                } else {
                                    create_lock(req.filename, req.sentence_num, sock_fd);
                                    
                                    WriteSession* session = &write_sessions[sock_fd];
                                    session->active = 1;
                                    strncpy(session->filename, req.filename, MAX_FILENAME);
                                    session->sentence_num = req.sentence_num;
                                    sprintf(session->original_path, "%s/%s", MY_STORAGE_PATH, req.filename);
                                    sprintf(session->temp_path, "%s/%s.tmp.%d", MY_STORAGE_PATH, req.filename, sock_fd);
                                    sprintf(session->backup_path, "%s/%s.bak", MY_STORAGE_PATH, req.filename);

                                    char cmd[520]; // Fixed size from warning
                                    snprintf(cmd, sizeof(cmd), "cp %s %s", session->original_path, session->temp_path);
                                    system(cmd); // Assumes file exists.

                                    session->temp_file = fopen(session->temp_path, "r+");
                                    if(session->temp_file == NULL) {
                                        perror("fopen temp file");
                                        send_simple_header(sock_fd, RES_ERROR);
                                        close(sock_fd);
                                        FD_CLR(sock_fd, &master_set);
                                    } else {
                                        printf("  -> Lock granted. Session started.\n");
                                        send_simple_header(sock_fd, RES_OK_LOCKED);
                                    }
                                }
                                break;
                            }
                            case REQ_WRITE_UPDATE: {
                                if (!write_sessions[sock_fd].active) {
                                    printf("Error: Got REQ_WRITE_UPDATE from socket %d with no active session.\n", sock_fd);
                                    break;
                                }
                                
                                Msg_Write_Update req;
                                recv(sock_fd, &req, sizeof(req), 0);
                                
                                // --- TODO IS DONE ---
                                // Call the new helper function to perform the complex update
                                handle_write_update(&write_sessions[sock_fd], &req);
                                // --------------------
                                
                                printf("  -> Applied update (word %d) to temp file for socket %d\n", req.word_index, sock_fd);
                                break;
                            }
                            case REQ_ETIRW: {
                                if (!write_sessions[sock_fd].active) {
                                    printf("Error: Got REQ_ETIRW from socket %d with no active session.\n", sock_fd);
                                    break;
                                }
                                
                                printf("Got REQ_ETIRW from socket %d. Committing changes.\n", sock_fd);
                                WriteSession* session = &write_sessions[sock_fd];
                                
                                fclose(session->temp_file);
                                rename(session->original_path, session->backup_path);
                                rename(session->temp_path, session->original_path);
                                release_lock(session->filename, session->sentence_num);
                                session->active = 0;
                                
                                send_simple_header(sock_fd, RES_OK);
                                close(sock_fd);
                                FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            // --- END PHASE 3 ---

                            default:
                                printf("Got unknown command %d from client %d\n", header.type, sock_fd);
                        }
                    }
                }
            }
        }
    } 
    return 0;
}