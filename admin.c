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

// aici avem functia cu care trimitem un mesaj udp catre server si asteptam sa ne raspunda, dar cu un timeout
void send_command_to_server(int sockfd, struct sockaddr_in *server_addr, const char *command, char *response_buffer, int max_len) {
    socklen_t addr_len = sizeof(*server_addr);
    
    // aruncam comanda pe retea spre server
    sendto(sockfd, command, strlen(command), 0, (struct sockaddr *)server_addr, addr_len);

    // punem doar un caracter nul pe prima pozitie in loc sa stergem tot buffer ul
    // e de ajuns daca punem terminatorul la final dupa ce citim datele
    response_buffer[0] = '\0';

    // asteptam sa vedem daca serverul zice ceva, avem grija sa nu ne blocam la infinit
    int bytes_received = recvfrom(sockfd, response_buffer, max_len - 1, 0, (struct sockaddr *)server_addr, &addr_len);
    
    if (bytes_received < 0) {
        strcpy(response_buffer, "Eroare: Serverul nu a raspuns sau este offline");
    } else {
        response_buffer[bytes_received] = '\0';
    }
}

int main() {
    // prima data ne cream un socket udp
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        printf("Eroare la crearea socket-ului UDP\n");
        return 1;
    }

    // punem un timeout pe operatia de primire ca sa nu ne inghete interfata, am ales doua secunde
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ADMIN_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    
    // curatam spatiul extra din structura adresei fix la nivel de biti
    for (int i = 0; i < 8; i++) {
        server_addr.sin_zero[i] = '\0';
    }

    // acum trecem la pornirea librariei ncurses
    initscr();            // intram in modul vizual
    cbreak();             // oprim buffer ul pe linii
    noecho();             // ascundem ce tasteaza utilizatorul in mod normal
    keypad(stdscr, TRUE); // ne asiguram ca ne merg sagetile directionale de pe tastatura

    int choice = 0;
    int max_choices = 4;
    const char *options[] = {
        "1. Raport Status Server",
        "2. Afisare Clienti Conectati",
        "3. Deconectare Client",
        "4. Iesire din Panoul de Administrare"
    };

    char response_buffer[2048] = {0};
    strcpy(response_buffer, "Bine ai venit in Panoul de Administrare.\nApasa Enter pe o optiune pentru a interoga serverul.");

    int running = 1;

    while (running) {
        clear(); // stergem tot ce e pe ecran la fiecare pas
        
        // punem si noi un titlu dragut sus
        mvprintw(1, 2, "=== Dockerfile Generator - Admin Panel ===");
        mvprintw(2, 2, "Foloseste sagetile pentru a naviga. Enter pentru a selecta.");

        // incepem sa desenam fiecare optiune din meniu
        for (int i = 0; i < max_choices; i++) {
            if (i == choice) {
                attron(A_REVERSE); // facem optiunea sa iasa in evidenta cand e selectata
                mvprintw(4 + i, 4, "%s", options[i]);
                attroff(A_REVERSE);
            } else {
                mvprintw(4 + i, 4, "%s", options[i]);
            }
        }

        // aici pregatim o zona separata unde sa vedem ce ne zice serverul inapoi
        mvprintw(10, 2, "--- Raspuns Server ---");
        
        // printam manual litera cu litera pe ecran ca sa facem un fel de auto incadrare pe randuri
        int line = 11;
        int col = 2;
        for (int i = 0; response_buffer[i] != '\0'; i++) {
            if (response_buffer[i] == '\n') {
                line++;
                col = 2;
            } else {
                mvaddch(line, col, response_buffer[i]);
                col++;
            }
            // cand ajungem la capatul liniei ne mutam cursorul mai jos cu un rand
            if (col > 78) {
                line++;
                col = 2;
            }
        }

        refresh(); // dam un refresh sa apara pe ecran ce am desenat

        // stam sa vedem ce apasa omul pe tastatura
        int ch = getch();
        
        switch (ch) {
            case KEY_UP:
                choice--;
                if (choice < 0) choice = max_choices - 1;
                break;
            case KEY_DOWN:
                choice++;
                if (choice >= max_choices) choice = 0;
                break;
            case 10: // a apasat enter deci trimitem comanda asociata
                if (choice == 0) {
                    send_command_to_server(sockfd, &server_addr, "CMD:STATUS", response_buffer, sizeof(response_buffer));
                } 
                else if (choice == 1) {
                    send_command_to_server(sockfd, &server_addr, "CMD:CLIENTS", response_buffer, sizeof(response_buffer));
                } 
                else if (choice == 2) {
                    // cand dam kick pur si simplu trimitem cererea simpla momentan
                    // in viitor probabil o sa preluam si un ip de undeva din interfata
                    send_command_to_server(sockfd, &server_addr, "CMD:KICK", response_buffer, sizeof(response_buffer));
                } 
                else if (choice == 3) {
                    // ii zicem serverului ca iesim ca sa poata accepta alt admin in locul nostru
                    send_command_to_server(sockfd, &server_addr, "CMD:LOGOUT", response_buffer, sizeof(response_buffer));
                    running = 0;
                }
                break;
        }
    }

    // la final facem curatenie si inchidem totul
    endwin(); // oprim modul vizual si ne intoarcem la terminalul normal
    close(sockfd);

    return 0;
}