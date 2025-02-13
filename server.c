#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <signal.h>


#define PORT "8080"
#define BUF_SIZE 1024
#define MAX_USERS 100
#define MAX_GAMES 50

typedef struct {
    char username[50];
    char password[50];
    int logged_in;
    int uid;
    int client_socket; 
} User;

typedef struct {
    int in_use;
    int user_id;
    char session_id[32];
} Session;

typedef struct {
    int n;            // размерность поля
    char** board;     // динамический массив (n x n)
    int player1_id;
    int player2_id;
    int current_player_id;
    int game_id;
    int is_active;
} Game;

static int listen_socket = -1;

pthread_mutex_t users_lock;
pthread_mutex_t games_lock;

void init_data_structures();
void* client_thread(void* param); // поток должен возвращать void*
int register_user(const char* username, const char* password);
int login_user(const char* username, const char* password, int sock);
void logout_user(int sock);
int find_user_by_socket(int sock);
int create_game(int sock, int n);
int join_game(int sock, int game_id);
int make_move(int sock, int game_id, int uid, char symbol, int x, int y);
void switch_to_next_player(Game *g);
void broadcast_game_state(int game_id, int x, int y, char symbol);
int check_win_condition(Game* game);
void send_to_client(int sock, const char* msg);
int create_session(int user_id);
void generate_session_id(char* buf, size_t len);

User users[MAX_USERS];
int user_count = 0;
static Session sessions[MAX_USERS];
Game games[MAX_GAMES];
int game_count = 0;


void cleanup_and_exit(int signum) {
    printf("\n[INFO] Caught signal %d (Ctrl + C). Cleaning up...\n", signum);

    // 1. Закрываем слушающий сокет
    if (listen_socket >= 0) {
        close(listen_socket);
        listen_socket = -1;
        printf("[INFO] Listen socket closed.\n");
    }

    // 2. Освобождаем динамический board для всех игр
    pthread_mutex_lock(&games_lock);
    for (int i = 0; i < game_count; i++) {
        Game* g = &games[i];
        if (g->board) {
            for (int row = 0; row < g->n; row++) {
                free(g->board[row]);
                g->board[row] = NULL;
            }
            free(g->board);
            g->board = NULL;
        }
    }
    pthread_mutex_unlock(&games_lock);
    printf("[INFO] Freed all game boards.\n");

    // 3. Уничтожаем мьютексы
    pthread_mutex_destroy(&users_lock);
    pthread_mutex_destroy(&games_lock);
    printf("[INFO] Mutexes destroyed.\n");

    // 4. Завершаем программу
    printf("[INFO] Exiting now.\n");
    exit(0);
}

int main() 
{
    // Чтобы корректно убивать сервер CTRL+C, можно игнорировать SIGPIPE
    // (чтобы не было падения при записи в закрытый сокет)
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, cleanup_and_exit);

    // Инициализируем структуры
    memset(users, 0, sizeof(users));
    memset(games, 0, sizeof(games));
    memset(sessions, 0, sizeof(sessions));

    // Инициализируем мьютексы вместо InitializeCriticalSection
    pthread_mutex_init(&users_lock, NULL);
    pthread_mutex_init(&games_lock, NULL);

    init_data_structures();

    // Создаём сокет для прослушивания
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family   = AF_INET;      // IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP
    hints.ai_flags    = AI_PASSIVE;   // Для bind
    hints.ai_protocol = IPPROTO_TCP;

    int rv = getaddrinfo(NULL, PORT, &hints, &result);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(rv));
        return 1;
    }

    listen_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listen_socket < 0) {
        perror("socket failed");
        freeaddrinfo(result);
        return 1;
    }

    // Чтобы сокет не висел в TIME_WAIT
    int optval = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    rv = bind(listen_socket, result->ai_addr, (int)result->ai_addrlen);
    freeaddrinfo(result);
    if (rv < 0) {
        perror("bind failed");
        close(listen_socket);
        return 1;
    }

    rv = listen(listen_socket, SOMAXCONN);
    if (rv < 0) {
        perror("listen failed");
        close(listen_socket);
        return 1;
    }

    printf("The Tic-Tac-Toe server is running on port %s...\n", PORT);

    // Цикл принятия входящих подключений
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket < 0) {
            perror("accept failed");
            continue;
        }

        printf("Client connected. Socket = %d\n", client_socket);

        // Создаём поток для клиента
        pthread_t thread_id;
        // Чтобы передать socket в поток, нужно либо передавать его напрямую (приведение к (void*)),
        // либо завести динамическую переменную. Для простоты — напрямую:
        int* sock_ptr = malloc(sizeof(int));
        if (!sock_ptr) {
            perror("malloc for client socket");
            close(client_socket);
            continue;
        }
        *sock_ptr = client_socket;

        if (pthread_create(&thread_id, NULL, client_thread, sock_ptr) != 0) {
            perror("pthread_create failed");
            close(client_socket);
            free(sock_ptr);
        } else {
            // Чтобы не накапливать потоки, делаем «detach»
            pthread_detach(thread_id);
        }
    }

    close(listen_socket);

    // Уничтожаем мьютексы
    pthread_mutex_destroy(&users_lock);
    pthread_mutex_destroy(&games_lock);

    return 0;
}

