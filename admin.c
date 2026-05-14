/**
 * client de administrare pentru dockerfile generator
 * 
 * folosim ncurses pentru interfata vizuala si trimitem mesaje udp catre server prin sendto si recvfrom
 */

#include <stdio.h>
#include <stdlib.h>      // exit
#include <string.h>      // strlen
#include <unistd.h>      // functii de baza
#include <fcntl.h>
#include <sys/socket.h>  // functiile esentiale pentru comunicare pe retea
#include <netinet/in.h>  // structurile de adrese ipv4
#include <arpa/inet.h>   // inet addr
#include <sys/time.h>    // timeval pt timeout
#include <ncurses.h>     // includem ncurses pentru a desena in terminal

#define ADMIN_PORT 8081
#define SERVER_IP "127.0.0.1"
#define RESPONSE_BUFFER_SIZE 2048
#define SIN_ZERO_SIZE 8
#define TIMEOUT_SECONDS 2
#define MAX_CHOICES 4
#define ENTER_KEY 10
#define MAX_LINE_WIDTH 78
#define TITLE_ROW 1
#define SUBTITLE_ROW 2
#define MENU_START_ROW 4
#define RESPONSE_TITLE_ROW 10
#define RESPONSE_START_ROW 11
#define COLUMN_OFFSET 2
#define MENU_COLUMN_OFFSET 4

// aici avem functia cu care trimitem un mesaj udp catre server si asteptam sa ne raspunda, dar cu un timeout
static void send_command_to_server(int sockfd, struct sockaddr_in *server_addr, const char *command, char *response_buffer, int max_len) {
    socklen_t addr_len = sizeof(*server_addr);
    
    // aruncam comanda pe retea spre server
    (void)sendto(sockfd, command, strlen(command), 0, (struct sockaddr *)server_addr, addr_len);

    // punem doar un caracter nul pe prima pozitie in loc sa stergem tot buffer ul
    // e de ajuns daca punem terminatorul la final dupa ce citim datele
    response_buffer[0] = '\0';

    // asteptam sa vedem daca serverul zice ceva, avem grija sa nu ne blocam la infinit
    ssize_t bytes_received = recvfrom(sockfd, response_buffer, (size_t)(max_len - 1), 0, (struct sockaddr *)server_addr, &addr_len);
    
    if (bytes_received < 0) {
        (void)strcpy(response_buffer, "Eroare: Serverul nu a raspuns sau este offline");
    } else {
        response_buffer[bytes_received] = '\0';
    }
}

int main(void) {
    // prima data ne cream un socket udp
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        printf("Eroare la crearea socket-ului UDP\n");
        return 1;
    }

    // punem un timeout pe operatia de primire ca sa nu ne inghete interfata, am ales doua secunde
    struct timeval timeout_val;
    timeout_val.tv_sec = TIMEOUT_SECONDS;
    timeout_val.tv_usec = 0;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_val, sizeof(timeout_val));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ADMIN_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    
    // curatam spatiul extra din structura adresei fix la nivel de biti
    for (int idx = 0; idx < SIN_ZERO_SIZE; idx++) {
        server_addr.sin_zero[idx] = '\0';
    }

    // acum trecem la pornirea librariei ncurses
    (void)initscr();            // intram in modul vizual
    (void)cbreak();             // oprim buffer ul pe linii
    (void)noecho();             // ascundem ce tasteaza utilizatorul in mod normal
    (void)keypad(stdscr, TRUE); // ne asiguram ca ne merg sagetile directionale de pe tastatura

    int choice = 0;
    const char *options[MAX_CHOICES] = {
        "1. Raport Status Server",
        "2. Afisare Clienti Conectati",
        "3. Deconectare Client",
        "4. Iesire din Panoul de Administrare"
    };

    char response_buffer[RESPONSE_BUFFER_SIZE] = {0};
    (void)strcpy(response_buffer, "Bine ai venit in Panoul de Administrare.\nApasa Enter pe o optiune pentru a interoga serverul.");

    int running = 1;

    while (running) {
        (void)clear(); // stergem tot ce e pe ecran la fiecare pas
        
        // punem si noi un titlu dragut sus
        (void)mvprintw(TITLE_ROW, COLUMN_OFFSET, "=== Dockerfile Generator - Admin Panel ===");
        (void)mvprintw(SUBTITLE_ROW, COLUMN_OFFSET, "Foloseste sagetile pentru a naviga. Enter pentru a selecta.");

        // incepem sa desenam fiecare optiune din meniu
        for (int idx = 0; idx < MAX_CHOICES; idx++) {
            if (idx == choice) {
                (void)attron(A_REVERSE); // facem optiunea sa iasa in evidenta cand e selectata
                (void)mvprintw(MENU_START_ROW + idx, MENU_COLUMN_OFFSET, "%s", options[idx]);
                (void)attroff(A_REVERSE);
            } else {
                (void)mvprintw(MENU_START_ROW + idx, MENU_COLUMN_OFFSET, "%s", options[idx]);
            }
        }

        // aici pregatim o zona separata unde sa vedem ce ne zice serverul inapoi
        (void)mvprintw(RESPONSE_TITLE_ROW, COLUMN_OFFSET, "--- Raspuns Server ---");
        
        // printam manual litera cu litera pe ecran ca sa facem un fel de auto incadrare pe randuri
        int line = RESPONSE_START_ROW;
        int col = COLUMN_OFFSET;
        for (int idx = 0; response_buffer[idx] != '\0'; idx++) {
            if (response_buffer[idx] == '\n') {
                line++;
                col = COLUMN_OFFSET;
            } else {
                (void)mvaddch(line, col, response_buffer[idx]);
                col++;
            }
            // cand ajungem la capatul liniei ne mutam cursorul mai jos cu un rand
            if (col > MAX_LINE_WIDTH) {
                line++;
                col = COLUMN_OFFSET;
            }
        }

        (void)refresh(); // dam un refresh sa apara pe ecran ce am desenat

        // stam sa vedem ce apasa omul pe tastatura
        int key_pressed = getch();
        
        switch (key_pressed) {
            case KEY_UP:
                choice--;
                if (choice < 0) {
                    choice = MAX_CHOICES - 1;
                }
                break;
            case KEY_DOWN:
                choice++;
                if (choice >= MAX_CHOICES) {
                    choice = 0;
                }
                break;
            case ENTER_KEY: // a apasat enter deci trimitem comanda asociata
                if (choice == 0) {
                    send_command_to_server(sockfd, &server_addr, "CMD:STATUS", response_buffer, (int)sizeof(response_buffer));
                }
                else if (choice == 1) {
                    send_command_to_server(sockfd, &server_addr, "CMD:CLIENTS", response_buffer, (int)sizeof(response_buffer));
                }
                else if (choice == 2) {
                    // cand dam kick pur si simplu trimitem cererea simpla momentan
                    // in viitor probabil o sa preluam si un ip de undeva din interfata
                    send_command_to_server(sockfd, &server_addr, "CMD:KICK", response_buffer, (int)sizeof(response_buffer));
                }
                else if (choice == 3) {
                    // ii zicem serverului ca iesim ca sa poata accepta alt admin in locul nostru
                    send_command_to_server(sockfd, &server_addr, "CMD:LOGOUT", response_buffer, (int)sizeof(response_buffer));
                    running = 0;
                }
                break;
            default:
                break;
        }
    }

    // la final facem curatenie si inchidem totul
    (void)endwin(); // oprim modul vizual si ne intoarcem la terminalul normal
    close(sockfd);

    return 0;
}