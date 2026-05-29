#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 2048 // 브라우저 헤더가 길어질 수 있으므로 버퍼를 조금 더 늘려줍니다.

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
    if (serv_sock == -1) {
        perror("socket() error");
        exit(1);
    }

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
        // [수정 핵심 1] accept 호출 전에 반드시 구조체 크기로 초기화해야 합니다.
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) continue;

        memset(buffer, 0, BUFFER_SIZE);
        int read_len = read(clnt_sock, buffer, BUFFER_SIZE - 1);
        
        if (read_len <= 0) {
            close(clnt_sock);
            continue;
        }

        // 1. 요청 라인 파싱 (메서드와 URL 경로 추출)
        char method[10] = {0};
        char path[255] = {0};
        sscanf(buffer, "%s %s", method, path);

        printf("Request: %s %s\n", method, path);

        // 2. 라우팅 분기 처리
        if (strcmp(method, "GET") == 0) {
            if (strcmp(path, "/") == 0 || strcmp(path, "/login") == 0) {
                send_file(clnt_sock, "./html/login.html");
            } 
            else if (strcmp(path, "/register") == 0) {
                send_file(clnt_sock, "./html/register.html");
            } 
            else {
                char error_resp[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>Page Not Found</h1>";
                write(clnt_sock, error_resp, strlen(error_resp));
            }
        } 
        else if (strcmp(method, "POST") == 0) {
            // HTTP Body(데이터) 시작 지점 찾기
            char *body = strstr(buffer, "\r\n\r\n");
            if (body != NULL) {
                body += 4; // "\r\n\r\n" 크기만큼 포인터 이동
            } else {
                body = "";
            }

            if (strcmp(path, "/register") == 0) {
                char username[100] = {0};
                char password[100] = {0};

                if (sscanf(body, "username=%[^&]&password=%s", username, password) == 2) {
                    FILE *fp = fopen("users.txt", "a");
                    
                    if (fp != NULL) {
                        fprintf(fp, "%s %s\n", username, password);
                        fclose(fp);
                        printf("[Server] 회원 가입 완료. ID: %s\n", username);

                        char alert_msg[] = 
                            "<script>"
                            "alert('회원가입 완료, 로그인 페이지로 이동합니다.');"
                            "window.location.href = '/login';"
                            "</script>";

                        // HTTP 200 OK 헤더와 함께 스크립트 전송
                        char response[512];
                        sprintf(response, 
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=UTF-8\r\n"
                            "Content-Length: %ld\r\n"
                            "\r\n"
                            "%s", strlen(alert_msg), alert_msg
                        );

                        write(clnt_sock, response, strlen(response));
                    } else {
                        perror("File open error");
                        char error_resp[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n<h1>500 Server Error</h1>";
                        write(clnt_sock, error_resp, strlen(error_resp));
                    }
                } else {
                    char bad_req[] = "HTTP/1.1 400 Bad Request\r\n\r\n<h1>400 Bad Request</h1>";
                    write(clnt_sock, bad_req, strlen(bad_req));
                }
            } 
            else if (strcmp(path, "/login") == 0) {
                char username[100] = {0};
                char password[100] = {0};

                // 1. 브라우저가 보낸 로그인 데이터 파싱
                if (sscanf(body, "username=%[^&]&password=%s", username, password) == 2) {
                    printf("[로그인 시도] ID: %s\n", username);

                    FILE *fp = fopen("users.txt", "r");
                    int login_success = 0;

                    if (fp != NULL) {
                        char file_user[100], file_pass[100];
                        
                        // 2. users.txt를 한 줄씩 읽으며 ID와 PW 검증
                        while (fscanf(fp, "%s %s", file_user, file_pass) != EOF) {
                            if (strcmp(username, file_user) == 0 && strcmp(password, file_pass) == 0) {
                                login_success = 1; // 일치하는 계정 발견
                                break;
                            }
                        }
                        fclose(fp);
                    }

                    char alert_msg[512];
                    // 3. 결과에 따른 자바스크립트 알림창 분기 처리
                    if (login_success) {
                        printf("[Server] 로그인 성공: %s\n", username);
                        sprintf(alert_msg, 
                            "<script>"
                            "alert('%s님, 환영합니다!');"
                            "window.location.href = '/';" // 추후 메인 홈이나 스트리밍 대시보드로 이동
                            "</script>", username);
                    } else {
                        printf("[Server] 로그인 실패: %s\n", username);
                        sprintf(alert_msg, 
                            "<script>"
                            "alert('아이디 또는 비밀번호가 일치하지 않습니다.');"
                            "window.history.back();" // 이전 로그인 창으로 되돌리기
                            "</script>");
                    }

                    // HTTP 200 OK 헤더와 함께 결과 스크립트 전송
                    char response[1024];
                    sprintf(response, 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=UTF-8\r\n"
                        "Content-Length: %ld\r\n"
                        "\r\n"
                        "%s", strlen(alert_msg), alert_msg
                    );
                    write(clnt_sock, response, strlen(response));

                } else {
                    char bad_req[] = "HTTP/1.1 400 Bad Request\r\n\r\n<h1>400 Bad Request</h1>";
                    write(clnt_sock, bad_req, strlen(bad_req));
                }
            }
        }

        close(clnt_sock);
    }
    
    close(serv_sock);
    return 0;
}