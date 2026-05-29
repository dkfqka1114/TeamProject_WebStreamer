#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h> // 멀티스레드 처리를 위한 헤더

#define PORT 8080
#define BUFFER_SIZE 65536 // 이미지 데이터(Base64) 전송을 위해 버퍼 크기를 대폭 확장

// [스트리밍 핵심 전역 변수]
// 송출자가 보낸 최신 프레임 데이터(Base64 문자열)를 저장하는 전역 공간
char live_stream_buffer[500000] = {0}; 
pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER; // 데이터 레이스 방지를 위한 뮤텍스

// 정적 HTML 파일을 전송하는 헬퍼 함수
void send_file(int clnt_sock, const char* file_path) {
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) {
        char error_resp[] = "HTTP/1.1 404 Not Found\r\n\r\n<h1>404 File Not Found</h1>";
        write(clnt_sock, error_resp, strlen(error_resp));
        return;
    }
    struct stat st;
    fstat(fd, &st);
    long file_size = st.st_size;

    char header[256];
    sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n", file_size);
    write(clnt_sock, header, strlen(header));

    char buffer[4096];
    int read_cnt;
    while ((read_cnt = read(fd, buffer, sizeof(buffer))) > 0) {
        write(clnt_sock, buffer, read_cnt);
    }
    close(fd);
}

