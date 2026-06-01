#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

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

extern StreamRoom rooms[MAX_ROOMS];
extern pthread_mutex_t file_mutex;

void send_file(int clnt_sock, const char* file_path);

int handle_auth(int clnt_sock, const char* method, const char* path, const char* body, int is_logged_in) {
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/login") == 0) {
            if (is_logged_in) {
                char redirect_main[] = "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n";
                write(clnt_sock, redirect_main, strlen(redirect_main));
            } else {
                send_file(clnt_sock, "./html/login.html");
            }
            return 1; 
        }
        else if (strcmp(path, "/register") == 0) {
            send_file(clnt_sock, "./html/register.html");
            return 1;
        }
        else if (strcmp(path, "/logout") == 0) {
            char logout_resp[] = "HTTP/1.1 302 Found\r\nSet-Cookie: user=; Max-Age=0; Path=/\r\nLocation: /login\r\n\r\n";
            write(clnt_sock, logout_resp, strlen(logout_resp));
            return 1;
        }
    }
    else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/register") == 0) {
            char username[100] = {0}, password[100] = {0};
            if (sscanf(body, "username=%99[^&]&password=%99s", username, password) == 2) {
                int id_exists = 0;

                pthread_mutex_lock(&file_mutex); 
                FILE *fp_read = fopen("users.txt", "r");
                if (fp_read != NULL) {
                    char file_user[100], file_pass[100];
                    while (fscanf(fp_read, "%99s %99s", file_user, file_pass) != EOF) {
                        if (strcmp(username, file_user) == 0) {
                            id_exists = 1;
                            break;
                        }
                    }
                    fclose(fp_read);
                }

                char alert_msg[512];
                char response[1024];

                if (id_exists) {
                    pthread_mutex_unlock(&file_mutex); 
                    printf("[Server] 회원가입 실패 (중복 ID): %s\n", username);
                    sprintf(alert_msg, "<script>alert('이미 존재하는 아이디입니다.'); window.history.back();</script>");
                    sprintf(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n%s", strlen(alert_msg), alert_msg);
                } else {
                    FILE *fp_write = fopen("users.txt", "a");
                    if (fp_write != NULL) {
                        fprintf(fp_write, "%s %s\n", username, password);
                        fclose(fp_write);
                    }
                    pthread_mutex_unlock(&file_mutex);
                    printf("[Server] 회원가입 완료: %s\n", username);
                    sprintf(alert_msg, "<script>alert('회원가입이 완료되었습니다!'); window.location.href = '/login';</script>");
                    sprintf(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n%s", strlen(alert_msg), alert_msg);
                }
                write(clnt_sock, response, strlen(response));
            }
            return 1;
        }
        else if (strcmp(path, "/login") == 0) {
            char username[100] = {0}, password[100] = {0};
            if (sscanf(body, "username=%99[^&]&password=%99s", username, password) == 2) {
                pthread_mutex_lock(&file_mutex);
                FILE *fp = fopen("users.txt", "r");
                int login_success = 0;

                if (fp != NULL) {
                    char file_user[100], file_pass[100];
                    while (fscanf(fp, "%99s %99s", file_user, file_pass) != EOF) {
                        if (strcmp(username, file_user) == 0 && strcmp(password, file_pass) == 0) {
                            login_success = 1;
                            break;
                        }
                    }
                    fclose(fp);
                }
                pthread_mutex_unlock(&file_mutex);

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
            return 1;
        }
    }
    return 0; 
}