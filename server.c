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
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) continue;

        // 브라우저의 요청 읽기
        read(clnt_sock, buffer, BUFFER_SIZE);

        // [중요] login.html 파일 열기
        int fd = open("./html/login.html", O_RDONLY);
        if (fd == -1) {
            // 파일을 못 찾을 경우 404 에러 전송
            char error_resp[] = "HTTP/1.1 404 Not Found\r\n\r\n<h1>404 File Not Found</h1>";
            write(clnt_sock, error_resp, strlen(error_resp));
        } else {
            // 파일 크기 확인 (Content-Length를 정확히 주기 위함)
            struct stat st;
            fstat(fd, &st);
            long file_size = st.st_size;

            // HTTP 헤더 작성
            char header[256];
            sprintf(header, 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=UTF-8\r\n"
                "Content-Length: %ld\r\n"
                "\r\n", file_size);
            
            // 1. 헤더 먼저 전송
            write(clnt_sock, header, strlen(header));

            // 2. 파일 내용을 읽어서 전송 (파일 바디)
            int read_cnt;
            while ((read_cnt = read(fd, buffer, BUFFER_SIZE)) > 0) {
                write(clnt_sock, buffer, read_cnt);
            }
            close(fd);
        }

        close(clnt_sock);
    }

    close(serv_sock);
    return 0;
}