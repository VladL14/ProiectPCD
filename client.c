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

// functie ca sa inlocuim strlen
size_t custom_len(const char *str) {
    size_t i = 0;
    while (str[i] != '\0') i++;
    return i;
}

void load_env_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        return; // daca nu exista .env, mergem pe valorile default din cod
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
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
            if (len > 0 && value[len - 1] == '\n') value[len - 1] = '\0';
            len = custom_len(value);
            if (len > 0 && value[len - 1] == '\r') value[len - 1] = '\0';

            // injectam variabila in mediul procesului curent
            setenv(key, value, 1);
        }
    }
    fclose(fp);
}

int main() {
    // incarcam variabilele din .env
    load_env_file(".env");

    // citim portul unde trebuie sa ne conectam
    int port = 8080; // portul default (fallback)
    char *env_port = getenv("SERVER_PORT");
    if (env_port != NULL) {
        port = (int)strtol(env_port, NULL, 10);
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
    for (int i = 0; i < 8; i++) server_addr.sin_zero[i] = '\0'; // Curatam restul structurii

    // Incercam sa stabilim conexiunea cu serverul
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Conexiune la server esuata");
        close(sockfd);
        return 1;
    }

    // Afisam un mesaj de succes si verificam daca write ul se trimite(pentru a evita erorile de la build)
    if (write(STDOUT_FILENO, "[Client] Conectat cu succes la server.\n", 39) < 0) {
        perror("Eroare scriere consola");
    }
    // facem bucla pentru a nu se inchide clientul dupa o singura comanda
    while (1) {
        // Afisam prompt ul custom care sa se afiseze la fiecare pas ca la o tema anterioara
        if (write(STDOUT_FILENO, "comanda:> ", 11) < 0) {
            perror("Eroare afisare prompt");
        }
        
        char input[1024];
        for (int i = 0; i < 1024; i++) input[i] = '\0';

        // Citim de la tastatura ce scrie utilizatorul
        ssize_t n = read(STDIN_FILENO, input, 1023);
        if (n <= 0) break; 
        input[n - 1] = '\0'; // Inlocuim enter ul de la final cu terminatorul

        // Daca utilizatorul scrie exit inchidem terminalul clientului
        if (strcmp(input, "exit") == 0) break;
        if (input[0] == '\0') continue; // daca da enter in gol nu se intampla nimic

        char payload[1024]; // construim mesajul pe care il trimitem in retea
        for (int i = 0; i < 1024; i++) payload[i] = '\0';
        size_t payload_len = 0;

        char output_file[256];
        for (int i = 0; i < 256; i++) output_file[i] = '\0';
        
        // Fisierul in care salvam dockerfile ul este by default "Dockerfile.gen"
        char default_out[] = "Dockerfile.gen";
        int idx = 0;
        while (default_out[idx]) { output_file[idx] = default_out[idx]; idx++; }

        // Folosim strtok pentru a sparge linia citita de la tastatura 
        char *token = strtok(input, " ");
        while (token != NULL) {
            // Daca gasim --dep in comanda data luam urmatorul cuvant si il lipim in payload sub forma D:pachet
            if (strcmp(token, "--dep") == 0) {
                token = strtok(NULL, " "); 
                if (token) {
                    payload[payload_len++] = 'D'; payload[payload_len++] = ':'; 
                    int j = 0; while (token[j]) payload[payload_len++] = token[j++];
                    payload[payload_len++] = ' '; 
                }
            } 
            // Daca gasim --env il convertim in E:variabila
            else if (strcmp(token, "--env") == 0) {
                token = strtok(NULL, " ");
                if (token) {
                    payload[payload_len++] = 'E'; payload[payload_len++] = ':'; 
                    int j = 0; while (token[j]) payload[payload_len++] = token[j++];
                    payload[payload_len++] = ' ';
                }
            } 
            // Daca gasim --copy il convertim in C:fisier
            else if (strcmp(token, "--copy") == 0) {
                token = strtok(NULL, " ");
                if (token) {
                    payload[payload_len++] = 'C'; payload[payload_len++] = ':'; 
                    int j = 0; while (token[j]) payload[payload_len++] = token[j++];
                    payload[payload_len++] = ' ';
                }
            } 
            // Daca utilizatorul a specificat --out, suprascriem numele default al fisierului
            else if (strcmp(token, "--out") == 0) {
                token = strtok(NULL, " ");
                if (token) {
                    int j = 0; while (token[j]) { output_file[j] = token[j]; j++; }
                    output_file[j] = '\0';
                }
            }
            token = strtok(NULL, " "); // mergem la urmatorul token din input
        }

        // daca nu baga niciun parametru valid
        if (payload_len == 0) {
            if (write(STDOUT_FILENO, "Comanda invalida/goala. In cazul in care doriti sa generati un Dockerfile, se recomanda folosirea structurii --dep x --env y --copy z\n", 135) < 0) {
                perror("Eroare scriere");
            }
            continue;
        }

        // Trimitem tot sirul format catre server
        send(sockfd, payload, payload_len, 0);

        // Deschidem fisierul in care vom salva dockerfile ul primit
        int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("Eroare la crearea fisierului de output");
            continue;
        }

        char buffer[1024];
        ssize_t bytes_received;
        int eof_found = 0;

        // Serverul nu inchide conexiunea ca sa putem da si urmatoarele comenzi deci ascultam pe retea sa gasim identificatorul eof
        while (!eof_found && (bytes_received = recv(sockfd, buffer, 1023, 0)) > 0) {
            buffer[bytes_received] = '\0';

            // cautam eof in buffer cu strstr
            char *eof_ptr = strstr(buffer, "===EOF===");
            if(eof_ptr != NULL){
                eof_found = 1;
                bytes_received = eof_ptr - buffer;//calculam fix cate caractere sunt inainte de eof ca sa nu scriem pe disk
            }
            
            // Daca avem caractere reale pentru dockerfile le scriem pe disk
            if (bytes_received > 0) {
                if (write(fd, buffer, bytes_received) < 0) {
                    perror("Eroare la scrierea in fisier");
                }
            }
        }
        
        close(fd); // Inchidem fisierul de pe disk
        
        if (write(STDOUT_FILENO, "[Client] Dockerfile primit si asamblat cu succes.\n", 50) < 0) {
            perror("Eroare scriere consola finala");
        }
    } 

    // Inchidem socketul 
    close(sockfd);
    return 0;
}