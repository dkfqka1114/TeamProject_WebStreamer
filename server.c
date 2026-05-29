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
#define STREAM_SIZE 500000 // 방마다 약 500KB의 고유 프레임 메모리 버퍼 할당

typedef struct {
    char room_id[100];   
    char stream_buffer[STREAM_SIZE]; // [추가] 각 방 마다 개별 할당된 화면 버퍼 공간
    int is_active;       
    int viewer_count;    
    time_t last_heartbeat; 
} StreamRoom;

// [글로벌 데이터 공간]
StreamRoom rooms[MAX_ROOMS] = {0};
pthread_mutex_t room_mutex = PTHREAD_MUTEX_INITIALIZER; 

void replace_string(char *src, const char *target, const char *replacement, char *dest) {
    char *p = strstr(src, target);
    if (!p) {
        strcpy(dest, src);
        return;
    }
    strncpy(dest, src, p - src);
    dest[p - src] = '\0';
    sprintf(dest + (p - src), "%s%s", replacement, p + strlen(target));
}

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

void send_main_page(int clnt_sock, const char* username, int page) {
    int fd = open("./html/main.html", O_RDONLY);
    if (fd == -1) {
        char error_resp[] = "HTTP/1.1 404 Not Found\r\n\r\n<h1>main2.html Not Found</h1>";
        write(clnt_sock, error_resp, strlen(error_resp));
        return;
    }
    
    struct stat st;
    fstat(fd, &st);
    char *html_tmpl = (char*)malloc(st.st_size + 1);
    read(fd, html_tmpl, st.st_size);
    html_tmpl[st.st_size] = '\0';
    close(fd);

    int items_per_page = 5; 
    int total_active_rooms = 0;
    time_t current_time = time(NULL);
    
    pthread_mutex_lock(&room_mutex);
    // 5초 동안 스트리머의 화면 갱신 패킷이 끊어지면 자동 방 폭파 처리 (Alive 체크 고도화)
    for(int i = 0; i < MAX_ROOMS; i++) {
        if(rooms[i].is_active) {
            if(current_time - rooms[i].last_heartbeat > 5) {
                printf("[System] 스트리머 %s 님의 송출 중단 확인. 방 폭파.\n", rooms[i].room_id);
                rooms[i].is_active = 0;
                rooms[i].viewer_count = 0;
                memset(rooms[i].stream_buffer, 0, STREAM_SIZE);
            } else {
                total_active_rooms++;
            }
        }
    }

    int total_pages = (total_active_rooms + items_per_page - 1) / items_per_page;
    if (total_pages == 0) total_pages = 1;
    if (page < 1) page = 1;
    if (page > total_pages) page = total_pages;

    char stream_rows_html[16384] = {0};
    int current_item_count = 0;
    int start_idx = (page - 1) * items_per_page;
    int end_idx = start_idx + items_per_page;
    int printed_count = 0;

    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].is_active) {
            if (current_item_count >= start_idx && current_item_count < end_idx) {
                char row_buffer[1024];
                sprintf(row_buffer, 
                    "<div class=\"stream-row\">"
                    "   <div class=\"stream-title\">🎬 %s 님의 라이브 방송 채널</div>"
                    "   <div>%s</div>"
                    "   <div class=\"viewer-count\">%d명 시청 중</div>"
                    "   <div><a href=\"/watch?room=%s\" target=\"_blank\" class=\"watch-btn\">시청하기</a></div>"
                    "</div>", rooms[i].room_id, rooms[i].room_id, rooms[i].viewer_count, rooms[i].room_id);
                strcat(stream_rows_html, row_buffer);
                printed_count++;
            }
            current_item_count++;
        }
    }
    pthread_mutex_unlock(&room_mutex);

    if (printed_count == 0) {
        strcpy(stream_rows_html, "<div class=\"empty-row\">현재 진행 중인 라이브 방송이 없습니다.</div>");
    }

    char prev_page_str[10], next_page_str[10], curr_page_str[10], total_pages_str[10], total_count_str[10];
    sprintf(prev_page_str, "%d", page > 1 ? page - 1 : 1);
    sprintf(next_page_str, "%d", page < total_pages ? page + 1 : total_pages);
    sprintf(curr_page_str, "%d", page);
    sprintf(total_pages_str, "%d", total_pages);
    sprintf(total_count_str, "%d", total_active_rooms); 
    
    const char *prev_disabled = (page == 1) ? "disabled" : "";
    const char *next_disabled = (page == total_pages) ? "disabled" : "";

    char *buf1 = (char*)malloc(st.st_size + 30000);
    char *buf2 = (char*)malloc(st.st_size + 30000);

    replace_string(html_tmpl, "{{USERNAME}}", username, buf1);
    replace_string(buf1, "{{STREAM_ROWS}}", stream_rows_html, buf2);
    replace_string(buf2, "{{TOTAL_COUNT}}", total_count_str, buf1); 
    replace_string(buf1, "{{PREV_PAGE}}", prev_page_str, buf2);
    replace_string(buf2, "{{PREV_DISABLED}}", prev_disabled, buf1);
    replace_string(buf1, "{{CURRENT_PAGE}}", curr_page_str, buf2);
    replace_string(buf2, "{{TOTAL_PAGES}}", total_pages_str, buf1);
    replace_string(buf1, "{{NEXT_PAGE}}", next_page_str, buf2);
    replace_string(buf2, "{{NEXT_DISABLED}}", next_disabled, buf1);

    long final_size = strlen(buf1);
    char header[256];
    sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n", final_size);
    
    write(clnt_sock, header, strlen(header));
    write(clnt_sock, buf1, final_size);

    free(html_tmpl);
    free(buf1);
    free(buf2);
}

