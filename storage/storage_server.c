#include "protocol.h"
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/select.h>
#include <fcntl.h>
#include <malloc.h> 
#include <ctype.h>
#include <errno.h>
#include <time.h>     // For logging timestamp
#include <stdarg.h>   // For ... in log_event

#define MY_IP_FOR_CLIENTS "127.0.0.1"
#define MY_PORT_FOR_CLIENTS 8082
#define MY_STORAGE_PATH "./ss_storage"

#define MAX_CONNECTIONS FD_SETSIZE
#define MAX_LOCKS 100

// --- NEW: Global Log File ---
FILE* ss_log_file;

// --- 1. Global State & Helpers for SS ---
// (Structs, init_locks, find_lock, create_lock, release_lock... same as Phase 3)
typedef struct { int active; char filename[MAX_FILENAME]; int sentence_num; int sock_fd; } LockInfo;
LockInfo global_locks[MAX_LOCKS];
typedef struct { int active; char filename[MAX_FILENAME]; int sentence_num; char original_path[256]; char temp_path[256]; char backup_path[256]; FILE* temp_file; } WriteSession;
WriteSession write_sessions[MAX_CONNECTIONS];
void init_locks() { for (int i = 0; i < MAX_LOCKS; i++) global_locks[i].active = 0; }
int find_lock(char* f, int s) { for (int i = 0; i < MAX_LOCKS; i++) if (global_locks[i].active && !strcmp(global_locks[i].filename, f) && global_locks[i].sentence_num == s) return i; return -1; }
int create_lock(char* f, int s, int sock) { if (find_lock(f, s) != -1) return 0; for (int i = 0; i < MAX_LOCKS; i++) if (!global_locks[i].active) { global_locks[i].active = 1; strncpy(global_locks[i].filename, f, MAX_FILENAME); global_locks[i].sentence_num = s; global_locks[i].sock_fd = sock; log_event("  -> Lock CREATED for '%s' (sent %d) by socket %d", f, s, sock); return 1; } return -1; }
void release_lock(char* f, int s) { int i = find_lock(f, s); if (i != -1) { global_locks[i].active = 0; log_event("  -> Lock RELEASED for '%s' (sent %d)", f, s); } }


// --- NEW: Logging Function ---
void log_event(const char* format, ...) {
    char time_buf[50];
    time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    va_list args;
    
    // Print to stdout
    printf("[%s] ", time_buf);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");

    // Print to log file
    fprintf(ss_log_file, "[%s] ", time_buf);
    va_start(args, format);
    vfprintf(ss_log_file, format, args);
    va_end(args);
    fprintf(ss_log_file, "\n");
    
    fflush(ss_log_file); // Ensure it's written immediately
}

// --- 3. File Operation Helpers ---
void handle_create_file(char* filename) { char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename); log_event("  -> Creating file at: %s", file_path); FILE* f = fopen(file_path, "w"); if (f) fclose(f); }
void handle_send_file(int client_sock, char* filename) { char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename); int fd = open(file_path, O_RDONLY); if (fd < 0) { log_event("  -> File not found. Sending error to socket %d", client_sock); send_simple_header(client_sock, RES_ERROR_NOT_FOUND); return; } send_simple_header(client_sock, RES_SS_FILE_OK); log_event("  -> Sending file '%s' to socket %d", filename, client_sock); char buffer[FILE_BUFFER_SIZE]; int bytes_read; while ((bytes_read = read(fd, buffer, FILE_BUFFER_SIZE)) > 0) if (send(client_sock, buffer, bytes_read, 0) < 0) break; close(fd); log_event("  -> Finished sending file to socket %d", client_sock); }

// --- 4. Disconnect Helper ---
void handle_client_disconnect(int sock_fd, fd_set* master_set) {
    // Get client IP *before* closing
    struct sockaddr_in addr; socklen_t len = sizeof(addr);
    char ip_buf[MAX_IP_LEN] = "UNKNOWN_IP";
    if (getpeername(sock_fd, (struct sockaddr*)&addr, &len) == 0) {
        strncpy(ip_buf, inet_ntoa(addr.sin_addr), MAX_IP_LEN);
    }
    
    log_event("Client on socket %d (%s) disconnected", sock_fd, ip_buf);
    if (write_sessions[sock_fd].active) { WriteSession* session = &write_sessions[sock_fd]; log_event("  -> Client was in a write session! Rolling back changes."); release_lock(session->filename, session->sentence_num); fclose(session->temp_file); remove(session->temp_path); session->active = 0; }
    close(sock_fd);
    FD_CLR(sock_fd, master_set);
}

