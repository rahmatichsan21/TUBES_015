#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_PLAYERS 100

typedef struct {
    char name[50];
    int level;
    int health;
    int wins;
    int client_socket;
    int in_duel; // 1 if the player is in a duel, 0 otherwise
} Player;

typedef struct {
    Player players[MAX_PLAYERS];
    int player_count;
} SharedData;

int shm_id;
SharedData *shared_data;

void init_shared_memory() {
    shm_id = shmget(IPC_PRIVATE, sizeof(SharedData), IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("shmget failed");
        exit(EXIT_FAILURE);
    }
    shared_data = (SharedData *)shmat(shm_id, NULL, 0);
    if (shared_data == (void *)-1) {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }
    shared_data->player_count = 0;
}

void cleanup_shared_memory() {
    shmdt(shared_data);
    shmctl(shm_id, IPC_RMID, NULL);
}

Player* get_or_create_player(const char* name, int client_socket) {
    for (int i = 0; i < shared_data->player_count; i++) {
        if (strcmp(shared_data->players[i].name, name) == 0) {
            return &shared_data->players[i];
        }
    }

    Player* new_player = &shared_data->players[shared_data->player_count++];
    strcpy(new_player->name, name);
    new_player->level = 1;
    new_player->health = 100;
    new_player->wins = 0;
    new_player->client_socket = client_socket;
    new_player->in_duel = 0;
    return new_player;
}

void handle_status(char* response) {
    char temp[BUFFER_SIZE];
    sprintf(response, "Connected players:\n");
    for (int i = 0; i < shared_data->player_count; i++) {
        sprintf(temp, "Player: %s | Level: %d | Health: %d | Wins: %d\n",
                shared_data->players[i].name,
                shared_data->players[i].level,
                shared_data->players[i].health,
                shared_data->players[i].wins);
        strcat(response, temp);
    }
}

void handle_hunt(Player* player, char* response) {
    while (1) {
        srand(time(NULL) + getpid());
        int monster_strength = (rand() % (player->level * 10)) + 1;

        if (monster_strength < player->health) {
            player->health -= monster_strength / 2;
            player->level++;
            sprintf(response, "[HUNT] You defeated a monster with strength %d!\nLevel up! Level: %d. Health: %d\n",
                    monster_strength, player->level, player->health);
        } else {
            player->health = 100;
            sprintf(response, "[HUNT] You were injured by a monster with strength %d and died.\nHealth reset to 100.\n",
                    monster_strength);
            return;
        }

        send(player->client_socket, response, strlen(response), 0);

        sprintf(response, "[HUNT] Continue hunting? (1 = yes, 2 = no): ");
        send(player->client_socket, response, strlen(response), 0);

        char buffer[BUFFER_SIZE];
        memset(buffer, 0, sizeof(buffer));
        read(player->client_socket, buffer, sizeof(buffer));
        int choice = atoi(buffer);

        if (choice == 2) {
            player->health = 100;
            sprintf(response, "[HUNT] Hunting cancelled. Health reset to 100.\n");
            return;
        }
    }
}

void handle_duel(Player* player, char* response) {
    if (player->in_duel) {
        sprintf(response, "[DUEL] You are already in a duel!\n");
        return;
    }

    // Tampilkan daftar lawan yang tersedia
    sprintf(response, "[DUEL] Available opponents:\n");
    int available_indices[MAX_PLAYERS];
    int available_count = 0;

    for (int i = 0; i < shared_data->player_count; i++) {
        Player* opponent = &shared_data->players[i];
        if (opponent != player && opponent->health > 0) {
            char temp[BUFFER_SIZE];
            sprintf(temp, "%d. Player: %s | Level: %d | Health: %d | Wins: %d\n",
                    available_count + 1, opponent->name, opponent->level, opponent->health, opponent->wins);
            strcat(response, temp);

            available_indices[available_count] = i;
            available_count++;
        }
    }

    if (available_count == 0) {
        strcat(response, "[DUEL] No available opponents.\n");
        return;
    }

    send(player->client_socket, response, strlen(response), 0);
    sprintf(response, "[DUEL] Enter the number of your opponent: ");
    send(player->client_socket, response, strlen(response), 0);

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    read(player->client_socket, buffer, sizeof(buffer));
    int choice = atoi(buffer) - 1;

    if (choice < 0 || choice >= available_count) {
        sprintf(response, "[DUEL] Invalid choice.\n");
        return;
    }

    Player* opponent = &shared_data->players[available_indices[choice]];
    player->in_duel = 1;
    opponent->in_duel = 1;

    // Informasi duel dimulai
    sprintf(response, "[DUEL] Duel started between %s and %s!\n", player->name, opponent->name);
    send(player->client_socket, response, strlen(response), 0);
    sprintf(response, "[DUEL] Your opponent's level: %d, health: %d\n", opponent->level, opponent->health);
    send(player->client_socket, response, strlen(response), 0);

    // Logika duel
    srand(time(NULL) + getpid());
    int player_attack = (rand() % 20) + player->level;
    int opponent_attack = (rand() % 20) + opponent->level;

    if (player_attack > opponent_attack) {
        opponent->health = 0;
        player->wins++;  // Pemain menang
        sprintf(response, "[DUEL] You won the duel against %s! Wins: %d\n", opponent->name, player->wins);
    } else {
        player->health = 0;
        opponent->wins++;  // Lawan menang
        sprintf(response, "[DUEL] You lost the duel against %s. Wins: %d\n", opponent->name, opponent->wins);
    }

    send(player->client_socket, response, strlen(response), 0);

    // Akhiri duel
    player->in_duel = 0;
    opponent->in_duel = 0;
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    read(client_socket, buffer, sizeof(buffer));
    Player* player = get_or_create_player(buffer, client_socket);
    sprintf(response, "Welcome, %s!\n", player->name);
    send(client_socket, response, strlen(response), 0);

    while (1) {
        memset(response, 0, sizeof(response));
        sprintf(response,
                "Main Menu:\n"
                "1. STATUS\n"
                "2. HUNT\n"
                "3. DUEL\n"
                "4. EXIT\n"
                "Enter your choice: ");
        send(client_socket, response, strlen(response), 0);

        memset(buffer, 0, sizeof(buffer));
        int bytes_read = read(client_socket, buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            close(client_socket);
            return;
        }

        int choice = atoi(buffer);
        memset(response, 0, sizeof(response));

        if (choice == 1) {
            handle_status(response);
        } else if (choice == 2) {
            handle_hunt(player, response);
        } else if (choice == 3) {
            handle_duel(player, response);
        } else if (choice == 4) {
            sprintf(response, "Goodbye, %s!\n", player->name);
            send(client_socket, response, strlen(response), 0);
            break;
        } else {
            sprintf(response, "Invalid choice. Please try again.\n");
        }

        send(client_socket, response, strlen(response), 0);
    }
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    init_shared_memory();

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, MAX_PLAYERS) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d\n", PORT);

    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept failed");
            exit(EXIT_FAILURE);
        }

        if (fork() == 0) {
            close(server_fd);
            handle_client(new_socket);
            close(new_socket);
            exit(0);
        } else {
            close(new_socket);
        }
    }

    cleanup_shared_memory();
    return 0;
}