void* handle_client(void* arg) {
    int clnt_sock = *((int*)arg);
    free(arg); 
    pthread_detach(pthread_self());

    // 스트리밍 데이터 수신을 위해 힙 영역 공간을 넓게 동적 할당
    char* buffer = (char*)malloc(STREAM_SIZE + 4096);
    if (buffer == NULL) {
        close(clnt_sock);
        return NULL;
    }
    
    memset(buffer, 0, STREAM_SIZE + 4096);
    int read_len = read(clnt_sock, buffer, (STREAM_SIZE + 4096) - 1);
    if (read_len <= 0) {
        free(buffer);
        close(clnt_sock);
        return NULL;
    }

    char method[10] = {0};
    char path[512] = {0}; // 쿼리스트링 파싱 유연성을 위해 늘림
    sscanf(buffer, "%s %s", method, path);

    char session_username[100] = "Guest";
    char *cookie_ptr = strstr(buffer, "Cookie: user=");
    if (cookie_ptr != NULL) {
        sscanf(cookie_ptr, "Cookie: user=%[^;\r\n]", session_username);
    }
    int is_logged_in = (strstr(buffer, "Cookie: user=") != NULL);

    // --- 1. GET 라우팅 분기 ---
    if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/", 1) == 0 && (strstr(path, "/main") || strcmp(path, "/") == 0)) {
            if (is_logged_in) {
                int req_page = 1;
                char *page_param = strstr(path, "page=");
                if (page_param != NULL) {
                    sscanf(page_param, "page=%d", &req_page);
                }
                send_main_page(clnt_sock, session_username, req_page);
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
        else if (strcmp(path, "/broadcast") == 0) {
            if (is_logged_in) {
                send_file(clnt_sock, "./html/broadcast.html");
            } else {
                char redirect_login[] = "HTTP/1.1 302 Found\r\nLocation: /login\r\n\r\n";
                write(clnt_sock, redirect_login, strlen(redirect_login));
            }
        }
        // [기능 추가 ⭐] 시청 템플릿 서빙 라우팅
        else if (strncmp(path, "/watch", 6) == 0) {
            if (is_logged_in) {
                send_file(clnt_sock, "./html/watch.html");
            } else {
                char redirect_login[] = "HTTP/1.1 302 Found\r\nLocation: /login\r\n\r\n";
                write(clnt_sock, redirect_login, strlen(redirect_login));
            }
        }
        // [기능 추가 ⭐] 특정 스트리머 방의 프레임 버퍼 데이터를 꺼내 시청자 브라우저로 응답하는 API
        else if (strncmp(path, "/live", 5) == 0) {
            char target_room[100] = {0};
            char *query = strstr(path, "room=");
            if (query != NULL) sscanf(query, "room=%[^&\r\n ]", target_room);

            char* response = (char*)malloc(STREAM_SIZE + 1024);
            memset(response, 0, STREAM_SIZE + 1024);

            pthread_mutex_lock(&room_mutex);
            int found = 0;
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                    sprintf(response, 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %ld\r\n\r\n%s", strlen(rooms[i].stream_buffer), rooms[i].stream_buffer);
                    found = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&room_mutex);

            if (!found) {
                sprintf(response, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            }
            write(clnt_sock, response, strlen(response));
            free(response);
        }
        else if (strcmp(path, "/logout") == 0) {
            char logout_resp[] = "HTTP/1.1 302 Found\r\nSet-Cookie: user=; Max-Age=0; Path=/\r\nLocation: /login\r\n\r\n";
            write(clnt_sock, logout_resp, strlen(logout_resp));
        }
        else {
            char error_resp[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>Page Not Found</h1>";
            write(clnt_sock, error_resp, strlen(error_resp));
        }
    } 
    // --- 2. POST 라우팅 분기 ---
    else if (strcmp(method, "POST") == 0) {
        char *body = strstr(buffer, "\r\n\r\n");
        body = (body != NULL) ? body + 4 : "";

        if (strncmp(path, "/stream", 7) == 0) {
            char target_room[100] = {0};
            char *query = strstr(path, "room=");
            if (query != NULL) sscanf(query, "room=%[^&\r\n ]", target_room);

            pthread_mutex_lock(&room_mutex);
            int target_idx = -1;
            
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                    target_idx = i;
                    break;
                }
            }
            if (target_idx == -1) {
                for (int i = 0; i < MAX_ROOMS; i++) {
                    if (!rooms[i].is_active) {
                        strcpy(rooms[i].room_id, target_room);
                        rooms[i].is_active = 1;
                        rooms[i].viewer_count = 0;
                        target_idx = i;
                        printf("[Server] 신규 라이브 방 개설 감지: %s\n", target_room);
                        break;
                    }
                }
            }
            
            // 전역 룸 구조체 메모리의 독립 버퍼에 안정적으로 프레임 카피 복사
            if (target_idx != -1) {
                rooms[target_idx].last_heartbeat = time(NULL);
                memset(rooms[target_idx].stream_buffer, 0, STREAM_SIZE);
                strncpy(rooms[target_idx].stream_buffer, body, STREAM_SIZE - 1);
            }
            pthread_mutex_unlock(&room_mutex);

            char ok_resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
            write(clnt_sock, ok_resp, strlen(ok_resp));
        }
        // [기능 추가 ⭐] 시청자 입장/퇴장 감지하여 실시간 카운트 가산/차감 처리 API
        else if (strncmp(path, "/viewer_heartbeat", 17) == 0) {
            char target_room[100] = {0};
            char action[20] = {0};
            
            char *query_room = strstr(path, "room=");
            if (query_room != NULL) sscanf(query_room, "room=%[^& ]", target_room);
            char *query_act = strstr(path, "action=");
            if (query_act != NULL) sscanf(query_act, "action=%[^&\r\n ]", action);

            pthread_mutex_lock(&room_mutex);
            for(int i = 0; i < MAX_ROOMS; i++) {
                if(rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                    if(strcmp(action, "enter") == 0) {
                        rooms[i].viewer_count++;
                        printf("[System] 채널 [%s]에 새 시청자 입장. (총 %d명)\n", target_room, rooms[i].viewer_count);
                    }
                    else if(strcmp(action, "leave") == 0 && rooms[i].viewer_count > 0) {
                        rooms[i].viewer_count--;
                        printf("[System] 채널 [%s]에서 시청자 퇴장. (총 %d명)\n", target_room, rooms[i].viewer_count);
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&room_mutex);

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

    printf("Full Functional Streaming Server started on port %d...\n", PORT);

    while (1) {
        clnt_addr_size = sizeof(clnt_addr);
        int* clnt_sock_ptr = (int*)malloc(sizeof(int)); 
        *clnt_sock_ptr = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        
        if (*clnt_sock_ptr == -1) {
            free(clnt_sock_ptr);
            continue;
        }

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