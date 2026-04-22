/**
 * P8
 * Proiect PCD - Dockerfile Generator (Server)
 * Aici avem implementarea unui server TCP care primeste comenzi de la un client (dependinte, variabile de mediu, fisiere de copiat) si asambleaza un Dockerfile.
 * Ne folosim de fork() pentru a crea un proces copil pentru fiecare pachet cerut, fiii folosesc doua api uri (repology si fallback -> homebrew) folosind libcurl ca sa verifice existenta dependintei dorite impreuna cu versiunea. la cererea clientului de a pune in dockerfile dependinte variabile de mediu sau fisiere de copy, programul verifica existenata dependintelor, iar daca exista pune sursa si versiunea acesteia, plus celelalte lucruri hardcodate by default clasice unui dockerfile.
 */

#include <stdio.h>       // print, perror
#include <stdlib.h>      // exit()
#include <string.h>      // strcmp, strtok, strstr
#include <unistd.h>      // read, write, close
#include <fcntl.h>       // o_rdonly, o_wronly, o_creat, o_trunc
#include <sys/wait.h>    // waitpid, WIFEXITED
#include <sys/socket.h>  // socket, bind, listen, accept, send, recv
#include <netinet/in.h>  // pentru structurile de ip si porturi
#include <curl/curl.h>   // libraria externa cu care luam json ul de pe internet
#include <libconfig.h>   // libraria cu care citim cfg ul de baza al serverului

#define PORT 8080
#define CURL_BUFFER_SIZE 8192

// functie ca sa inlocuim strlen
size_t custom_len(const char *str) {
    size_t i = 0;
    while (str[i] != '\0') i++;
    return i;
}

// structura in care lipim bucatile de json venite de pe net
typedef struct {
    char data[CURL_BUFFER_SIZE];
    size_t len;
} CurlBuffer;

// functie dummy cand tragem ceva cu curl by default vrea sa printeze pe ecran, noi ii dam functia asta ca sa o opreasca
size_t curl_dummy_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb; 
}

// functia apelata de curl ca sa ne bage datele de pe net in bufferul nostru
size_t curl_write_buffer(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuffer *buf = (CurlBuffer *)userdata;
    size_t incoming = size * nmemb;
 
    // verificam sa nu crape daca vine un fisier prea mare
    if (buf->len + incoming >= sizeof(buf->data) - 1)
        incoming = sizeof(buf->data) - 1 - buf->len;
 
    for (int i = 0; i < (int)incoming; i++)
        buf->data[buf->len + i] = ((char *)ptr)[i];
 
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return size * nmemb; 
}

// parsare simpla de json de mana. gasim cuvantul "key" si luam ce e in ghilimele la valoare
int extract_json_string(const char *json, const char *key, char *out, size_t out_sz) {
    char pattern[128];
    pattern[0] = '"';
    int pi = 1, ki = 0;
    while (key[ki] && pi < 126) pattern[pi++] = key[ki++];
    pattern[pi++] = '"';
    pattern[pi] = '\0';
 
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
 
    pos += pi; // sarim peste cheie
    while (*pos == ' ' || *pos == ':') pos++; // sarim peste : si spatii
    if (*pos != '"') return 0; 
    pos++; 
 
    size_t i = 0; // tragem litera cu litera pana la urmatoarele ghilimele
    while (*pos && *pos != '"' && i < out_sz - 1) out[i++] = *pos++;
    out[i] = '\0';
 
    return (i > 0) ? 1 : 0;
}

// asta lasa fiul parintelui in fisierul temporar
typedef struct {
    int  valid;        // 1=gasit, 0=nu
    char version[64];  // versiunea gasita pe net
    char source[32];   // repology sau homebrew
} DepResult;

