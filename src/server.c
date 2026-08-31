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

  const char* welcome = "DBMS Network Server 1.0 (Type SQL commands or .exit)\n";
  write(fd, welcome, strlen(welcome));

  char recv_buf[BUFFER_SIZE];
  char sql_buf[BUFFER_SIZE];
  uint32_t sql_len = 0;

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