// --- 5. Write Update Helper ---
// (The complex handle_write_update function with INSERT logic... same as Phase 3 fix)
void handle_write_update(WriteSession* session, Msg_Write_Update* req) {
    rewind(session->temp_file); fseek(session->temp_file, 0, SEEK_END); long file_size = ftell(session->temp_file); rewind(session->temp_file);
    char* mem_buffer = (char*)malloc(file_size + 1); char* file_content_orig = (char*)malloc(file_size + 1);
    if (!mem_buffer || !file_content_orig) { perror("malloc update buffer"); return; }
    fread(mem_buffer, 1, file_size, session->temp_file); mem_buffer[file_size] = '\0'; strcpy(file_content_orig, mem_buffer);
    long new_buf_size = file_size + strlen(req->content) + 1024; char* new_mem_buffer = (char*)malloc(new_buf_size);
    char* write_ptr = new_mem_buffer;
    if (!new_mem_buffer) { perror("malloc new buffer"); free(mem_buffer); free(file_content_orig); return; }
    *write_ptr = '\0'; char* sent_context = NULL; char* sentence = strtok_r(mem_buffer, ".!?", &sent_context); int sent_count = 0;
    while (sentence != NULL) {
        long sent_start_offset = sentence - mem_buffer; long orig_sent_len = strlen(sentence); char delimiter = file_content_orig[sent_start_offset + orig_sent_len];
        while (isspace((unsigned char)*sentence)) { *write_ptr++ = *sentence++; }
        long sent_len = strlen(sentence);
        if (sent_count == session->sentence_num) {
            log_event("  -> Modifying sentence %d", sent_count);
            char* word_context = NULL; char* word = strtok_r(sentence, " ", &word_context); int word_count = 0;
            while (word != NULL && word_count < req->word_index) { strcpy(write_ptr, word); write_ptr += strlen(word); *write_ptr++ = ' '; word_count++; word = strtok_r(NULL, " ", &word_context); }
            strcpy(write_ptr, req->content); write_ptr += strlen(req->content);
            if (word != NULL) {
                *write_ptr++ = ' '; strcpy(write_ptr, word); write_ptr += strlen(word); *write_ptr++ = ' ';
                word = strtok_r(NULL, " ", &word_context);
                while (word != NULL) { strcpy(write_ptr, word); write_ptr += strlen(word); *write_ptr++ = ' '; word = strtok_r(NULL, " ", &word_context); }
            }
            if (write_ptr > new_mem_buffer && *(write_ptr - 1) == ' ') { write_ptr--; }
        } else { strcpy(write_ptr, sentence); write_ptr += sent_len; }
        if (delimiter != '\0') { *write_ptr++ = delimiter; }
        sent_count++; sentence = strtok_r(NULL, ".!?", &sent_context);
    }
    *write_ptr = '\0'; rewind(session->temp_file); fputs(new_mem_buffer, session->temp_file); fflush(session->temp_file); ftruncate(fileno(session->temp_file), ftell(session->temp_file));
    free(mem_buffer); free(file_content_orig); free(new_mem_buffer);
}

// --- NEW FOR PHASE 4: Stream Helper ---
void handle_stream_file(int client_sock, char* filename) {
    char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, filename);
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) { log_event("  -> File not found. Sending error to socket %d", client_sock); send_simple_header(client_sock, RES_ERROR_NOT_FOUND); return; }
    send_simple_header(client_sock, RES_SS_FILE_OK);
    lseek(fd, 0, SEEK_END); long file_size = lseek(fd, 0, SEEK_CUR); lseek(fd, 0, SEEK_SET);
    char* mem_buffer = (char*)malloc(file_size + 1);
    if (!mem_buffer) { perror("malloc stream buffer"); close(fd); return; }
    read(fd, mem_buffer, file_size); mem_buffer[file_size] = '\0'; close(fd);
    log_event("  -> Streaming file '%s' to socket %d", filename, client_sock);
    char* word_context = NULL; char* word = strtok_r(mem_buffer, " \n\t", &word_context);
    while (word != NULL) {
        if (send(client_sock, word, strlen(word), 0) < 0) break;
        if (send(client_sock, " ", 1, 0) < 0) break;
        usleep(100000); 
        word = strtok_r(NULL, " \n\t", &word_context);
    }
    free(mem_buffer); log_event("  -> Finished streaming file to socket %d", client_sock);
}