// prima data verificam pe repology 
static int check_via_repology(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    // asamblam url ul manual ca sa evitam sprintf, ui tine evidenta la ce caracter am ajuns in url, iar pi parcurge literele pachetului
    char url[512];
    int ui = 0;
    const char *base = "https://repology.org/api/v1/project/";
    while (*base && ui < 480) url[ui++] = *base++; // copiem adresa de baza
    
    int pi = 0;
    while (pkg[pi] && ui < 505) url[ui++] = pkg[pi++]; // lipim numele pachetului la final
    url[ui] = '\0';
 
    // pregatim un buffer in care sa ne puna curl datele cand le primeste de pe net
    CurlBuffer buf;
    buf.len = 0; 
    buf.data[0] = '\0';
 
    // setarile esentiale pentru curl
    curl_easy_setopt(curl, CURLOPT_URL, url); // ii spunem pe ce link sa intre
    
    // by default curl printeaza tot ce descarca direct in consola, noi nu vrem asta pt ca ne umple ecranul de json, asa ca ii dam functia curl_write_buffer care stie cum sa preia bucatile de date si ii dam adresa buffer ului unde sa le puna.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // daca site ul ne da redirect, mergem dupa el
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0"); // ne identificam ca useri sa nu se blocheze ca pentru boti
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L); // maxim 8 secunde sa ne raspunda altfel renuntam
    CURLcode res = curl_easy_perform(curl); // dam cererea pe retea
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code); // luam codul http(succes, esec)
    curl_easy_cleanup(curl);
 
    if (res != CURLE_OK || http_code != 200) return 0; // nu s a putut realiza cautarea sau pachetul nu exista
    if (buf.len < 5) return 0; 
 
    // pe repology daca nu exista pachetul iti returneaza in loc de eroare un array gol 
    if (buf.data[0] == '[' && buf.data[1] == ']') return 0;
    
    // tragem direct versiunea in variabila pe care o intoarcem
    extract_json_string(buf.data, "version", version, vsz);
    return 1;
}

//  fallback cauta pe homebrew pachetele generice de dev
static int check_via_homebrew(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    // la fel ca sus asamblam manual linkul din bucati ui pentru url index, pi pt package index si ei pt extension index
    char url[512];
    int ui = 0;
    const char *base = "https://formulae.brew.sh/api/formula/";
    while (*base && ui < 470) url[ui++] = *base++;
    
    int pi = 0;
    while (pkg[pi] && ui < 500) url[ui++] = pkg[pi++];
    
    const char *ext = ".json"; // homebrew cere neaparat sa pui .json la finalul url ului
    int ei = 0;
    while (ext[ei]) url[ui++] = ext[ei++];
    url[ui] = '\0';
 
    CurlBuffer buf;
    buf.len = 0; 
    buf.data[0] = '\0';
 
    // aceleasi setari de curl, le facem sa scrie in structura noastra si nu pe ecran
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
 
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
 
    if (res != CURLE_OK || http_code != 200) return 0;
 
    // homebrew are json ul diferit, gasim intai sectiunea cu stable si de acolo luam versiunea
    const char *ver_section = strstr(buf.data, "\"versions\"");
    if (ver_section) {
        extract_json_string(ver_section, "stable", version, vsz);
    }
    return 1;
}

// functia in care copilul pune mana la treaba. Incearca primul api si apoi pica pe al doilea daca e nevoie
static void child_check_dep(const char *dep_name, DepResult *res) {
    res->valid = 0;
    res->version[0] = '\0';
    res->source[0] = '\0';
 
    // facem un mesaj de log in memorie
    char log[256];
    int li = 0;
    const char *pre = "[MAP] Verific: ";
    int i = 0; while (pre[i]) log[li++] = pre[i++];
    i = 0; while (dep_name[i]) log[li++] = dep_name[i++];
    log[li++] = '\n';
    write(STDOUT_FILENO, log, li); // afisam un singur mesaj pe ecran
 
    if (check_via_repology(dep_name, res->version, sizeof(res->version))) {
        res->valid = 1;
        const char *src = "repology";
        i = 0; while (src[i]) res->source[i] = src[i++];
        res->source[i] = '\0';
    }
    else if (check_via_homebrew(dep_name, res->version, sizeof(res->version))) {
        res->valid = 1;
        const char *src = "homebrew";
        i = 0; while (src[i]) res->source[i] = src[i++];
        res->source[i] = '\0';
    }
}