// 각 클라이언트 연결을 독립적으로 처리할 스레드 함수
void* handle_client(void* arg) {
    int clnt_sock = *((int*)arg);
    free(arg); // 동적 할당된 소켓 디스크립터 메모리 해제
    
    // 스레드가 종료되면 자원을 알아서 커널에 반환하도록 설정
    pthread_detach(pthread_self());

    char* buffer = (char*)malloc(BUFFER_SIZE);
    if (buffer == NULL) {
        close(clnt_sock);
        return NULL;
    }
    
    memset(buffer, 0, BUFFER_SIZE);
    int read_len = read(clnt_sock, buffer, BUFFER_SIZE - 1);
    if (read_len <= 0) {
        free(buffer);
        close(clnt_sock);
        return NULL;
    }

    char method[10] = {0};
    char path[255] = {0};
    sscanf(buffer, "%s %s", method, path);
    printf("[Thread %lu] Request: %s %s\n", (unsigned long)pthread_self(), method, path);

    int is_logged_in = (strstr(buffer, "Cookie: user=") != NULL);

    // 1. GET 라우팅 분기
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/main") == 0) {
            if (is_logged_in) {
                send_file(clnt_sock, "./main2.html");
            } else {
                char redirect_login[] = "HTTP/1.1 302 Found\r\nLocation: /login\r\n\r\n";
                write(clnt_sock, redirect_login, strlen(redirect_login));
            }
        } 
        else if (strcmp(path, "/login") == 0) {
            if (is_logged_in) {
                char redirect_main[] = "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n";
                write(clnt_sock, redirect_main, strlen(redirect_main));
            } else {
                send_file(clnt_sock, "./html/login.html");
            }
        } 
        else if (strcmp(path, "/register") == 0) {
            send_file(clnt_sock, "./html/register.html");
        }
        else if (strcmp(path, "/logout") == 0) {
            char logout_resp[] = "HTTP/1.1 302 Found\r\nSet-Cookie: user=; Max-Age=0; Path=/\r\nLocation: /login\r\n\r\n";
            write(clnt_sock, logout_resp, strlen(logout_resp));
        }
        // [스트리밍 수신 비동기 경로] 시청자가 최신 화면 프레임을 요구할 때
        else if (strcmp(path, "/live") == 0) {
            char* response = (char*)malloc(600000);
            
            pthread_mutex_lock(&stream_mutex);
            sprintf(response, 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %ld\r\n"
                "\r\n"
                "%s", strlen(live_stream_buffer), live_stream_buffer);
            pthread_mutex_unlock(&stream_mutex);
            
            write(clnt_sock, response, strlen(response));
            free(response);
        }
        else {
            char error_resp[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>Page Not Found</h1>";
            write(clnt_sock, error_resp, strlen(error_resp));
        }
    } 
    // 2. POST 라우팅 분기
    else if (strcmp(method, "POST") == 0) {
        char *body = strstr(buffer, "\r\n\r\n");
        body = (body != NULL) ? body + 4 : "";

        // [스트리밍 송출 비동기 경로] 송출자가 화면 프레임을 밀어 넣을 때
        if (strcmp(path, "/stream") == 0) {
            pthread_mutex_lock(&stream_mutex);
            memset(live_stream_buffer, 0, sizeof(live_stream_buffer));
            // 바디 전체(Base64 이미지 데이터)를 전역 스트림 버퍼에 덮어씌움
            strncpy(live_stream_buffer, body, sizeof(live_stream_buffer) - 1);
            pthread_mutex_unlock(&stream_mutex);

            char ok_resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
            write(clnt_sock, ok_resp, strlen(ok_resp));
        }
        else if (strcmp(path, "/register") == 0) {
            char username[100] = {0}, password[100] = {0};
            if (sscanf(body, "username=%[^&]&password=%s", username, password) == 2) {
                FILE *fp = fopen("users.txt", "a");
                if (fp != NULL) {
                    fprintf(fp, "%s %s\n", username, password);
                    fclose(fp);
                    
                    char alert_msg[] = "<script>alert('회원가입이 완료되었습니다!'); window.location.href = '/login';</script>";
                    char response[512];
                    sprintf(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n%s", strlen(alert_msg), alert_msg);
                    write(clnt_sock, response, strlen(response));
                }
            }
        } 
        else if (strcmp(path, "/login") == 0) {
            char username[100] = {0}, password[100] = {0};
            if (sscanf(body, "username=%[^&]&password=%s", username, password) == 2) {
                FILE *fp = fopen("users.txt", "r");
                int login_success = 0;

                if (fp != NULL) {
                    char file_user[100], file_pass[100];
                    while (fscanf(fp, "%s %s", file_user, file_pass) != EOF) {
                        if (strcmp(username, file_user) == 0 && strcmp(password, file_pass) == 0) {
                            login_success = 1;
                            break;
                        }
                    }
                    fclose(fp);
                }

                char alert_msg[512];
                char response[1024];

                if (login_success) {
                    sprintf(alert_msg, "<script>alert('%s님, 환영합니다!'); window.location.href = '/';</script>", username);
                    sprintf(response, "HTTP/1.1 200 OK\r\nSet-Cookie: user=%s; Path=/\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n%s", username, strlen(alert_msg), alert_msg);
                } else {
                    sprintf(alert_msg, "<script>alert('아이디 또는 비밀번호가 일치하지 않습니다.'); window.history.back();</script>");
                    sprintf(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n%s", strlen(alert_msg), alert_msg);
                }
                write(clnt_sock, response, strlen(response));
            }
        }
    }

    free(buffer);
    close(clnt_sock);
    return NULL;
}

int main() {
    int serv_sock;
    struct sockaddr_in serv_addr, clnt_addr;
    socklen_t clnt_addr_size;

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    int option = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);

    if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("bind() error");
        exit(1);
    }
    if (listen(serv_sock, 10) == -1) {
        perror("listen() error");
        exit(1);
    }

    printf("Multi-Threaded Server started on port %d...\n", PORT);

    while (1) {
        clnt_addr_size = sizeof(clnt_addr);
        int* clnt_sock_ptr = (int*)malloc(sizeof(int)); // 스레드간 소켓 값 변조 예방을 위해 동적 할당
        *clnt_sock_ptr = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        
        if (*clnt_sock_ptr == -1) {
            free(clnt_sock_ptr);
            continue;
        }

        // [핵심 차이] 클라이언트가 접속할 때마다 새로운 스레드를 생성하여 처리를 위임합니다.
        pthread_t t_id;
        if (pthread_create(&t_id, NULL, handle_client, (void*)clnt_sock_ptr) != 0) {
            perror("pthread_create() error");
            close(*clnt_sock_ptr);
            free(clnt_sock_ptr);
        }
    }
    close(serv_sock);
    return 0;
}