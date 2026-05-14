/**
 * P8
 * Proiect PCD - Dockerfile Generator (Client)
 * aici avem implementarea clientului TCP. Ideea a fost sa cream un shell interactiv in care utilizatorul sa introduca comenzile necesare pentru generarea dockerfile ului, iar noi sa traducem acele argumente in format simplu  pentru server (ex: D:curl E:PORT=80). Clientul se conecteaza o singura data la server, trimite cererea si apoi asteapta sa primeasca inapoi raspunsul de la server. Deoarece conexiunea nu stie cand se termina un fisier de construit am implementat un mecanism prin care clientul citeste pana gaseste textul "===EOF===" trimis de server, iar dupa inchide fisierul salvat local si cere o noua comanda.
 */

#include <stdio.h>       // perror
#include <stdlib.h>      // functii de baza standard
#include <unistd.h>      // read, write, close
#include <fcntl.h>       //  O_WRONLY, O_CREAT, O_TRUNC
#include <string.h>      // strcmp,strtok
#include <sys/socket.h>  // socket, connect, send, recv
#include <netinet/in.h>  // pentru familiile de adrese si structurile de porturi (sockaddr_in)
#include <arpa/inet.h>   // pentru inet_addr, converteste adresa ip in format de retea

#define BUFFER_SIZE 1024
#define ENV_LINE_SIZE 256
#define OUTPUT_FILE_SIZE 256
#define DEFAULT_PORT 8080
#define DECIMAL_BASE 10
#define SIN_ZERO_SIZE 8
#define PROMPT_SIZE 11
#define SUCCESS_MSG_SIZE 39
#define INVALID_CMD_MSG_SIZE 135
#define FINAL_MSG_SIZE 50
#define READ_SIZE 1023
#define FILE_PERMISSIONS 0644

// functie ca sa inlocuim strlen
static size_t custom_len(const char *str) {
    size_t idx = 0;
    while (str[idx] != '\0') {
        idx++;
    }
    return idx;
}

static void load_env_file(const char *filename) {
    FILE *file_ptr = fopen(filename, "r");
    if (file_ptr == NULL) {
        return; // daca nu exista .env, mergem pe valorile default din cod
    }

    char line[ENV_LINE_SIZE];
    while (fgets(line, sizeof(line), file_ptr) != NULL) {
        // ignoram liniile goale sau comentariile
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\r') {
            continue;
        }

        // cautam separatorul '='
        char *separator = strchr(line, '=');
        if (separator != NULL) {
            *separator = '\0';
            char *key = line;
            char *value = separator + 1;

            // eliminam newline-ul de la finalul valorii (\n sau \r\n)
            size_t len = custom_len(value);
            if (len > 0 && value[len - 1] == '\n') {
                value[len - 1] = '\0';
            }
            len = custom_len(value);
            if (len > 0 && value[len - 1] == '\r') {
                value[len - 1] = '\0';
            }

            if (setenv(key, value, 1) != 0) {
                perror("setenv");
            }
        }
    }
    if (fclose(file_ptr) != 0) {
        perror("fclose");
    }
}