// --- 6. Main Function ---
int main() {
    // --- Open Log File ---
    ss_log_file = fopen("ss.log", "a");
    if (ss_log_file == NULL) error_exit("fopen ss.log");
    log_event("--- Storage Server Started ---");
    
    int nm_sock, client_listener_sock, new_client_sock;
    struct sockaddr_in nm_addr, client_listen_addr, client_addr;
    socklen_t client_len;
    fd_set master_set, read_set;
    int fdmax;
    init_locks();
    for(int i = 0; i < MAX_CONNECTIONS; i++) write_sessions[i].active = 0;
    
    char my_files[MAX_FILES_PER_SS][MAX_FILENAME];
    int file_count = 0;
    mkdir(MY_STORAGE_PATH, 0777);
    log_event("Scanning storage directory: %s", MY_STORAGE_PATH);
    DIR *d = opendir(MY_STORAGE_PATH);
    if (d) { struct dirent *dir; while ((dir = readdir(d)) != NULL && file_count < MAX_FILES_PER_SS) { if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue; strncpy(my_files[file_count], dir->d_name, MAX_FILENAME); file_count++; } closedir(d); }
    log_event("Found %d files.", file_count);
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) error_exit("socket");
    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) error_exit("inet_pton");
    if (connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0) error_exit("connect");
    log_event("Storage Server connected to Name Server on port %d", NM_PORT);
    
    Header header; header.type = REQ_SS_REGISTER; header.payload_size = sizeof(Msg_SS_Register);
    Msg_SS_Register reg_msg; strncpy(reg_msg.ss_ip, MY_IP_FOR_CLIENTS, MAX_IP_LEN); reg_msg.client_port = MY_PORT_FOR_CLIENTS; reg_msg.file_count = file_count;
    if (send(nm_sock, &header, sizeof(Header), 0) < 0) error_exit("send header");
    if (send(nm_sock, &reg_msg, sizeof(reg_msg), 0) < 0) error_exit("send payload");
    log_event("Sending file list...");
    for (int i = 0; i < file_count; i++) { Msg_File_Item item; strncpy(item.filename, my_files[i], MAX_FILENAME); if (send(nm_sock, &item, sizeof(item), 0) < 0) error_exit("send file item"); }
    if (recv(nm_sock, &header, sizeof(Header), 0) < 0 || header.type != RES_OK) error_exit("Registration failed");
    log_event("Registration successful!");
    
    client_listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_listener_sock < 0) error_exit("client listener socket");
    int yes = 1; if (setsockopt(client_listener_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) error_exit("setsockopt");
    memset(&client_listen_addr, 0, sizeof(client_listen_addr));
    client_listen_addr.sin_family = AF_INET; client_listen_addr.sin_addr.s_addr = INADDR_ANY; client_listen_addr.sin_port = htons(MY_PORT_FOR_CLIENTS);
    if (bind(client_listener_sock, (struct sockaddr *)&client_listen_addr, sizeof(client_listen_addr)) < 0) error_exit("client listener bind");
    if (listen(client_listener_sock, 10) < 0) error_exit("client listener listen");
    log_event("Storage Server now listening for clients on port %d...", MY_PORT_FOR_CLIENTS);
    
    FD_ZERO(&master_set); FD_SET(nm_sock, &master_set); FD_SET(client_listener_sock, &master_set);
    fdmax = (nm_sock > client_listener_sock) ? nm_sock : client_listener_sock;

    // --- Main Server Loop ---
    while (1) {
        read_set = master_set;
        if (select(fdmax + 1, &read_set, NULL, NULL, NULL) < 0) { log_event("select() error"); error_exit("select"); }

        for (int sock_fd = 0; sock_fd <= fdmax; sock_fd++) {
            if (FD_ISSET(sock_fd, &read_set)) {
                
                // A) --- Activity from Name Server ---
                if (sock_fd == nm_sock) {
                    if (recv(nm_sock, &header, sizeof(Header), 0) <= 0) {
                        log_event("Name Server disconnected!"); error_exit("Name Server disconnected");
                    }
                    switch (header.type) {
                        case REQ_SS_CREATE: {
                            Msg_Filename_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_CREATE for '%s' from NM", req.filename);
                            handle_create_file(req.filename); send_simple_header(nm_sock, RES_OK);
                            break;
                        }
                        case REQ_SS_DELETE: {
                            Msg_Filename_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_DELETE for '%s' from NM", req.filename);
                            char file_path[256]; sprintf(file_path, "%s/%s", MY_STORAGE_PATH, req.filename);
                            if (remove(file_path) == 0) { log_event("  -> File deleted."); }
                            else { log_event("  -> Error deleting file: %s", strerror(errno)); }
                            char backup_path[256]; sprintf(backup_path, "%s/%s.bak", MY_STORAGE_PATH, req.filename);
                            remove(backup_path);
                            send_simple_header(nm_sock, RES_OK);
                            break;
                        }
                        case REQ_SS_UNDO: {
                            Msg_Filename_Request req; recv(nm_sock, &req, sizeof(req), 0);
                            log_event("Got REQ_SS_UNDO for '%s' from NM", req.filename);
                            char original_path[256], backup_path[256], temp_swap_path[256];
                            sprintf(original_path, "%s/%s", MY_STORAGE_PATH, req.filename);
                            sprintf(backup_path, "%s/%s.bak", MY_STORAGE_PATH, req.filename);
                            sprintf(temp_swap_path, "%s/%s.swap", MY_STORAGE_PATH, req.filename);
                            rename(original_path, temp_swap_path); rename(backup_path, original_path); rename(temp_swap_path, backup_path);
                            log_event("  -> Swapped backup file."); send_simple_header(nm_sock, RES_OK);
                            break;
                        }
                        default:
                            log_event("Got unknown command %d from NM", header.type);
                    }
                }
                
                // B) --- New Client Connection ---
                else if (sock_fd == client_listener_sock) {
                    client_len = sizeof(client_addr);
                    new_client_sock = accept(client_listener_sock, (struct sockaddr *)&client_addr, &client_len);
                    if (new_client_sock < 0) { perror("accept new client"); }
                    else {
                        FD_SET(new_client_sock, &master_set);
                        if (new_client_sock > fdmax) fdmax = new_client_sock;
                        log_event("New client connection from %s on socket %d", inet_ntoa(client_addr.sin_addr), new_client_sock);
                    }
                }
                
                // C) --- Activity from Existing Client ---
                else {
                    if (recv(sock_fd, &header, sizeof(Header), 0) <= 0) {
                        handle_client_disconnect(sock_fd, &master_set);
                    } else {
                        switch (header.type) {
                            case REQ_CLIENT_READ: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CLIENT_READ for '%s' from socket %d", req.filename, sock_fd);
                                handle_send_file(sock_fd, req.filename);
                                close(sock_fd); FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            case REQ_CLIENT_STREAM: {
                                Msg_Filename_Request req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CLIENT_STREAM for '%s' from socket %d", req.filename, sock_fd);
                                handle_stream_file(sock_fd, req.filename);
                                close(sock_fd); FD_CLR(sock_fd, &master_set);
                                break;
                            }
                            case REQ_CLIENT_WRITE: {
                                Msg_Client_Write req; recv(sock_fd, &req, sizeof(req), 0);
                                log_event("Got REQ_CLIENT_WRITE for '%s' (sent %d) from socket %d", req.filename, req.sentence_num, sock_fd);
                                if (find_lock(req.filename, req.sentence_num) != -1) {
                                    log_event("  -> Lock conflict! Sending error.");
                                    send_simple_header(sock_fd, RES_ERROR_LOCKED);
                                    close(sock_fd); FD_CLR(sock_fd, &master_set);
                                } else {
                                    create_lock(req.filename, req.sentence_num, sock_fd);
                                    WriteSession* session = &write_sessions[sock_fd];
                                    session->active = 1; strncpy(session->filename, req.filename, MAX_FILENAME);
                                    session->sentence_num = req.sentence_num;
                                    sprintf(session->original_path, "%s/%s", MY_STORAGE_PATH, req.filename);
                                    sprintf(session->temp_path, "%s/%s.tmp.%d", MY_STORAGE_PATH, req.filename, sock_fd);
                                    sprintf(session->backup_path, "%s/%s.bak", MY_STORAGE_PATH, req.filename);
                                    char cmd[520]; snprintf(cmd, sizeof(cmd), "cp %s %s", session->original_path, session->temp_path);
                                    system(cmd);
                                    session->temp_file = fopen(session->temp_path, "r+");
                                    if(session->temp_file == NULL) {
                                        log_event("  -> ERROR: fopen temp file failed.");
                                        send_simple_header(sock_fd, RES_ERROR);
                                        close(sock_fd); FD_CLR(sock_fd, &master_set);
                                    } else {
                                        log_event("  -> Lock granted. Session started.");
                                        send_simple_header(sock_fd, RES_OK_LOCKED);
                                    }
                                }
                                break;
                            }
                            case REQ_WRITE_UPDATE: {
                                if (!write_sessions[sock_fd].active) { log_event("Error: Got REQ_WRITE_UPDATE from socket %d with no active session.", sock_fd); break; }
                                Msg_Write_Update req; recv(sock_fd, &req, sizeof(req), 0);
                                handle_write_update(&write_sessions[sock_fd], &req);
                                log_event("  -> Applied update (word %d) to temp file for socket %d", req.word_index, sock_fd);
                                break;
                            }
                            case REQ_ETIRW: {
                                if (!write_sessions[sock_fd].active) { log_event("Error: Got REQ_ETIRW from socket %d with no active session.", sock_fd); break; }
                                log_event("Got REQ_ETIRW from socket %d. Committing changes.", sock_fd);
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
                            default:
                                log_event("Got unknown command %d from client %d", header.type, sock_fd);
                        }
                    }
                }
            }
        }
    } 
    return 0;
}