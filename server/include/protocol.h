#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_PIPE_PATH_LENGTH 40

enum {
  OP_CODE_CONNECT = 1,
  OP_CODE_DISCONNECT = 2,
  OP_CODE_PLAY = 3,
  OP_CODE_BOARD = 4,
};

typedef struct {
    int op_code;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} msg_connect_t;

typedef struct {
    int op_code;
} msg_disconnect_t;

typedef struct {
    int op_code;
    char command;
} msg_play_t;

typedef struct {
    int op_code;
    int width;
    int height;
    int tempo;
    int victory;
    int game_over;
    int accumulated_points;
} msg_board_header_t;

#endif