#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "dbms.h"

#define DEFAULT_PORT 8888
#define BUFFER_SIZE 16384

static volatile bool g_running = true;
static int g_server_fd = -1;
static char g_db_path[512] = "server.db";

static void handle_sigint(int sig) {
  (void)sig;
  g_running = false;
  if (g_server_fd != -1) {
    close(g_server_fd);
    g_server_fd = -1;
  }
}

typedef struct {
  int client_fd;
  struct sockaddr_in client_addr;
} ClientContext;

#include <sys/time.h>
#include <ctype.h>

static void send_http_response(int fd, int status_code, const char* status_text, const char* body) {
  char header[512];
  int body_len = (int)strlen(body);
  int hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "\r\n",
                      status_code, status_text, body_len);
  write(fd, header, hlen);
  if (body_len > 0) {
    write(fd, body, body_len);
  }
}

static void escape_json_string(const char* src, char* dst, size_t dst_max) {
  size_t d = 0;
  for (size_t i = 0; src[i] && d + 4 < dst_max; i++) {
    char c = src[i];
    if (c == '"') { dst[d++] = '\\'; dst[d++] = '"'; }
    else if (c == '\\') { dst[d++] = '\\'; dst[d++] = '\\'; }
    else if (c == '\n') { dst[d++] = '\\'; dst[d++] = 'n'; }
    else if (c == '\r') { dst[d++] = '\\'; dst[d++] = 'r'; }
    else if (c == '\t') { dst[d++] = '\\'; dst[d++] = 't'; }
    else dst[d++] = c;
  }
  dst[d] = '\0';
}

