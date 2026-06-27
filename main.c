#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h> 

#define PORT 8080
#define BUFFER_SIZE 65536
#define MAX_ROOMS 12       
#define STREAM_SIZE 1500000
#define MAX_CHATS 50

typedef struct {
    char user[50];
    char message[256];
} ChatMessage;

typedef struct {
    char room_id[100];   
    char stream_buffer[STREAM_SIZE]; 
    int is_active;       
    int viewer_count;    
    time_t last_heartbeat; 
    ChatMessage chat_list[MAX_CHATS];
    int chat_count;
} StreamRoom;

StreamRoom rooms[MAX_ROOMS] = {0};
pthread_mutex_t room_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

int handle_auth(int clnt_sock, const char* method, const char* path, const char* body, int is_logged_in);
int handle_stream(int clnt_sock, const char* method, const char* path, const char* body, const char* session_username, long total_read, char* buffer);
void send_main_page(int clnt_sock, const char* username, int page);

// 공유용 정적 파일 전송 헬퍼 함수
void send_file(int clnt_sock, const char* file_path) {
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) {
        char error_resp[] = "HTTP/1.1 404 Not Found\r\n\r\n<h1>404 File Not Found</h1>";
        write(clnt_sock, error_resp, strlen(error_resp));
        return;
    }
    struct stat st; fstat(fd, &st); long file_size = st.st_size;
    char header[256]; sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n", file_size);
    write(clnt_sock, header, strlen(header));
    char buffer[4096]; int read_cnt;
    while ((read_cnt = read(fd, buffer, sizeof(buffer))) > 0) write(clnt_sock, buffer, read_cnt);
    close(fd);
}

// 스레드 핸들러
void* handle_client(void* arg) {
    int clnt_sock = *((int*)arg); free(arg); 
    pthread_detach(pthread_self());

    char* buffer = (char*)malloc(STREAM_SIZE + 8192);
    if (buffer == NULL) { close(clnt_sock); return NULL; }
    
    memset(buffer, 0, STREAM_SIZE + 8192);
    int total_read = read(clnt_sock, buffer, BUFFER_SIZE - 1); 
    if (total_read <= 0) { free(buffer); close(clnt_sock); return NULL; }

    char method[10] = {0}, path[512] = {0}; 
    sscanf(buffer, "%9s %511s", method, path);

    // TCP 대용량 스트림 분할 수신 안전장치
    if (strcmp(method, "POST") == 0) {
        char *content_len_ptr = strstr(buffer, "Content-Length:");
        int content_length = 0;
        if (content_len_ptr != NULL) sscanf(content_len_ptr, "Content-Length: %d", &content_length);
        
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (body_start != NULL) {
            body_start += 4;
            long header_len = body_start - buffer;
            if (content_length > (STREAM_SIZE + 8192 - 1) - header_len) {
                char too_large[] = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n";
                write(clnt_sock, too_large, strlen(too_large));
                free(buffer);
                close(clnt_sock);
                return NULL;
            }

            int current_body_len = total_read - header_len;
            while (current_body_len < content_length) {
                int r = read(clnt_sock, buffer + total_read, (STREAM_SIZE + 8192) - total_read - 1);
                if (r <= 0) break;
                total_read += r;
                buffer[total_read] = '\0';
                current_body_len = total_read - header_len;
            }
        }
    }

    char session_username[100] = "Guest";
    char *cookie_ptr = strstr(buffer, "Cookie: user=");
    if (cookie_ptr != NULL) sscanf(cookie_ptr, "Cookie: user=%99[^;\r\n]", session_username);
    int is_logged_in = (strstr(buffer, "Cookie: user=") != NULL);

    char *body = strstr(buffer, "\r\n\r\n");
    body = (body != NULL) ? body + 4 : "";

    if (handle_auth(clnt_sock, method, path, body, is_logged_in)) { /* auth.c가 처리 완료 */ }
    else if (handle_stream(clnt_sock, method, path, body, session_username, total_read, buffer)) { /* stream.c가 처리 완료 */ }
    else if (strncmp(path, "/", 1) == 0 && (strstr(path, "/main") || strcmp(path, "/") == 0)) {
        if (is_logged_in) send_main_page(clnt_sock, session_username, 1);
        else { char redirect[] = "HTTP/1.1 302 Found\r\nLocation: /login\r\n\r\n"; write(clnt_sock, redirect, strlen(redirect)); }
    }
    else {
        char error[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>Page Not Found</h1>";
        write(clnt_sock, error, strlen(error));
    }

    free(buffer); close(clnt_sock); return NULL;
}

int main() {
    int serv_sock; struct sockaddr_in serv_addr, clnt_addr; socklen_t clnt_addr_size;
    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    int option = 1; setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET; serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); serv_addr.sin_port = htons(PORT);

    if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1 || listen(serv_sock, 10) == -1) {
        perror("Socket Setup Error"); exit(1);
    }
    printf("3-Module Source Code Cluster Started on port %d...\n", PORT);

    while (1) {
        clnt_addr_size = sizeof(clnt_addr);
        int* clnt_sock_ptr = (int*)malloc(sizeof(int)); 
        *clnt_sock_ptr = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (*clnt_sock_ptr == -1) { free(clnt_sock_ptr); continue; }

        pthread_t t_id;
        pthread_create(&t_id, NULL, handle_client, (void*)clnt_sock_ptr);
    }
    close(serv_sock); return 0;
}
