#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

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
extern pthread_mutex_t room_mutex;

void send_file(int clnt_sock, const char* file_path);

// 내부 헬퍼 함수들
static void url_decode(const char *src, char *dst) {
    int i, len = strlen(src), j = 0;
    for (i = 0; i < len; i++) {
        if (src[i] == '%') {
            if (i + 2 < len) {
                char hex[3] = { src[i+1], src[i+2], '\0' };
                dst[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            }
        } else if (src[i] == '+') dst[j++] = ' ';
        else dst[j++] = src[i];
    }
    dst[j] = '\0';
}

static void replace_string(char *src, const char *target, const char *replacement, char *dest) {
    char *p = strstr(src, target);
    if (!p) { strcpy(dest, src); return; }
    strncpy(dest, src, p - src);
    dest[p - src] = '\0';
    sprintf(dest + (p - src), "%s%s", replacement, p + strlen(target));
}

// 로비 페이지 생성 함수
void send_main_page(int clnt_sock, const char* username, int page) {
    int fd = open("./html/main.html", O_RDONLY);
    if (fd == -1) {
        char error_resp[] = "HTTP/1.1 404 Not Found\r\n\r\n<h1>main.html Not Found</h1>";
        write(clnt_sock, error_resp, strlen(error_resp));
        return;
    }
    struct stat st; fstat(fd, &st);
    char *html_tmpl = (char*)malloc(st.st_size + 1);
    read(fd, html_tmpl, st.st_size); html_tmpl[st.st_size] = '\0'; close(fd);

    int items_per_page = 5, total_active_rooms = 0;
    time_t current_time = time(NULL);
    
    pthread_mutex_lock(&room_mutex);
    for(int i = 0; i < MAX_ROOMS; i++) {
        if(rooms[i].is_active) {
            if(current_time - rooms[i].last_heartbeat > 5) {
                rooms[i].is_active = 0; rooms[i].viewer_count = 0; rooms[i].chat_count = 0;
                memset(rooms[i].stream_buffer, 0, STREAM_SIZE);
            } else total_active_rooms++;
        }
    }

    int total_pages = (total_active_rooms + items_per_page - 1) / items_per_page;
    if (total_pages == 0) total_pages = 1;
    if (page < 1) page = 1; if (page > total_pages) page = total_pages;

    char stream_rows_html[16384] = {0};
    int current_item_count = 0, start_idx = (page - 1) * items_per_page, end_idx = start_idx + items_per_page, printed_count = 0;

    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].is_active) {
            if (current_item_count >= start_idx && current_item_count < end_idx) {
                char row_buffer[1024];
                sprintf(row_buffer, 
                    "<div class=\"stream-row\"><div class=\"stream-title\">🎬 %s 님의 라이브 채널</div>"
                    "<div>%s</div><div class=\"viewer-count\">%d명 시청 중</div>"
                    "<div><a href=\"/watch?room=%s\" target=\"_blank\" class=\"watch-btn\">시청하기</a></div></div>",
                    rooms[i].room_id, rooms[i].room_id, rooms[i].viewer_count, rooms[i].room_id);
                strcat(stream_rows_html, row_buffer); printed_count++;
            }
            current_item_count++;
        }
    }
    pthread_mutex_unlock(&room_mutex);

    if (printed_count == 0) strcpy(stream_rows_html, "<div class=\"empty-row\">현재 진행 중인 라이브 방송이 없습니다.</div>");

    char prev_page_str[10], next_page_str[10], curr_page_str[10], total_pages_str[10], total_count_str[10];
    sprintf(prev_page_str, "%d", page > 1 ? page - 1 : 1); sprintf(next_page_str, "%d", page < total_pages ? page + 1 : total_pages);
    sprintf(curr_page_str, "%d", page); sprintf(total_pages_str, "%d", total_pages); sprintf(total_count_str, "%d", total_active_rooms);
    const char *prev_disabled = (page == 1) ? "disabled" : ""; const char *next_disabled = (page == total_pages) ? "disabled" : "";

    char *buf1 = (char*)malloc(st.st_size + 30000); char *buf2 = (char*)malloc(st.st_size + 30000);
    replace_string(html_tmpl, "{{USERNAME}}", username, buf1); replace_string(buf1, "{{STREAM_ROWS}}", stream_rows_html, buf2);
    replace_string(buf2, "{{TOTAL_COUNT}}", total_count_str, buf1); replace_string(buf1, "{{PREV_PAGE}}", prev_page_str, buf2);
    replace_string(buf2, "{{PREV_DISABLED}}", prev_disabled, buf1); replace_string(buf1, "{{CURRENT_PAGE}}", curr_page_str, buf2);
    replace_string(buf2, "{{TOTAL_PAGES}}", total_pages_str, buf1); replace_string(buf1, "{{NEXT_PAGE}}", next_page_str, buf2);
    replace_string(buf2, "{{NEXT_DISABLED}}", next_disabled, buf1);

    long final_size = strlen(buf1); char header[256];
    sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n", final_size);
    write(clnt_sock, header, strlen(header)); write(clnt_sock, buf1, final_size);
    free(html_tmpl); free(buf1); free(buf2);
}