static void handle_http_request(int fd, dbms* db, const char* initial_data, ssize_t initial_bytes) {
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

  char req_buf[BUFFER_SIZE];
  size_t total_read = 0;
  if (initial_bytes > 0) {
    size_t copy_len = (initial_bytes < (ssize_t)(sizeof(req_buf) - 1)) ? (size_t)initial_bytes : (sizeof(req_buf) - 1);
    memcpy(req_buf, initial_data, copy_len);
    total_read = copy_len;
  }
  req_buf[total_read] = '\0';

  char* body_start = strstr(req_buf, "\r\n\r\n");
  if (!body_start) body_start = strstr(req_buf, "\n\n");

  /* Parse Content-Length if present */
  int content_length = 0;
  char* cl_hdr = strcasestr(req_buf, "Content-Length:");
  if (cl_hdr) {
    content_length = atoi(cl_hdr + 15);
  }

  /* Read rest of headers if needed */
  while (!body_start && total_read < sizeof(req_buf) - 1) {
    ssize_t n = read(fd, req_buf + total_read, sizeof(req_buf) - 1 - total_read);
    if (n <= 0) break;
    total_read += n;
    req_buf[total_read] = '\0';
    body_start = strstr(req_buf, "\r\n\r\n");
    if (!body_start) body_start = strstr(req_buf, "\n\n");
  }

  char* actual_body = "";
  if (body_start) {
    if (strncmp(body_start, "\r\n\r\n", 4) == 0) actual_body = body_start + 4;
    else actual_body = body_start + 2;

    size_t current_body_len = total_read - (actual_body - req_buf);
    while ((int)current_body_len < content_length && total_read < sizeof(req_buf) - 1) {
      ssize_t n = read(fd, req_buf + total_read, sizeof(req_buf) - 1 - total_read);
      if (n <= 0) break;
      total_read += n;
      req_buf[total_read] = '\0';
      current_body_len = total_read - (actual_body - req_buf);
    }
  }

  char method[16] = {0};
  char path[256] = {0};
  sscanf(req_buf, "%15s %255s", method, path);

  if (strcasecmp(method, "GET") == 0) {
    if (strcmp(path, "/health") == 0 || strcmp(path, "/api/health") == 0) {
      send_http_response(fd, 200, "OK", "{\"status\": \"ok\"}");
      return;
    }
    send_http_response(fd, 404, "Not Found", "{\"error\": \"Endpoint not found\"}");
    return;
  }

  if (strcasecmp(method, "POST") == 0) {
    if (strcmp(path, "/query") == 0 || strcmp(path, "/api/query") == 0 || strcmp(path, "/sql") == 0) {
      char sql[BUFFER_SIZE] = {0};
      char* sql_key = strstr(actual_body, "\"sql\"");
      if (sql_key) {
        char* val_start = strchr(sql_key + 5, ':');
        if (val_start) {
          val_start = strchr(val_start, '"');
          if (val_start) {
            val_start++;
            char* val_end = strchr(val_start, '"');
            if (val_end) {
              size_t slen = val_end - val_start;
              if (slen < sizeof(sql) - 1) {
                memcpy(sql, val_start, slen);
                sql[slen] = '\0';
              }
            }
          }
        }
      } else {
        const char* q = actual_body;
        while (*q && isspace((unsigned char)*q)) q++;
        strncpy(sql, q, sizeof(sql) - 1);
        size_t l = strlen(sql);
        while (l > 0 && isspace((unsigned char)sql[l - 1])) sql[--l] = '\0';
      }

      if (strlen(sql) == 0) {
        send_http_response(fd, 400, "Bad Request", "{\"error\": \"No SQL query provided\"}");
        return;
      }

      dbms_stmt* stmt = NULL;
      int prep = dbms_prepare_v2(db, sql, (int)strlen(sql), &stmt, NULL);
      if (prep != DBMS_OK) {
        send_http_response(fd, 400, "Bad Request", "{\"error\": \"SQL syntax or semantic error\"}");
        return;
      }

      int col_count = dbms_column_count(stmt);
      char* resp = malloc(BUFFER_SIZE * 2);
      if (!resp) {
        dbms_finalize(stmt);
        send_http_response(fd, 500, "Internal Server Error", "{\"error\": \"Out of memory\"}");
        return;
      }

      size_t roff = 0;
      roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "{\"columns\": [");
      for (int c = 0; c < col_count; c++) {
        char esc_col[128];
        escape_json_string(dbms_column_name(stmt, c), esc_col, sizeof(esc_col));
        roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "%s\"%s\"", (c > 0 ? ", " : ""), esc_col);
      }
      roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "], \"rows\": [");

      int row_idx = 0;
      int step_res;
      while ((step_res = dbms_step(stmt)) == DBMS_ROW && roff + 1024 < BUFFER_SIZE * 2) {
        if (row_idx > 0) roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, ", ");
        roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "[");
        for (int c = 0; c < col_count; c++) {
          const char* txt = dbms_column_text(stmt, c);
          if (c > 0) roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, ", ");
          if (!txt || strcmp(txt, "NULL") == 0) {
            roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "null");
          } else {
            char esc_val[512];
            escape_json_string(txt, esc_val, sizeof(esc_val));
            roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "\"%s\"", esc_val);
          }
        }
        roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "]");
        row_idx++;
      }
      dbms_finalize(stmt);
      roff += snprintf(resp + roff, (BUFFER_SIZE * 2) - roff, "]}");

      send_http_response(fd, 200, "OK", resp);
      free(resp);
      return;
    }
    send_http_response(fd, 404, "Not Found", "{\"error\": \"Endpoint not found\"}");
    return;
  }

  send_http_response(fd, 405, "Method Not Allowed", "{\"error\": \"Method not allowed\"}");
}

