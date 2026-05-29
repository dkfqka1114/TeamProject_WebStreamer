#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 2048

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

    char buffer[BUFFER_SIZE];
    int read_cnt;
    while ((read_cnt = read(fd, buffer, BUFFER_SIZE)) > 0) {
        write(clnt_sock, buffer, read_cnt);
    }
    close(fd);
}

int main() {
    int serv_sock, clnt_sock;
    struct sockaddr_in serv_addr, clnt_addr;
    socklen_t clnt_addr_size;
    char buffer[BUFFER_SIZE];

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

    printf("Server started on port %d... Waiting for requests\n", PORT);

    while (1) {
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) continue;

        memset(buffer, 0, BUFFER_SIZE);
        int read_len = read(clnt_sock, buffer, BUFFER_SIZE - 1);
        if (read_len <= 0) {
            close(clnt_sock);
            continue;
        }

        char method[10] = {0};
        char path[255] = {0};
        sscanf(buffer, "%s %s", method, path);
        printf("Request: %s %s\n", method, path);

        // [인증 검증] 헤더에 "Cookie: user=" 문자열이 포함되어 있는지 확인
        int is_logged_in = (strstr(buffer, "Cookie: user=") != NULL);

        // 1. GET 라우팅 분기
        if (strcmp(method, "GET") == 0) {
            if (strcmp(path, "/") == 0 || strcmp(path, "/main") == 0) {
                // 로그인된 상태면 main.html을 보여주고, 아니면 login창으로 튕김
                if (is_logged_in) {
                    send_file(clnt_sock, "./html/main.html");
                } else {
                    char redirect_login[] = "HTTP/1.1 302 Found\r\nLocation: /login\r\n\r\n";
                    write(clnt_sock, redirect_login, strlen(redirect_login));
                }
            } 
            else if (strcmp(path, "/login") == 0) {
                // 이미 로그인되어 있으면 굳이 로그인창 안 보여주고 메인으로 토스
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
                // 로그아웃 요청 시 브라우저 쿠키 만료 처리(Max-Age=0) 후 로그인 창으로 이동
                char logout_resp[] = 
                    "HTTP/1.1 302 Found\r\n"
                    "Set-Cookie: user=; Max-Age=0; Path=/\r\n"
                    "Location: /login\r\n\r\n";
                write(clnt_sock, logout_resp, strlen(logout_resp));
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

            if (strcmp(path, "/register") == 0) {
                char username[100] = {0}, password[100] = {0};
                if (sscanf(body, "username=%[^&]&password=%s", username, password) == 2) {
                    FILE *fp = fopen("users.txt", "a");
                    if (fp != NULL) {
                        fprintf(fp, "%s %s\n", username, password);
                        fclose(fp);
                        
                        char alert_msg[] = 
                            "<script>alert('회원가입이 완료되었습니다!'); window.location.href = '/login';</script>";
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
                        printf("[Server] 로그인 성공: %s\n", username);
                        sprintf(alert_msg, 
                            "<script>alert('%s님, 환영합니다!'); window.location.href = '/';</script>", username);
                        
                        // [핵심 변경] 헤더에 Set-Cookie를 추가하여 브라우저에 쿠키를 저장시킵니다.
                        sprintf(response, 
                            "HTTP/1.1 200 OK\r\n"
                            "Set-Cookie: user=%s; Path=/\r\n"
                            "Content-Type: text/html; charset=UTF-8\r\n"
                            "Content-Length: %ld\r\n\r\n%s", username, strlen(alert_msg), alert_msg);
                    } else {
                        printf("[Server] 로그인 실패: %s\n", username);
                        sprintf(alert_msg, 
                            "<script>alert('아이디 또는 비밀번호가 일치하지 않습니다.'); window.history.back();</script>");
                        sprintf(response, 
                            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n%s", strlen(alert_msg), alert_msg);
                    }
                    write(clnt_sock, response, strlen(response));
                }
            }
        }
        close(clnt_sock);
    }
    close(serv_sock);
    return 0;
}