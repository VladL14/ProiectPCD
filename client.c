#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    /* --- 1. SETUP CONEXIUNE TCP --- */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Eroare la creare socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // Conectare pe localhost
    for (int i = 0; i < 8; i++) server_addr.sin_zero[i] = '\0';

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Conexiune la server esuata");
        close(sockfd);
        return 1;
    }

    write(STDOUT_FILENO, "[Client] Conectat cu succes la server.\n", 39);

    /* --- 2. BUCLA INFINITA (SHELL INTERACTIV) --- */
    while (1) {
        write(STDOUT_FILENO, "comanda:> ", 11); // Prompt-ul nostru custom
        
        char input[1024];
        for (int i = 0; i < 1024; i++) input[i] = '\0';

        // Citim de la tastatura
        ssize_t n = read(STDIN_FILENO, input, 1023);
        if (n <= 0) break;
        input[n - 1] = '\0'; // Inlocuim \n (Enter) cu terminatorul de sir

        // Conditii de iesire sau resetare bucla
        if (strcmp(input, "exit") == 0) break;
        if (input[0] == '\0') continue; 

        /* --- 3. PREGATIREA DATELOR PENTRU SERVER --- */
        char payload[1024]; 
        for (int i = 0; i < 1024; i++) payload[i] = '\0';
        size_t payload_len = 0;

        char output_file[256];
        for (int i = 0; i < 256; i++) output_file[i] = '\0';
        
        // Setup nume default de fisier
        char default_out[] = "Dockerfile.gen";
        int idx = 0;
        while (default_out[idx]) { output_file[idx] = default_out[idx]; idx++; }

        // Parsare manuala folosind strtok
        char *token = strtok(input, " ");
        while (token != NULL) {
            if (strcmp(token, "--dep") == 0) {
                token = strtok(NULL, " "); 
                if (token) {
                    payload[payload_len++] = 'D'; payload[payload_len++] = ':'; 
                    int j = 0; while (token[j]) payload[payload_len++] = token[j++];
                    payload[payload_len++] = ' '; 
                }
            } else if (strcmp(token, "--env") == 0) {
                token = strtok(NULL, " ");
                if (token) {
                    payload[payload_len++] = 'E'; payload[payload_len++] = ':'; 
                    int j = 0; while (token[j]) payload[payload_len++] = token[j++];
                    payload[payload_len++] = ' ';
                }
            } else if (strcmp(token, "--copy") == 0) {
                token = strtok(NULL, " ");
                if (token) {
                    payload[payload_len++] = 'C'; payload[payload_len++] = ':'; 
                    int j = 0; while (token[j]) payload[payload_len++] = token[j++];
                    payload[payload_len++] = ' ';
                }
            } else if (strcmp(token, "--out") == 0) {
                token = strtok(NULL, " ");
                if (token) {
                    int j = 0; while (token[j]) { output_file[j] = token[j]; j++; }
                    output_file[j] = '\0';
                }
            }
            token = strtok(NULL, " "); 
        }

        if (payload_len == 0) {
            write(STDOUT_FILENO, "Comanda invalida/goala.\n", 24);
            continue;
        }

        /* --- 4. TRIMITERE SI RECEPTIE --- */
        send(sockfd, payload, payload_len, 0);

        int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("Eroare la crearea fisierului de output");
            continue;
        }

        char buffer[1024];
        ssize_t bytes_received;
        int eof_found = 0;

        // Receptie date pana cand gasim marker-ul ===EOF===
        while (!eof_found && (bytes_received = recv(sockfd, buffer, 1023, 0)) > 0) {
            buffer[bytes_received] = '\0';

            // Cautare manuala a marker-ului de final
            for (int i = 0; i <= bytes_received - 9; i++) {
                if (buffer[i] == '=' && buffer[i+1] == '=' && buffer[i+2] == '=' &&
                    buffer[i+3] == 'E' && buffer[i+4] == 'O' && buffer[i+5] == 'F' &&
                    buffer[i+6] == '=' && buffer[i+7] == '=' && buffer[i+8] == '=') {
                    
                    eof_found = 1;
                    bytes_received = i; 
                    break;
                }
            }
            
            if (bytes_received > 0) {
                write(fd, buffer, bytes_received);
            }
        }
        close(fd);
        write(STDOUT_FILENO, "[Client] Dockerfile primit si asamblat cu succes.\n", 50);
    } 

    close(sockfd);
    return 0;
}