void init_data_structures() {
    memset(users, 0, sizeof(users));
    memset(games, 0, sizeof(games));
    memset(sessions, 0, sizeof(sessions));
    user_count = 0;
    game_count = 0;
}

void* client_thread(void* param) 
{
    int client_socket = *((int*)param);
    free(param); // Освобождаем память под int*

    char recvbuf[BUF_SIZE];
    memset(recvbuf, 0, sizeof(recvbuf));

    const char* welcome_msg = "WELCOME\n";
    send_to_client(client_socket, welcome_msg);
    printf("Sent to client (%d): %s", client_socket, welcome_msg);

    while (1) {
        int bytes_received = recv(client_socket, recvbuf, BUF_SIZE - 1, 0);
        if (bytes_received > 0) {
            recvbuf[bytes_received] = '\0';
            printf("Received from client (%d): %s", client_socket, recvbuf);

            // Парсим команду
            char* cmd_line = recvbuf;
            char* cmd = strtok(cmd_line, " \r\n");
            if (!cmd) continue;

            if (strcmp(cmd, "REGISTER") == 0) {
                char* username = strtok(NULL, " \r\n");
                char* password = strtok(NULL, " \r\n");
                if (username && password) {
                    if (register_user(username, password) == 0) {
                        printf("User registered: %s\n", username);
                        send_to_client(client_socket, "OK Registered\n");
                    } else {
                        send_to_client(client_socket, "ERR UsernameTakenOrError\n");
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            }
            else if (strcmp(cmd, "LOGIN") == 0) {
                char* username = strtok(NULL, " \r\n");
                char* password = strtok(NULL, " \r\n");
                if (username && password) {
                    int uid = login_user(username, password, client_socket);
                    if (uid >= 0) {
                        send_to_client(client_socket, "OK LoggedIn\n");
                    } else {
                        send_to_client(client_socket, "ERR InvalidCredentials\n");
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            }
            else if (strcmp(cmd, "CREATE_GAME") == 0) {
                char* g_size_str = strtok(NULL, " \r\n");
                int uid = find_user_by_socket(client_socket);
                if (uid < 0) {
                    send_to_client(client_socket, "ERR NotLoggedIn\n");
                } else {
                    int g_size = atoi(g_size_str);
                    int game_id = create_game(client_socket, g_size); // по умолчанию создаём поле 3x3
                    if (game_id > 0) {
                        char buf[64];
                        sprintf(buf, "OK GameCreated. GameID - %d UID - %d Size - %d\n", game_id, uid, g_size);
                        send_to_client(client_socket, buf);
                    } else {
                        send_to_client(client_socket, "ERR CannotCreateGame\n");
                    }
                }
            }            
            else if (strcmp(cmd, "START_GAME") == 0) {
                char* game_id_str = strtok(NULL, " \r\n");
                if (game_id_str) {
                    int game_id = atoi(game_id_str);
                    int uid = find_user_by_socket(client_socket);

                    if (uid < 0) {
                        send_to_client(client_socket, "ERR NotLoggedIn\n");
                    } else {
                        // Отправляем сообщение создателю игры, чтобы он отрисовал поле
                        pthread_mutex_lock(&games_lock);
                        for (int i = 0; i < game_count; i++) {
                            if (games[i].game_id == game_id && games[i].is_active) {
                                if (games[i].player1_id >= 0) {
                                    char start_msg[64];
                                    sprintf(start_msg, "START_GAME %d\n", game_id);
                                    send_to_client(users[games[i].player1_id].client_socket, start_msg);
                                }
                                break;
                            }
                        }
                        pthread_mutex_unlock(&games_lock);
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            }
            else if (strcmp(cmd, "JOIN_GAME") == 0) {
                char* g_id_str = strtok(NULL, " \r\n");
                if (g_id_str) {
                    int g_id = atoi(g_id_str);
                    int uid = find_user_by_socket(client_socket);
                    if (uid < 0) {
                        send_to_client(client_socket, "ERR NotLoggedIn\n");
                    } else {
                        if (join_game(client_socket, g_id) == 0) {
                            char ptr[128];
                            sprintf(ptr, "OK GameJoined. GameID - %d UID - %d Size - %d\n", g_id, uid, games[g_id - 1].n);
                            send_to_client(client_socket, ptr);
                        } else {
                            send_to_client(client_socket, "ERR CannotJoin\n");
                        }
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            }
            else if (strcmp(cmd, "MOVE") == 0) {
                char* game_id_str = strtok(NULL, " \r\n");
                char* symbol     = strtok(NULL, " \r\n");
                char* x_str      = strtok(NULL, " \r\n");
                char* y_str      = strtok(NULL, " \r\n");

                if (game_id_str && symbol && x_str && y_str) {
                    int game_id = atoi(game_id_str);
                    int x = atoi(x_str);
                    int y = atoi(y_str);

                    int uid = find_user_by_socket(client_socket);
                    if (uid < 0) {
                        send_to_client(client_socket, "ERR NotLoggedIn\n");
                        continue;
                    }
                    if (strcmp(symbol, "X") != 0 && strcmp(symbol, "O") != 0) {
                        send_to_client(client_socket, "ERR InvalidSymbol\n");
                        continue;
                    }

                    if (make_move(client_socket, game_id, uid, symbol[0], x, y) == 0) {
                        send_to_client(client_socket, "OK MoveAccepted\n");
                    } else {
                        send_to_client(client_socket, "ERR InvalidMove\n");
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            } else if (strcmp(cmd, "LIST_GAMES") == 0) {
                     // Собираем список активных игр и отправляем клиенту
                send_to_client(client_socket, "LIST_GAMES\n");

                pthread_mutex_lock(&games_lock);
                for (int i = 0; i < game_count; i++) {
                    if (games[i].is_active) {
                        // допустим, если player2_id == -1, то игра free, иначе busy
                        int p1 = games[i].player1_id;
                        int p2 = games[i].player2_id;
                        const char* creator = (p1 >= 0) ? users[p1].username : "Unknown";
                        const char* status = (p2 == -1) ? "Free" : "Busy";
                        char line[128];
                        sprintf(line, "%d %s %s\n", games[i].game_id, creator, status);
                        send_to_client(client_socket, line);
                    }
                }
                pthread_mutex_unlock(&games_lock);

                send_to_client(client_socket, "END\n");
            }
            else if (strcmp(cmd, "CHAT") == 0) {
                //char* game_id_str = strtok(NULL, " \r\n");
                char* message = strtok(NULL, "\n"); // остаток строки - сообщение

                if (message) {
                    //int game_id = atoi(game_id_str);
                    int uid = find_user_by_socket(client_socket);

                    if (uid < 0) {
                        send_to_client(client_socket, "ERR NotLoggedIn\n");
                    } else {
                        // Блокируем список игр
                        char buf[BUF_SIZE];
                        sprintf(buf, "CHAT %s %s\n", users[uid].username, message);

                        pthread_mutex_lock(&users_lock);
                        for (int i = 0; i < user_count; i++) {
                            if (users[i].logged_in) {
                                printf("CHAT %s %s\n", users[uid].username, message);
                                send_to_client(users[i].client_socket, buf);
                            }
                        }
                        pthread_mutex_unlock(&users_lock);
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            }
            else if (strcmp(cmd, "EXIT_GAME") == 0) {
                char* game_id_str = strtok(NULL, " \r\n");
                if (game_id_str) {
                    int game_id = atoi(game_id_str);
                    int uid = find_user_by_socket(client_socket);

                    if (uid < 0) {
                        send_to_client(client_socket, "ERR NotLoggedIn\n");
                    } else {
                        pthread_mutex_lock(&games_lock);
                        for (int i = 0; i < game_count; i++) {
                            if (games[i].game_id == game_id) {
                                if (games[i].player1_id == uid) games[i].player1_id = -1;
                                if (games[i].player2_id == uid) games[i].player2_id = -1;

                                if (games[i].player1_id == -1 && games[i].player2_id == -1) {
                                    games[i].is_active = 0;
                                    printf("Игра %d завершена из-за выхода игроков.\n", game_id);
                                }
                                break;
                            }
                        }
                        pthread_mutex_unlock(&games_lock);

                        send_to_client(client_socket, "OK GameExited\n");
                    }
                } else {
                    send_to_client(client_socket, "ERR BadSyntax\n");
                }
            }
            else if (strcmp(cmd, "LOGOUT") == 0) {
                logout_user(client_socket);
                send_to_client(client_socket, "OK LoggedOut\n");
            }
            else {
                send_to_client(client_socket, "ERR UnknownCommand\n");
            }
        }
        else if (bytes_received == 0) {
            printf("Client (%d) disconnected.\n", client_socket);
            logout_user(client_socket);
            close(client_socket);
            break;
        }
        else {
            perror("recv failed");
            logout_user(client_socket);
            close(client_socket);
            break;
        }
    }

    pthread_exit(NULL);
    return NULL; // на всякий случай
}


int register_user(const char* username, const char* password) {
    pthread_mutex_lock(&users_lock);

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            pthread_mutex_unlock(&users_lock);
            return -1; // юзер уже есть
        }
    }
    if (user_count >= MAX_USERS) {
        pthread_mutex_unlock(&users_lock);
        return -1;
    }

    strcpy(users[user_count].username, username);
    strcpy(users[user_count].password, password);
    users[user_count].logged_in = 0;
    users[user_count].uid = user_count; // присваиваем UID = индексу
    user_count++;

    pthread_mutex_unlock(&users_lock);
    return 0;
}

int login_user(const char* username, const char* password, int sock) {
    pthread_mutex_lock(&users_lock);

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0 && 
            strcmp(users[i].password, password) == 0) 
        {
            users[i].logged_in = 1;
            users[i].client_socket = sock;

            printf("User logged in: %s, UID - %d\n", username, users[i].uid);

            // Создаем сессию
            int sid = create_session(i);
            if (sid >= 0) {
                printf("User logged in: %s, SID - %d\n", username, sid);
                pthread_mutex_unlock(&users_lock);
                return i;
            } else {
                // Не удалось создать сессию
                users[i].logged_in = 0;
                users[i].client_socket = -1;
                pthread_mutex_unlock(&users_lock);
                return -1;
            }
        }
    }

    pthread_mutex_unlock(&users_lock);
    return -1;
}

void logout_user(int sock) {
    pthread_mutex_lock(&users_lock);
    for (int i = 0; i < user_count; i++) {
        if (users[i].client_socket == sock && users[i].logged_in == 1) {
            users[i].logged_in = 0;
            users[i].client_socket = -1;
            // Освобождаем сессию
            for (int s = 0; s < MAX_USERS; s++) {
                if (sessions[s].in_use && sessions[s].user_id == i) {
                    sessions[s].in_use = 0;
                    sessions[s].session_id[0] = '\0';
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&users_lock);
}

int find_user_by_socket(int sock) {
    pthread_mutex_lock(&users_lock);
    for (int i = 0; i < user_count; i++) {
        if (users[i].client_socket == sock && users[i].logged_in == 1) {
            pthread_mutex_unlock(&users_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&users_lock);
    return -1; 
}

int create_game(int sock, int n) {
    int uid = find_user_by_socket(sock);
    if (uid < 0) return -1;

    pthread_mutex_lock(&games_lock);

    if (game_count >= MAX_GAMES) {
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    Game* g = &games[game_count];
    g->n = n;
    g->board = (char**)malloc(n * sizeof(char*));
    if (!g->board) {
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        g->board[i] = (char*)malloc(n * sizeof(char));
        if (!g->board[i]) {
            // Освобождаем всё, что успели аллоцировать
            for (int j = 0; j < i; j++) {
                free(g->board[j]);
            }
            free(g->board);
            pthread_mutex_unlock(&games_lock);
            return -1;
        }
        memset(g->board[i], ' ', n);
    }

    g->player1_id = uid;
    g->player2_id = -1;
    g->current_player_id = uid;
    g->game_id = game_count + 1;
    g->is_active = 1;

    game_count++;
    pthread_mutex_unlock(&games_lock);

    return g->game_id;
}


int join_game(int sock, int game_id) {
    int uid = find_user_by_socket(sock);
    if (uid < 0) return -1;

    pthread_mutex_lock(&games_lock);

    for (int i = 0; i < game_count; i++) {
        if (games[i].game_id == game_id && games[i].is_active == 1) {
            if (games[i].player2_id == -1 && games[i].player1_id != uid) {
                games[i].player2_id = uid;

                // Отправляем сообщение создателю игры, чтобы он отрисовал поле
                if (games[i].player1_id >= 0) {
                    char start_msg[64];
                    sprintf(start_msg, "START_GAME %d\n", game_id);
                    send_to_client(users[games[i].player1_id].client_socket, start_msg);
                }

                pthread_mutex_unlock(&games_lock);
                return 0;
            }
        }
    }

    pthread_mutex_unlock(&games_lock);
    return -1;
}

int make_move(int sock, int game_id, int uid, char symbol, int x, int y) {
    pthread_mutex_lock(&games_lock);

    // Поиск игры
    Game* g = NULL;
    for (int i = 0; i < game_count; i++) {
        if (games[i].game_id == game_id) {
            g = &games[i];
            break;
        }
    }
    if (!g || !g->is_active) {
        send_to_client(sock, "ERR GameNotFoundOrInactive\n");
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    // Проверка, является ли пользователь одним из игроков
    if (g->player1_id != uid && g->player2_id != uid) {
        send_to_client(sock, "ERR NotAPlayerInGame\n");
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    // Проверка соответствия символа
    char expected_symbol = (g->player1_id == uid) ? 'X' : 'O';
    if (symbol != expected_symbol) {
        send_to_client(sock, "ERR InvalidSymbol\n");
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    // Проверка очередности хода
    if (g->current_player_id != uid) {
        send_to_client(sock, "ERR NotYourTurn\n");
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    // Проверка корректности хода
    if (x < 0 || y < 0 || x >= g->n || y >= g->n || g->board[x][y] != ' ') {
        send_to_client(sock, "ERR InvalidMove\n");
        pthread_mutex_unlock(&games_lock);
        return -1;
    }

    g->board[x][y] = symbol;
    printf("1-st - %d\t 2-nd - %d\n", g->player1_id, g->player2_id);
    // Проверка победы / ничьи
    int result = check_win_condition(g);
    if (result == 1) {
        // Победа
        printf("[INFO] Player %s wins game %d!\n", users[uid].username, game_id);
        g->is_active = 0;

        char win_msg[128];
        sprintf(win_msg, "GAME_OVER WIN %s Match ID: %d\n", users[uid].username, g->game_id);
        send_to_client(users[g->player1_id].client_socket, win_msg);
        if (g->player2_id >= 0) {
            send_to_client(users[g->player2_id].client_socket, win_msg);
        }
    } else if (result == 2) {
        // Ничья
        printf("[INFO] Game %d ended in a draw.\n", g->game_id);
        g->is_active = 0;

        char draw_msg[128];
        sprintf(draw_msg, "GAME_OVER DRAW Match ID: %d %d\n", g->game_id, g->game_id);
        send_to_client(users[g->player1_id].client_socket, draw_msg);
        if (g->player2_id >= 0) {
            send_to_client(users[g->player2_id].client_socket, draw_msg);
        }
    } else {
        // Игра продолжается — переключаем ход
        g->current_player_id = (g->current_player_id == g->player1_id) ? 
                                g->player2_id : g->player1_id;

        char next_msg[64];
        sprintf(next_msg, "NEXT_PLAYER %s\n", users[g->current_player_id].username);
        send_to_client(users[g->player1_id].client_socket, next_msg);
        if (g->player2_id >= 0) {
            send_to_client(users[g->player2_id].client_socket, next_msg);
        }

        // Рассылаем обновлённое состояние
        broadcast_game_state(g->game_id, x, y, symbol);
    }
    
    if(g->is_active == 0){

        char buf[BUF_SIZE];
        int pos = 0;

        pos += sprintf(buf + pos, "UPDATE_BOARD %d %d %d %c\n",
                        g->game_id, x, y, symbol);
        
        if (g->player1_id >= 0 && users[g->player1_id].logged_in){
            send_to_client(users[g->player1_id].client_socket, buf);
        }
        if (g->player2_id >= 0 && users[g->player2_id].logged_in){
            send_to_client(users[g->player2_id].client_socket, buf);
        }
    }


    pthread_mutex_unlock(&games_lock);
    return 0;
}

void switch_to_next_player(Game *g) {
    if (!g) return;
    if (g->current_player_id == g->player1_id) {
        g->current_player_id = g->player2_id;
    } else {
        g->current_player_id = g->player1_id;
    }
}

void broadcast_game_state(int game_id, int x, int y, char symbol) {
    Game* g = NULL;
    for (int i = 0; i < game_count; i++) {
        if (games[i].game_id == game_id) {
            g = &games[i];
            break;
        }
    }
    if (!g) return;

    char buf[BUF_SIZE];
    int pos = 0;
    pos += sprintf(buf + pos, "UPDATE_BOARD %d %d %d %c\n", 
                   g->game_id, x, y, symbol);

    // NEXT_PLAYER уже был отослан при make_move,
    // но при желании можно отсылать и здесь.

    // Отправляем обоим игрокам
    if (g->player1_id >= 0 && users[g->player1_id].logged_in) {
        send_to_client(users[g->player1_id].client_socket, buf);
    }
    if (g->player2_id >= 0 && users[g->player2_id].logged_in) {
        send_to_client(users[g->player2_id].client_socket, buf);
    }
}

int check_win_condition(Game* game) {
    int n = game->n;

    // Проверяем строки
    for (int i = 0; i < n; i++) {
        char first = game->board[i][0];
        if (first != ' ') {
            int j;
            for (j = 1; j < n; j++) {
                if (game->board[i][j] != first) break;
            }
            if (j == n) return 1; // победа
        }
    }
    // Проверяем столбцы
    for (int j = 0; j < n; j++) {
        char first = game->board[0][j];
        if (first != ' ') {
            int i;
            for (i = 1; i < n; i++) {
                if (game->board[i][j] != first) break;
            }
            if (i == n) return 1;
        }
    }
    // Диагональ (левая верхняя -> правая нижняя)
    {
        char first = game->board[0][0];
        if (first != ' ') {
            int i;
            for (i = 1; i < n; i++) {
                if (game->board[i][i] != first) break;
            }
            if (i == n) return 1;
        }
    }
    // Диагональ (правая верхняя -> левая нижняя)
    {
        char first = game->board[0][n - 1];
        if (first != ' ') {
            int i;
            for (i = 1; i < n; i++) {
                if (game->board[i][n - 1 - i] != first) break;
            }
            if (i == n) return 1;
        }
    }
    // Проверяем, остались ли пустые клетки
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (game->board[i][j] == ' ') {
                return 0; // игра продолжается
            }
        }
    }
    // Если пустых нет — ничья
    return 2;
}

void send_to_client(int sock, const char* msg) {
    if (sock < 0) return;
    send(sock, msg, (int)strlen(msg), 0);
}

void generate_session_id(char* buf, size_t len) {
    static const char charset[] = "0123456789";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    buf[len - 1] = '\0';
}

int create_session(int user_id) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (!sessions[i].in_use) {
            sessions[i].in_use = 1;
            sessions[i].user_id = user_id;
            generate_session_id(sessions[i].session_id, sizeof(sessions[i].session_id));
            return i;
        }
    }
    return -1;
}