static void* client_worker(void* arg) {
  ClientContext* ctx = (ClientContext*)arg;
  int fd = ctx->client_fd;

  dbms* db = NULL;
  if (dbms_open(g_db_path, &db) != DBMS_OK) {
    const char* err_msg = "Error: Failed to open database.\n";
    write(fd, err_msg, strlen(err_msg));
    close(fd);
    free(ctx);
    return NULL;
  }

  char recv_buf[BUFFER_SIZE];
  char sql_buf[BUFFER_SIZE];
  uint32_t sql_len = 0;

  /* Check if client immediately sent HTTP request (sniff with MSG_PEEK) */
  char peek_buf[16];
  ssize_t peeked = recv(fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK | MSG_DONTWAIT);
  if (peeked > 0) {
    peek_buf[peeked] = '\0';
    if (strncmp(peek_buf, "GET ", 4) == 0 ||
        strncmp(peek_buf, "POST ", 5) == 0 ||
        strncmp(peek_buf, "OPTIONS ", 8) == 0 ||
        strncmp(peek_buf, "HEAD ", 5) == 0) {
      handle_http_request(fd, db, NULL, 0);
      dbms_close(db);
      close(fd);
      free(ctx);
      return NULL;
    }
  }

  const char* welcome = "DBMS Network Server 1.0 (Type SQL commands or .exit)\n";
  write(fd, welcome, strlen(welcome));

  while (g_running) {
    ssize_t bytes = read(fd, recv_buf, sizeof(recv_buf) - 1);
    if (bytes <= 0) break;
    recv_buf[bytes] = '\0';

    for (ssize_t i = 0; i < bytes; i++) {
      char c = recv_buf[i];
      if (c == '\r') continue;

      if (sql_len < sizeof(sql_buf) - 1) {
        sql_buf[sql_len++] = c;
      }

      if (c == ';' || c == '\n') {
        sql_buf[sql_len] = '\0';
        char* query = sql_buf;
        while (*query == ' ' || *query == '\t' || *query == '\n') query++;

        if (strlen(query) > 0) {
          if (strncasecmp(query, ".exit", 5) == 0 || strncasecmp(query, "exit", 4) == 0 || strncasecmp(query, "quit", 4) == 0) {
            dbms_close(db);
            close(fd);
            free(ctx);
            return NULL;
          }
          if (strncasecmp(query, "ping", 4) == 0) {
            write(fd, "PONG\n", 5);
            sql_len = 0;
            continue;
          }

          dbms_stmt* stmt = NULL;
          int prep_res = dbms_prepare_v2(db, query, (int)strlen(query), &stmt, NULL);
          if (prep_res != DBMS_OK) {
            const char* err = "Error: SQL syntax or semantic error.\n";
            write(fd, err, strlen(err));
          } else {
            int col_count = dbms_column_count(stmt);
            int step_res;
            while ((step_res = dbms_step(stmt)) == DBMS_ROW) {
              char row_out[BUFFER_SIZE];
              int r_len = 0;
              row_out[r_len++] = '(';
              for (int col = 0; col < col_count; col++) {
                if (col > 0) {
                  row_out[r_len++] = ',';
                  row_out[r_len++] = ' ';
                }
                const char* val = dbms_column_text(stmt, col);
                int v_len = (int)strlen(val);
                if (r_len + v_len + 16 < BUFFER_SIZE) {
                  memcpy(row_out + r_len, val, v_len);
                  r_len += v_len;
                }
              }
              row_out[r_len++] = ')';
              row_out[r_len++] = '\n';
              write(fd, row_out, r_len);
            }
            dbms_finalize(stmt);
            write(fd, "Executed.\n", 10);
          }
        }
        sql_len = 0;
      }
    }
  }

  dbms_close(db);
  close(fd);
  free(ctx);
  return NULL;
}

int main(int argc, char* argv[]) {
  int port = DEFAULT_PORT;
  if (argc > 1) {
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = DEFAULT_PORT;
  }
  if (argc > 2) {
    strncpy(g_db_path, argv[2], sizeof(g_db_path) - 1);
  }

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);
  signal(SIGPIPE, SIG_IGN);

  g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_server_fd < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  if (bind(g_server_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("bind");
    close(g_server_fd);
    return 1;
  }

  if (listen(g_server_fd, 128) < 0) {
    perror("listen");
    close(g_server_fd);
    return 1;
  }

  printf("DBMS Server running on port %d (Database: %s)...\n", port, g_db_path);

  while (g_running) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(g_server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
      if (!g_running) break;
      continue;
    }

    ClientContext* ctx = malloc(sizeof(ClientContext));
    ctx->client_fd = client_fd;
    ctx->client_addr = client_addr;

    pthread_t th;
    if (pthread_create(&th, NULL, client_worker, ctx) == 0) {
      pthread_detach(th);
    } else {
      close(client_fd);
      free(ctx);
    }
  }

  printf("DBMS Server stopped.\n");
  return 0;
}
