#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

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
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);

    if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("bind() error");
        exit(1);
    }

    if (listen(serv_sock, 5) == -1) {
        perror("listen() error");
        exit(1);
    }

    printf("Server started on port %d... Waiting for login.html requests\n", PORT);

while (1) {
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) continue;

        memset(buffer, 0, BUFFER_SIZE);
        read(clnt_sock, buffer, BUFFER_SIZE - 1);

        // 1. 요청 라인 파싱 (메서드와 URL 경로 추출)
        char method[10], path[255];
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
                char error_resp[] = "HTTP/1.1 404 Not Found\r\n\r\n<h1>Page Not Found</h1>";
                write(clnt_sock, error_resp, strlen(error_resp));
            }
        } 
        else if (strcmp(method, "POST") == 0) {
            // HTTP Body(데이터) 시작 지점 찾기 (\r\n\r\n 다음이 데이터 body)
            char *body = strstr(buffer, "\r\n\r\n");
            if (body != NULL) {
                body += 4; // "\r\n\r\n" 크기만큼 포인터 이동
            }

            if (strcmp(path, "/register") == 0) {
                // TODO: body에서 username=값&password=값 파싱
                // TODO: 파싱된 데이터를 로컬 파일(users.txt)에 저장하거나 DB에 insert
                
                // 가입 완료 후 로그인 페이지로 리다이렉트 응답 생성
                char register_ok[] = "HTTP/1.1 302 Found\r\nLocation: /login\r\n\r\n";
                write(clnt_sock, register_ok, strlen(register_ok));
            } 
            else if (strcmp(path, "/login") == 0) {
                // TODO: body에서 ID/PW 파싱 후 기존 가입 정보와 비교 인증
                // 인증 성공/실패에 따른 결과 화면 전송
            }
        }

        close(clnt_sock);
    }
    
    close(serv_sock);
    return 0;
}