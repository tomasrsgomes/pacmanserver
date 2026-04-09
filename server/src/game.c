#include "board.h"
#include "display.h" 
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <semaphore.h>
#include <errno.h>
#include <signal.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2

// Global settings
char LEVELS_DIR[256];
int MAX_GAMES;
char REGISTER_FIFO[256];

sem_t available_slots;

// Global sessions registry for Signal Handler
struct {
    void **sessions; // Array of pointers to game_session_t
    pthread_mutex_t lock;
} registry;

typedef struct {
    int id;
    int req_fd;
    int notif_fd;
    char last_command;
    int client_connected;
    board_t board;
    pthread_mutex_t cmd_mutex;
    pthread_mutex_t session_mutex;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
} game_session_t;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    game_session_t *session;
    int ghost_index;
    int *shutdown_flag;
} thread_arg_t;

void send_board(game_session_t* session, board_t* board) {
    if (!session->client_connected) return;

    msg_board_header_t head;
    head.op_code = OP_CODE_BOARD;
    head.width = board->width;
    head.height = board->height;
    head.tempo = board->tempo;
    head.accumulated_points = board->pacmans[0].points;
    head.victory = 0;
    head.game_over = 0; 

    ssize_t w = write(session->notif_fd, &head, sizeof(head));
    if (w < 0) {
        session->client_connected = 0;
        return;
    }

    int size = board->width * board->height;
    char *data = malloc(size);
    if (!data) return;

    for(int y=0; y<board->height; y++) {
        for(int x=0; x<board->width; x++) {
             board_pos_t *pos = &board->board[y*board->width + x];
             char c = ' ';
             if (pos->content == 'W') c = '#';
             else if (pos->has_portal) c = '@'; 
             else if (pos->has_dot) c = '.';
             data[y*board->width + x] = c;
        }
    }
    
    for(int i=0; i<board->n_pacmans; i++) {
        if(board->pacmans[i].alive) {
            int idx = board->pacmans[i].pos_y * board->width + board->pacmans[i].pos_x;
            if(idx < size) data[idx] = 'C';
        }
    }
    for(int i=0; i<board->n_ghosts; i++) {
        int idx = board->ghosts[i].pos_y * board->width + board->ghosts[i].pos_x;
        if(idx < size) data[idx] = 'M';
    }

    write(session->notif_fd, data, size);
    free(data);
}


void* notif_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t*) arg;
    game_session_t *session = targ->session;
    board_t *board = &session->board;
    int *shutdown = targ->shutdown_flag;

    while (true) {
        sleep_ms(board->tempo);
        
        pthread_rwlock_rdlock(&board->state_lock);
        if (*shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }
        
        send_board(session, board);
        
        pthread_rwlock_unlock(&board->state_lock);

        if (!session->client_connected) break;
    }
    return NULL;
}

void* input_thread(void *arg) {
    game_session_t *session = (game_session_t*) arg;
    
    while(session->client_connected) {
        msg_play_t msg; 
        ssize_t r = read(session->req_fd, &msg, sizeof(msg));
        if (r <= 0) {
            session->client_connected = 0;
            break;
        }
        
        if (msg.op_code == OP_CODE_PLAY) {
            pthread_mutex_lock(&session->cmd_mutex);
            session->last_command = msg.command;
            pthread_mutex_unlock(&session->cmd_mutex);
        } else if (msg.op_code == OP_CODE_DISCONNECT) {
            session->client_connected = 0;
            break;
        }
    }
    return NULL;
}