int main(void) {
    // incarcam variabilele din .env
    load_env_file(".env");

    // citim portul unde trebuie sa ne conectam
    int port = DEFAULT_PORT; // portul default (fallback)
    char *env_port = getenv("SERVER_PORT");
    if (env_port != NULL) {
        port = (int)strtol(env_port, NULL, DECIMAL_BASE);
    }

    // Cream un socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Eroare la creare socket");
        return 1;
    }

    // Configuram adresa serverului la care vrem sa ne conectam
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET; // adresa ipv4
    server_addr.sin_port = htons((uint16_t)port); // setam portul 8080 si se converteste cu htons in format de retea
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // Ne conectam la ocalhost
    for (int idx = 0; idx < SIN_ZERO_SIZE; idx++) {
        server_addr.sin_zero[idx] = '\0'; // Curatam restul structurii
    }

    // Incercam sa stabilim conexiunea cu serverul
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Conexiune la server esuata");
        close(sockfd);
        return 1;
    }

    if (write(STDOUT_FILENO, "[Client] Conectat cu succes la server.\n", SUCCESS_MSG_SIZE) < 0) {
        perror("write");
    }
    // facem bucla pentru a nu se inchide clientul dupa o singura comanda
    while (1) {
        if (write(STDOUT_FILENO, "comanda:> ", PROMPT_SIZE) < 0) {
            perror("write");
        }
        
        char input[BUFFER_SIZE];
        for (int idx = 0; idx < BUFFER_SIZE; idx++) {
            input[idx] = '\0';
        }

        // Citim de la tastatura ce scrie utilizatorul
        ssize_t bytes_read = read(STDIN_FILENO, input, READ_SIZE);
        if (bytes_read <= 0) {
            break;
        }
        input[bytes_read - 1] = '\0'; // Inlocuim enter ul de la final cu terminatorul

        // Daca utilizatorul scrie exit inchidem terminalul clientului
        if (strcmp(input, "exit") == 0) {
            break;
        }
        if (input[0] == '\0') {
            continue; // daca da enter in gol nu se intampla nimic
        }

        char payload[BUFFER_SIZE]; // construim mesajul pe care il trimitem in retea
        for (int idx = 0; idx < BUFFER_SIZE; idx++) {
            payload[idx] = '\0';
        }
        size_t payload_len = 0;

        char output_file[OUTPUT_FILE_SIZE];
        for (int idx = 0; idx < OUTPUT_FILE_SIZE; idx++) {
            output_file[idx] = '\0';
        }
        
        // Fisierul in care salvam dockerfile ul este by default "Dockerfile.gen"
        const char default_out[] = "Dockerfile.gen";
        int idx = 0;
        while (default_out[idx]) {
            output_file[idx] = default_out[idx];
            idx++;
        }

        char *saveptr;
        char *token = strtok_r(input, " ", &saveptr);
        while (token != NULL) {
            if (strcmp(token, "--dep") == 0) {
                token = strtok_r(NULL, " ", &saveptr);
                if (token) {
                    payload[payload_len++] = 'D';
                    payload[payload_len++] = ':';
                    int jdx = 0;
                    while (token[jdx]) {
                        payload[payload_len++] = token[jdx++];
                    }
                    payload[payload_len++] = ' ';
                }
            }
            else if (strcmp(token, "--env") == 0) {
                token = strtok_r(NULL, " ", &saveptr);
                if (token) {
                    payload[payload_len++] = 'E';
                    payload[payload_len++] = ':';
                    int jdx = 0;
                    while (token[jdx]) {
                        payload[payload_len++] = token[jdx++];
                    }
                    payload[payload_len++] = ' ';
                }
            }
            else if (strcmp(token, "--copy") == 0) {
                token = strtok_r(NULL, " ", &saveptr);
                if (token) {
                    payload[payload_len++] = 'C';
                    payload[payload_len++] = ':';
                    int jdx = 0;
                    while (token[jdx]) {
                        payload[payload_len++] = token[jdx++];
                    }
                    payload[payload_len++] = ' ';
                }
            }
            else if (strcmp(token, "--out") == 0) {
                token = strtok_r(NULL, " ", &saveptr);
                if (token) {
                    int jdx = 0;
                    while (token[jdx]) {
                        output_file[jdx] = token[jdx];
                        jdx++;
                    }
                    output_file[jdx] = '\0';
                }
            }
            token = strtok_r(NULL, " ", &saveptr);
        }

        if (payload_len == 0) {
            if (write(STDOUT_FILENO, "Comanda invalida/goala. In cazul in care doriti sa generati un Dockerfile, se recomanda folosirea structurii --dep x --env y --copy z\n", INVALID_CMD_MSG_SIZE) < 0) {
                perror("write");
            }
            continue;
        }

        if (send(sockfd, payload, payload_len, 0) < 0) {
            perror("send");
            continue;
        }

        // Deschidem fisierul in care vom salva dockerfile ul primit
        int file_desc = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, FILE_PERMISSIONS);
        if (file_desc < 0) {
            perror("Eroare la crearea fisierului de output");
            continue;
        }

        char buffer[BUFFER_SIZE];
        ssize_t bytes_received;
        int eof_found = 0;

        // Serverul nu inchide conexiunea ca sa putem da si urmatoarele comenzi deci ascultam pe retea sa gasim identificatorul eof
        while (!eof_found && (bytes_received = recv(sockfd, buffer, READ_SIZE, 0)) > 0) {
            buffer[bytes_received] = '\0';

            // cautam eof in buffer cu strstr
            char *eof_ptr = strstr(buffer, "===EOF===");
            if(eof_ptr != NULL){
                eof_found = 1;
                bytes_received = eof_ptr - buffer;//calculam fix cate caractere sunt inainte de eof ca sa nu scriem pe disk
            }
            
            if (bytes_received > 0) {
                if (write(file_desc, buffer, (size_t)bytes_received) < 0) {
                    perror("write");
                    break;
                }
            }
        }
        
        close(file_desc);
        
        if (write(STDOUT_FILENO, "[Client] Dockerfile primit si asamblat cu succes.\n", FINAL_MSG_SIZE) < 0) {
            perror("write");
        }
    }

    // Inchidem socketul
    close(sockfd);
    return 0;
}