// aici centralizam toata logica dupa ce a venit request ul pe socket
void process_request_and_send(int client_fd, char *request) {
    char deps[10][64];  int dep_count = 0;
    char envs[10][64];  int env_count = 0;
    char copies[10][64]; int copy_count = 0;

    // resetam matricea la fiecare request sa nu amestecam ce ne cere clientul
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 64; j++) {
            deps[i][j] = '\0'; envs[i][j] = '\0'; copies[i][j] = '\0';
        }
    }

    // impartim bufferul primit folosind strtok 
    char *token = strtok(request, " ");
    while (token != NULL) {
        // Daca incepe cu D: stim ca e dependinta
        if (token[0] == 'D' && token[1] == ':') {
            if (dep_count < 10) {
                int j = 0;
                // copiem litera cu litera incepem de la token[j + 2] pentru ca vrem sa sarim peste d si :
                while (token[j + 2] != '\0' && j < 63) { 
                    deps[dep_count][j] = token[j + 2]; 
                    j++; 
                }
                deps[dep_count][j] = '\0'; 
                dep_count++;
            }
        } 
        // Daca incepe cu E: e o variabila de mediu 
        else if (token[0] == 'E' && token[1] == ':') {
            if (env_count < 10) {
                int j = 0;
                // sarim la fel peste cele 2 litere prefixate si ne protejam la 63 de caractere max
                while (token[j + 2] != '\0' && j < 63) { 
                    envs[env_count][j] = token[j + 2]; 
                    j++; 
                }
                envs[env_count][j] = '\0'; 
                env_count++;
            }
        } 
        // Daca incepe cu C: este o comanda de copy
        else if (token[0] == 'C' && token[1] == ':') {
            if (copy_count < 10) {
                int j = 0;
                while (token[j + 2] != '\0' && j < 63) {//la fel si la copy
                    copies[copy_count][j] = token[j + 2];
                    j++;
                }
                copies[copy_count][j] = '\0'; 
                copy_count++;
            }
        }
        token = strtok(NULL, " "); // trecem la urmatorul argument impachetat in mesajul clientului
    }

    // folosim libconfig ca sa parsam valorile default scrise in cfg
    config_t cfg;
    config_init(&cfg);

    const char *base_image = "ubuntu:22.04";
    const char *maintainer = "admin";
    const char *workdir = "/app";

    if (config_read_file(&cfg, "demo.cfg")) {
        config_lookup_string(&cfg, "container.base_image", &base_image);
        config_lookup_string(&cfg, "container.maintainer", &maintainer);
        config_lookup_string(&cfg, "container.workdir", &workdir);
    }

    // de aici in jos incepem sa construim efectiv fisierul si sa turnam pe socket la client liniile, rand pe rand
    send(client_fd, "FROM ", 5, 0);
    send(client_fd, base_image, custom_len(base_image), 0);
    send(client_fd, "\nLABEL maintainer=\"", 19, 0);
    send(client_fd, maintainer, custom_len(maintainer), 0);
    send(client_fd, "\"\nWORKDIR ", 10, 0);
    send(client_fd, workdir, custom_len(workdir), 0);
    send(client_fd, "\n\n", 2, 0);

    for (int i = 0; i < env_count; i++) {
        send(client_fd, "ENV ", 4, 0); send(client_fd, envs[i], custom_len(envs[i]), 0);
        send(client_fd, "\n", 1, 0);
    }
    if (env_count > 0) send(client_fd, "\n", 1, 0);

    for (int i = 0; i < copy_count; i++) {
        send(client_fd, "COPY ", 5, 0); send(client_fd, copies[i], custom_len(copies[i]), 0);
        send(client_fd, "\n", 1, 0);
    }
    if (copy_count > 0) send(client_fd, "\n", 1, 0);

    if (dep_count > 0) {
        char *run_start = "RUN apt-get update && apt-get install -y \\\n";//asamblam efectiv fisierul
        send(client_fd, run_start, custom_len(run_start), 0); 
        
        for (int i = 0; i < dep_count; i++) {
            
            pid_t pid = fork(); // cream copilul pt pachetul curent
            
            if (pid == 0) {
                
                char version[64];
                version[0] = '\0'; 
                char *log_msg = "[MAP] Caut pachet prin intermediul curl\n";
                if(write(STDOUT_FILENO, log_msg, custom_len(log_msg)) < 0){
                    perror("Eroare la cautare pachet");
                }
                if (check_via_repology(deps[i], version, sizeof(version))) {
                    // Pachet gasit pe prima varianta, dam send urile ca sa construim linia de genul: "   curl \  # v8.5.0 (repology)"
                    send(client_fd, "    ", 4, 0);
                    send(client_fd, deps[i], custom_len(deps[i]), 0);
                    send(client_fd, " \\  # v", 7, 0);
                    send(client_fd, version, custom_len(version), 0);
                    send(client_fd, " (repology)\n", 12, 0);
                } 
                else if (check_via_homebrew(deps[i], version, sizeof(version))) {
                    // Gasit pe a doua varianta
                    send(client_fd, "    ", 4, 0);
                    send(client_fd, deps[i], custom_len(deps[i]), 0);
                    send(client_fd, " \\  # v", 7, 0);
                    send(client_fd, version, custom_len(version), 0);
                    send(client_fd, " (homebrew)\n", 12, 0);
                } 
                else {
                    // Nu este gasit, folosim custom_len sa nu mai trunchiem textul
                    char *fail_start = "    # ESUAT: ";
                    char *fail_end = " (Lipseste de pe sursele verificate)\n";
                    send(client_fd, fail_start, custom_len(fail_start), 0);
                    send(client_fd, deps[i], custom_len(deps[i]), 0);
                    send(client_fd, fail_end, custom_len(fail_end), 0);
                }
                
                exit(0); // copilul si-a incheiat munca
            }
            
            // parintele asteapta primul copil sa dea search sa scrie in socket, pe al doilea la fel si tot asa
            int status;
            waitpid(pid, &status, 0); 
            sleep(1); // Pauza obligatorie ca sa nu primim Rate Limit (429) de la Repology
        }
        
        // Toti copiii au terminat
        char *run_end = "    && apt-get clean \\\n    && rm -rf /var/lib/apt/lists/*\n\n";
        send(client_fd, run_end, custom_len(run_end), 0);
    }

    char *footer = "CMD [\"/bin/bash\"]\n";
    send(client_fd, footer, custom_len(footer), 0);

    // Daca am inchide conexiunea, terminalul clientului ar crapa si ar iesi din acesta asa ca lasam socketul deschis si trimitem un string de referinta pentru a stii cand s a terminat construirea dockerfile ului
    char *eof_marker = "\n===EOF===\n";
    send(client_fd, eof_marker, custom_len(eof_marker), 0);

    config_destroy(&cfg); // curatenie de memorie
}