void* pacman_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t*) arg;
    game_session_t *session = targ->session;
    board_t *board = &session->board;
    int *retval = malloc(sizeof(int));
    *retval = QUIT_GAME; 

    pacman_t* pacman = &board->pacmans[0];

    while (session->client_connected) {
        if(!pacman->alive) {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        if (pacman->n_moves == 0) { // Interactive mode
            pthread_mutex_lock(&session->cmd_mutex);
            c.command = session->last_command;
            session->last_command = 0; // Consume
            pthread_mutex_unlock(&session->cmd_mutex);

            if(c.command == 0) {
                continue;
            }

            c.turns = 1;
            play = &c;
        }
        else {
            play = &pacman->moves[pacman->current_move%pacman->n_moves];
        }

        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        pthread_rwlock_wrlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            *retval = NEXT_LEVEL;
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        if(result == DEAD_PACMAN) {
            *retval = QUIT_GAME;
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    return (void*) retval;
}

void* server_ghost_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t*) arg;
    board_t *board = &targ->session->board;
    int ghost_ind = targ->ghost_index;
    int *shutdown = targ->shutdown_flag;

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (*shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            return NULL;
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

void run_game_session(game_session_t *session) {
    DIR* level_dir = opendir(LEVELS_DIR);
    if (!level_dir) return;

    int accumulated_points = 0;
    bool end_game = false;
    struct dirent* entry;

    pthread_t input_tid;
    pthread_create(&input_tid, NULL, input_thread, session);
    
    while ((entry = readdir(level_dir)) != NULL && !end_game && session->client_connected) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;

        load_level(&session->board, entry->d_name, LEVELS_DIR, accumulated_points);
        
        while(session->client_connected) {
            pthread_t notif_tid, pacman_tid;
            pthread_t *ghost_tids = malloc(session->board.n_ghosts * sizeof(pthread_t));
            int shutdown = 0;
            
            thread_arg_t common_arg = { .session = session, .shutdown_flag = &shutdown };
            
            pthread_create(&pacman_tid, NULL, pacman_thread, &common_arg);
            
            for (int i = 0; i < session->board.n_ghosts; i++) {
                thread_arg_t *garg = malloc(sizeof(thread_arg_t));
                *garg = common_arg;
                garg->ghost_index = i;
                pthread_create(&ghost_tids[i], NULL, server_ghost_thread, garg);
            }
            pthread_create(&notif_tid, NULL, notif_thread, &common_arg);

            int *retval;
            pthread_join(pacman_tid, (void**)&retval); // Wait for Pacman logic to end level/game
            int result = *retval;
            free(retval);

            pthread_rwlock_wrlock(&session->board.state_lock);
            shutdown = 1;
            pthread_rwlock_unlock(&session->board.state_lock);

            pthread_join(notif_tid, NULL);
            for(int i=0; i<session->board.n_ghosts; i++) pthread_join(ghost_tids[i], NULL);
            free(ghost_tids);

            if(result == NEXT_LEVEL) {
                 send_board(session, &session->board); 
                 sleep_ms(session->board.tempo);
                 break; 
            }

            if(result == QUIT_GAME) {
                 end_game = true;
                 break;
            }
            
            accumulated_points = session->board.pacmans[0].points;
        }
        
        unload_level(&session->board);
    }
    
    if (session->client_connected) {
        msg_board_header_t head = {0};
        head.op_code = OP_CODE_BOARD;
        head.game_over = 1;
        write(session->notif_fd, &head, sizeof(head));
    }

    session->client_connected = 0;
    pthread_join(input_tid, NULL);
    
    closedir(level_dir);
}

void unregister_session(game_session_t *session) {
    pthread_mutex_lock(&registry.lock);
    for(int i=0; i<MAX_GAMES; i++) {
        if (registry.sessions[i] == session) {
             registry.sessions[i] = NULL;
             break;
        }
    }
    pthread_mutex_unlock(&registry.lock);
    sem_post(&available_slots);
}

void signal_handler(int signum) {
    if (signum != SIGUSR1) return;
    
    int fd = open("server_dump.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) return;

    dprintf(fd, "Server Board Dump\n==================\n");

    for (int i=0; i<MAX_GAMES; i++) {
        game_session_t *s = (game_session_t*) registry.sessions[i];
        if (s) {
            dprintf(fd, "Game ID: %d\n", s->id);
            board_t *b = &s->board;
            dprintf(fd, "Level: %s, Size: %dx%d\n", b->level_name, b->width, b->height);
            
            for (int y=0; y<b->height; y++) {
                for (int x=0; x<b->width; x++) {
                    char c = ' ';
                    if (b->board[y*b->width + x].content == 'W') c = '#';
                    else if (b->board[y*b->width + x].has_portal) c = '@';
                    else if (b->board[y*b->width + x].has_dot) c = '.';

                    for(int k=0; k<b->n_pacmans; k++) 
                        if (b->pacmans[k].alive && b->pacmans[k].pos_x == x && b->pacmans[k].pos_y == y) c = 'C';
                    for(int k=0; k<b->n_ghosts; k++) 
                        if (b->ghosts[k].pos_x == x && b->ghosts[k].pos_y == y) c = 'M';
                    write(fd, &c, 1);
                }
                write(fd, "\n", 1);
            }
            dprintf(fd, "\n");
        }
    }
    close(fd);
}

void* game_worker(void* arg) {
    game_session_t *session = (game_session_t*) arg;
    
    debug("Starting session %d, waiting for client pipes...\n", session->id);
    
    // Connect to client pipes
    session->notif_fd = open(session->notif_pipe_path, O_WRONLY);
    if(session->notif_fd == -1) {
         debug("Failed to open notif pipe %s\n", session->notif_pipe_path);
         unregister_session(session);
         free(session);
         return NULL;
    }

    session->req_fd = open(session->req_pipe_path, O_RDONLY);
    if(session->req_fd == -1) {
        debug("Failed to open req pipe %s\n", session->req_pipe_path);
        close(session->notif_fd);
        unregister_session(session);
        free(session);
        return NULL;
    }

    debug("Session %d connected.\n", session->id);

    session->client_connected = 1;
    pthread_mutex_init(&session->cmd_mutex, NULL);
    
    run_game_session(session);

    close(session->req_fd);
    close(session->notif_fd);
    pthread_mutex_destroy(&session->cmd_mutex);
    
    unregister_session(session);
    
    debug("Session %d finished\n", session->id);
    free(session);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <fifo_name>\n", argv[0]);
        return -1;
    }

    strncpy(LEVELS_DIR, argv[1], 255);
    MAX_GAMES = atoi(argv[2]);
    strncpy(REGISTER_FIFO, argv[3], 255);

    registry.sessions = calloc(MAX_GAMES, sizeof(void*));
    pthread_mutex_init(&registry.lock, NULL);
    signal(SIGUSR1, signal_handler);

    if (mkfifo(REGISTER_FIFO, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo register");
            return 1;
        }
    }
    
    sem_init(&available_slots, 0, MAX_GAMES);
    open_debug_file("server.log");
    
    printf("Server started! Listening on %s with %d slots...\n", REGISTER_FIFO, MAX_GAMES);
    debug("Server started on %s with %d slots\n", REGISTER_FIFO, MAX_GAMES);

    int server_fd = open(REGISTER_FIFO, O_RDWR); // O_RDWR blocks EOF
    if (server_fd == -1) {
        perror("open register fifo");
        return 1;
    }

    int game_id_counter = 0;

    while(1) {
        msg_connect_t msg; 
        ssize_t n = read(server_fd, &msg, sizeof(msg));
        if (n == sizeof(msg) && msg.op_code == OP_CODE_CONNECT) {
            
            debug("Received connect request\n");
            
            // Block if full, retry on interrupt (e.g. signal)
            while (sem_wait(&available_slots) == -1) {
                if (errno != EINTR) { 
                    perror("sem_wait failed");
                    break;
                }
            }
            
            pthread_mutex_lock(&registry.lock);
            int slot_idx = -1;
            for(int i=0; i<MAX_GAMES; i++) {
                if (registry.sessions[i] == NULL) {
                    slot_idx = i;
                    break;
                }
            }
            if (slot_idx == -1) {
                // Should not happen if semaphore works correctly
                pthread_mutex_unlock(&registry.lock);
                sem_post(&available_slots);
                continue;
            }

            game_session_t *session = malloc(sizeof(game_session_t));
            registry.sessions[slot_idx] = session;
            pthread_mutex_unlock(&registry.lock);

            session->id = ++game_id_counter;
            memset(session->req_pipe_path, 0, MAX_PIPE_PATH_LENGTH+1);
            memset(session->notif_pipe_path, 0, MAX_PIPE_PATH_LENGTH+1);
            strncpy(session->req_pipe_path, msg.req_pipe_path, MAX_PIPE_PATH_LENGTH);
            strncpy(session->notif_pipe_path, msg.notif_pipe_path, MAX_PIPE_PATH_LENGTH);
            
            pthread_t tid;
            pthread_create(&tid, NULL, game_worker, session);
            pthread_detach(tid); 
        }
    }

    close(server_fd);
    unlink(REGISTER_FIFO);
    return 0;
}
