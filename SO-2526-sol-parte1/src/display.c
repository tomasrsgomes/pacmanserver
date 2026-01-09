#include "display.h"
#include "board.h"
#include <stdio.h>

int terminal_init() {
    return 0;
}

void terminal_cleanup() {
}

void draw_board(board_t* board, int mode) {
    (void)board;
    (void)mode;
}

char get_input() {
    return 0;
}

void refresh_screen() {
}