int main() {
    // initializam serverul tcp, cod folosit si la teme anterioare
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 1;
    
    // reuseaddr ne scapa de erorile in care portul 8080 ramane blocat cand oprim brusc programul
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET; //adresa ipv4
    address.sin_addr.s_addr = INADDR_ANY;//poate primi de la orice adresa
    address.sin_port = htons(PORT);//htons transforma din int in limbaj de retea

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return 1;
    
    // pornim si asteptam o posibila coada mica de 1 om
    listen(server_fd, 1);

    curl_global_init(CURL_GLOBAL_DEFAULT);//initializam curl
    
    char msg_start[] = "[Server] Ascult pe port 8080...\n";
    if (write(STDOUT_FILENO, msg_start, custom_len(msg_start)) < 0) {
        perror("Eroare la pornire");
    }
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&address, &addrlen);//acceptam conexiunea de la client
        if (client_fd < 0) continue;

        if (write(STDOUT_FILENO, "[Server] Client conectat!\n", 26) < 0) {
            perror("Eroare la afisare conectare");
        }

        while (1) {//in aceasta bucla primim comenziile clientului
            char buffer[1024];
            for (int i = 0; i < 1024; i++) buffer[i] = '\0';
            
            ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            
            if (bytes_read <= 0) {
                if (write(STDOUT_FILENO, "[Server] Client deconectat.\n", 28) < 0) {
                    perror("Eroare la afisare deconectare");
                }
                break; 
            }
            
            process_request_and_send(client_fd, buffer);//construim efectiv dockerfile ul
        }
        close(client_fd);
    }

    close(server_fd);
    curl_global_cleanup();
    return 0;
}