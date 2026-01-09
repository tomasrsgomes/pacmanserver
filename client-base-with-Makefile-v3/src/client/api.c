#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>

struct Session {
  int id;
  int req_pipe_fd;
  int notif_pipe_fd;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1, .req_pipe_fd = -1, .notif_pipe_fd = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  if (mkfifo(session.req_pipe_path, 0666) == -1) {
    if (errno != EEXIST) {
      perror("Failed to create request pipe");
      return 1;
    }
  }

  if (mkfifo(session.notif_pipe_path, 0666) == -1) {
    if (errno != EEXIST) {
      perror("Failed to create notification pipe");
      unlink(session.req_pipe_path);
      return 1;
    }
  }

  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    perror("Failed to open server pipe");
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);
    return 1;
  }

  msg_connect_t msg;
  msg.op_code = OP_CODE_CONNECT;
  strncpy(msg.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(msg.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  if (write(server_fd, &msg, sizeof(msg)) == -1) {
    perror("Failed to send connect request");
    close(server_fd);
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);
    return 1;
  }
  close(server_fd);

  session.notif_pipe_fd = open(session.notif_pipe_path, O_RDONLY);
  if (session.notif_pipe_fd == -1) {
    perror("Failed to open notification pipe");
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);
    return 1;
  }

  session.req_pipe_fd = open(session.req_pipe_path, O_WRONLY);
  if (session.req_pipe_fd == -1) {
    perror("Failed to open request pipe");
    close(session.notif_pipe_fd);
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);
    return 1;
  }

  session.id = 1;
  return 0;
}

int pacman_disconnect() {
  if (session.id == -1) return 0;

  msg_disconnect_t msg;
  msg.op_code = OP_CODE_DISCONNECT;
  if (write(session.req_pipe_fd, &msg, sizeof(msg)) == -1) {
      perror("Failed to send disconnect");
  }

  close(session.req_pipe_fd);
  close(session.notif_pipe_fd);
  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);

  session.id = -1;
  return 0;
}

void pacman_play(char command) {
  if (session.id == -1) return;

  msg_play_t msg; 
  msg.op_code = OP_CODE_PLAY;
  msg.command = command;

  if (write(session.req_pipe_fd, &msg, sizeof(msg)) == -1) {
      perror("Failed to send play command");
  }
}

Board receive_board_update(void) {
  Board b = {0};
  if (session.id == -1) {
      b.game_over = 1;
      return b;
  }

  msg_board_header_t head;
  ssize_t bytes_read = read(session.notif_pipe_fd, &head, sizeof(head));

  if (bytes_read <= 0) {
      b.game_over = 1;
      return b;
  }

  if (head.op_code != OP_CODE_BOARD) {
      // Ignore or handle other opcodes
      // If we received something else, maybe sync error?
      return b;
  }

  b.width = head.width;
  b.height = head.height;
  b.tempo = head.tempo;
  b.victory = head.victory;
  b.game_over = head.game_over;
  b.accumulated_points = head.accumulated_points;

  int data_size = b.width * b.height;
  if (data_size > 0) {
      b.data = malloc(data_size);
      if (!b.data) {
          b.game_over = 1;
          return b;
      }
      
      int total_read = 0;
      while (total_read < data_size) {
        int r = read(session.notif_pipe_fd, b.data + total_read, data_size - total_read);
        if (r <= 0) {
            free(b.data);
            b.data = NULL;
            b.game_over = 1;
            return b;
        }
        total_read += r;
      }
  }

  return b;
}


