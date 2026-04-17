#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <getopt.h>
#include <curl/curl.h>
#include <libconfig.h>

// Funcție pentru a "simula" verificarea unei dependințe folosind libcurl
// În varianta finală, aici poți apela API-ul Docker Hub sau alt registru
int check_dependency_online(const char *dep_name) {
    CURL *curl;
    CURLcode res;
    int success = 0;

    curl = curl_easy_init();
    if(curl) {
        // Facem un request catre o adresa generica doar pentru demo
        // In realitate ai construi un URL bazat pe dep_name
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/status/200");
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // Doar HEAD request, nu tragem tot body-ul
        
        // Ascundem output-ul curl de pe ecran
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            success = 1; // Pachetul "exista"
        } else {
            fprintf(stderr, "[Copil] Eroare curl pentru %s: %s\n", dep_name, curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }
    return success;
}

int main(int argc, char **argv) {
    int opt;
    char *output_file = "Dockerfile.gen"; // Nume default
    char deps[10][64]; // Maxim 10 dependinte pt demo
    int dep_count = 0;

    // 1. Preluarea informatiilor de mediu
    char *user = getenv("USER");
    if (!user) user = "unknown_user";
    printf("[Info] Rulat de utilizatorul sistemului: %s\n", user);

    // 2. Parsarea argumentelor de pe linia de comanda
    struct option long_options[] = {
        {"dep", required_argument, 0, 'd'},
        {"out", required_argument, 0, 'o'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "d:o:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                if (dep_count < 10) {
                    strncpy(deps[dep_count], optarg, 63);
                    deps[dep_count][63] = '\0';
                    dep_count++;
                }
                break;
            case 'o':
                output_file = optarg;
                break;
            default:
                fprintf(stderr, "Folosire: %s --dep <pachet1> --dep <pachet2> --out <fisier>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (dep_count == 0) {
        printf("[Info] Nicio dependinta adaugata. Folositi --dep <nume>\n");
        return 0;
    }

    // 3. Citirea configurarii folosind libconfig
    config_t cfg;
    config_init(&cfg);

    if (!config_read_file(&cfg, "demo.cfg")) {
        fprintf(stderr, "Eroare libconfig: %s la linia %d\n", 
                config_error_text(&cfg), config_error_line(&cfg));
        config_destroy(&cfg);
        return EXIT_FAILURE;
    }

    const char *base_image, *maintainer, *workdir;
    if (!config_lookup_string(&cfg, "container.base_image", &base_image)) base_image = "ubuntu:latest";
    if (!config_lookup_string(&cfg, "container.maintainer", &maintainer)) maintainer = user;
    if (!config_lookup_string(&cfg, "container.workdir", &workdir)) workdir = "/tmp";

    // Initializeaza mediul curl global inainte de fork
    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("--- Incepere generare Dockerfile (Map-Reduce Demo) ---\n");
    pid_t pids[10];
    int valid_deps[10] = {0}; // 1 daca copilul confirma dependinta, 0 altfel

    // 4. Map-Reduce (Fork/Wait) pentru verificarea asincrona a dependintelor
    for (int i = 0; i < dep_count; i++) {
        pids[i] = fork();
        
        if (pids[i] < 0) {
            perror("Eroare la fork");
            exit(EXIT_FAILURE);
        }
        
        if (pids[i] == 0) { // Proces copil
            printf("[Copil %d] Verific dependinta pe retea: %s\n", getpid(), deps[i]);
            int is_valid = check_dependency_online(deps[i]);
            
            // Returnam 0 ca exit code daca e valid, 1 daca a esuat
            exit(is_valid ? EXIT_SUCCESS : EXIT_FAILURE); 
        }
    }

    // Proces Parinte: Colecteaza rezultatele (Reduce)
    for (int i = 0; i < dep_count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS) {
            valid_deps[i] = 1; // Verificare cu succes
            printf("[Parinte] Dependinta '%s' a fost validata.\n", deps[i]);
        } else {
            printf("[Parinte] AVERTISMENT: Dependinta '%s' nu a putut fi validata!\n", deps[i]);
        }
    }

    // 5. Asamblarea si scrierea Dockerfile-ului (Folosind apeluri Low-Level)
    int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Eroare la crearea fisierului de output");
        config_destroy(&cfg);
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    char buffer[1024];
    int len;

    // Scriere header
    len = snprintf(buffer, sizeof(buffer), 
                   "FROM %s\nLABEL maintainer=\"%s\"\nWORKDIR %s\n\nRUN apt-get update && apt-get install -y \\\n", 
                   base_image, maintainer, workdir);
    write(fd, buffer, len);

    // Scriere dependinte validate
    for (int i = 0; i < dep_count; i++) {
        if (valid_deps[i]) {
            len = snprintf(buffer, sizeof(buffer), "    %s \\\n", deps[i]);
            write(fd, buffer, len);
        }
    }

    // Cleanup final Dockerfile
    len = snprintf(buffer, sizeof(buffer), "    && rm -rf /var/lib/apt/lists/*\n\nCMD [\"/bin/bash\"]\n");
    write(fd, buffer, len);

    close(fd);
    config_destroy(&cfg);
    curl_global_cleanup();

    printf("--- Finalizat! Fisierul '%s' a fost generat. ---\n", output_file);
    return EXIT_SUCCESS;
}