// 스트리밍 비즈니스 로직 라우터
int handle_stream(int clnt_sock, const char* method, const char* path, const char* body, const char* session_username, long total_read, char* buffer) {
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/broadcast") == 0) { send_file(clnt_sock, "./html/broadcast.html"); return 1; }
        else if (strncmp(path, "/watch", 6) == 0) {
            int fd = open("./html/watch.html", O_RDONLY);
            if (fd != -1) {
                struct stat st; fstat(fd, &st);
                char *tmpl = (char*)malloc(st.st_size + 1); read(fd, tmpl, st.st_size); tmpl[st.st_size] = '\0'; close(fd);
                char *final_watch = (char*)malloc(st.st_size + 2000);
                replace_string(tmpl, "{{USERNAME}}", session_username, final_watch);
                char header[256]; sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n", strlen(final_watch));
                write(clnt_sock, header, strlen(header)); write(clnt_sock, final_watch, strlen(final_watch));
                free(tmpl); free(final_watch);
            }
            return 1;
        }
        else if (strncmp(path, "/chat", 5) == 0) {
            char target_room[100] = {0}; char *query = strstr(path, "room=");
            if (query != NULL) sscanf(query, "room=%99[^&\r\n ]", target_room);
            char *json_resp = (char*)malloc(32768); strcpy(json_resp, "[");
            pthread_mutex_lock(&room_mutex);
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                    for (int j = 0; j < rooms[i].chat_count; j++) {
                        char chat_element[1024];
                        sprintf(chat_element, "{\"user\":\"%s\",\"message\":\"%s\"}", rooms[i].chat_list[j].user, rooms[i].chat_list[j].message);
                        strcat(json_resp, chat_element); if (j < rooms[i].chat_count - 1) strcat(json_resp, ",");
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&room_mutex);
            strcat(json_resp, "]");
            char header[256]; sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nContent-Length: %ld\r\n\r\n", strlen(json_resp));
            write(clnt_sock, header, strlen(header)); write(clnt_sock, json_resp, strlen(json_resp)); free(json_resp);
            return 1;
        }
        else if (strncmp(path, "/live", 5) == 0) {
            char target_room[100] = {0}; char *query = strstr(path, "room=");
            if (query != NULL) sscanf(query, "room=%99[^&\r\n ]", target_room);
            char header[512];
            char *stream_copy = NULL;
            long stream_len = 0;
            pthread_mutex_lock(&room_mutex);
            int found = 0;
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                    stream_len = strlen(rooms[i].stream_buffer);
                    stream_copy = (char*)malloc(stream_len + 1);
                    if (stream_copy != NULL) {
                        memcpy(stream_copy, rooms[i].stream_buffer, stream_len + 1);
                    }
                    found = 1; break;
                }
            }
            pthread_mutex_unlock(&room_mutex);
            if (found && stream_copy != NULL) {
                sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n\r\n", stream_len);
                write(clnt_sock, header, strlen(header));
                if (stream_len > 0) write(clnt_sock, stream_copy, stream_len);
                free(stream_copy);
            } else if (found) {
                char error_resp[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
                write(clnt_sock, error_resp, strlen(error_resp));
            } else {
                char not_found[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                write(clnt_sock, not_found, strlen(not_found));
            }
            return 1;
        }
    } 
    else if (strcmp(method, "POST") == 0) {
        if (strncmp(path, "/chat", 5) == 0) {
            char target_room[100] = {0}; char *query = strstr(path, "room=");
            if (query != NULL) sscanf(query, "room=%99[^&\r\n ]", target_room);
            char raw_user[100] = {0}, raw_msg[512] = {0}, decoded_user[100] = {0}, decoded_msg[512] = {0};
            if (sscanf(body, "user=%99[^&]&message=%511s", raw_user, raw_msg) == 2) {
                url_decode(raw_user, decoded_user); url_decode(raw_msg, decoded_msg);
                pthread_mutex_lock(&room_mutex);
                for (int i = 0; i < MAX_ROOMS; i++) {
                    if (rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                        if (rooms[i].chat_count >= MAX_CHATS) {
                            for (int k = 1; k < MAX_CHATS; k++) rooms[i].chat_list[k - 1] = rooms[i].chat_list[k];
                            rooms[i].chat_count = MAX_CHATS - 1;
                        }
                        int idx = rooms[i].chat_count;
                        strncpy(rooms[i].chat_list[idx].user, decoded_user, 49); strncpy(rooms[i].chat_list[idx].message, decoded_msg, 254);
                        rooms[i].chat_count++; break;
                    }
                }
                pthread_mutex_unlock(&room_mutex);
            }
            char ok_resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"; write(clnt_sock, ok_resp, strlen(ok_resp));
            return 1;
        }
        else if (strncmp(path, "/stream", 7) == 0) {
            char target_room[100] = {0}; char *query = strstr(path, "room=");
            if (query != NULL) sscanf(query, "room=%99[^&\r\n ]", target_room);
            pthread_mutex_lock(&room_mutex);
            int target_idx = -1;
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) { target_idx = i; break; }
            }
            if (target_idx == -1) {
                for (int i = 0; i < MAX_ROOMS; i++) {
                    if (!rooms[i].is_active) {
                        strcpy(rooms[i].room_id, target_room); rooms[i].is_active = 1; rooms[i].viewer_count = 0; rooms[i].chat_count = 0;
                        target_idx = i; printf("[Server] 실시간 라이브 채널 개설: %s\n", target_room); break;
                    }
                }
            }
            if (target_idx != -1) {
                rooms[target_idx].last_heartbeat = time(NULL);
                memset(rooms[target_idx].stream_buffer, 0, STREAM_SIZE);
                strncpy(rooms[target_idx].stream_buffer, body, STREAM_SIZE - 1);
            }
            pthread_mutex_unlock(&room_mutex);
            char ok_resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"; write(clnt_sock, ok_resp, strlen(ok_resp));
            return 1;
        }
        else if (strncmp(path, "/viewer_heartbeat", 17) == 0) {
            char target_room[100] = {0}, action[20] = {0};
            char *query_room = strstr(path, "room="); if (query_room != NULL) sscanf(query_room, "room=%99[^& ]", target_room);
            char *query_act = strstr(path, "action="); if (query_act != NULL) sscanf(query_act, "action=%19[^&\r\n ]", action);
            pthread_mutex_lock(&room_mutex);
            for(int i = 0; i < MAX_ROOMS; i++) {
                if(rooms[i].is_active && strcmp(rooms[i].room_id, target_room) == 0) {
                    if(strcmp(action, "enter") == 0) rooms[i].viewer_count++;
                    else if(strcmp(action, "leave") == 0 && rooms[i].viewer_count > 0) rooms[i].viewer_count--;
                    break;
                }
            }
            pthread_mutex_unlock(&room_mutex);
            char ok_resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"; write(clnt_sock, ok_resp, strlen(ok_resp));
            return 1;
        }
    }
    return 0;
}
