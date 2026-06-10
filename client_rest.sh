#!/bin/bash
# Proiect PCD - Dockerfile Generator (Client REST Shell)
# Client shell pentru serviciile Web/REST ale serverului (Component 7 - tip diferit).
# Foloseste curl pentru comunicare HTTP.
#
# Rulare: bash client_rest.sh [comanda] [argumente]
#
# Comenzi:
#   status                          - afiseaza starea serverului
#   files                           - listeaza fisierele din uploads/
#   generate <dep1> [dep2 ...]      - genereaza Dockerfile cu dependintele date
#   demo                            - ruleaza un scenariu complet demonstrativ
#
# Variabile de mediu:
#   SERVER_HOST     (implicit: 127.0.0.1)
#   SERVER_WEB_PORT (implicit: 8082)

set -euo pipefail

# --- Configurare ---
HOST="${SERVER_HOST:-127.0.0.1}"
PORT="${SERVER_WEB_PORT:-8082}"
BASE="http://${HOST}:${PORT}"

# Incarca .env daca exista
if [ -f ".env" ]; then
    while IFS='=' read -r key value; do
        [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue
        export "$key"="${value}"
    done < .env
    HOST="${SERVER_HOST:-127.0.0.1}"
    PORT="${SERVER_WEB_PORT:-8082}"
    BASE="http://${HOST}:${PORT}"
fi

# --- Functii helper ---

check_curl() {
    if ! command -v curl &>/dev/null; then
        echo "[REST Shell] Eroare: curl nu este instalat." >&2
        exit 1
    fi
}

cmd_status() {
    echo "[REST Shell] GET ${BASE}/status"
    curl -s "${BASE}/status" | python3 -m json.tool 2>/dev/null \
        || curl -s "${BASE}/status"
    echo
}

cmd_files() {
    echo "[REST Shell] GET ${BASE}/files"
    curl -s "${BASE}/files" | python3 -m json.tool 2>/dev/null \
        || curl -s "${BASE}/files"
    echo
}

cmd_generate() {
    # Construim JSON body din argumentele date ca dependinte
    # Exemplu: ./client_rest.sh generate curl git wget
    local deps=("$@")
    local json_deps
    json_deps=$(printf '"%s",' "${deps[@]}")
    json_deps="[${json_deps%,}]"   # scoatem virgula finala

    local payload="{\"deps\":${json_deps},\"envs\":[],\"copies\":[]}"
    local out_file="Dockerfile.gen"

    echo "[REST Shell] POST ${BASE}/generate"
    echo "[REST Shell] Payload: ${payload}"
    echo

    curl -s -X POST "${BASE}/generate" \
        -H "Content-Type: application/json" \
        -d "${payload}" \
        -o "${out_file}"

    if [ -s "${out_file}" ]; then
        echo "[REST Shell] Dockerfile salvat in ${out_file}"
        echo "--- Continut ---"
        cat "${out_file}"
    else
        echo "[REST Shell] Eroare: raspuns gol de la server." >&2
    fi
}

cmd_demo() {
    echo "=============================="
    echo " Demo complet REST API"
    echo "=============================="
    echo

    echo "1) Status server:"
    cmd_status
    echo

    echo "2) Fisiere in uploads/:"
    cmd_files
    echo

    echo "3) Generare Dockerfile cu curl si git:"
    # Dezactivam set -e temporar ca sa nu iesim daca repology e lent
    set +e
    cmd_generate curl git
    set -e
    echo

    echo "============================== Demo incheiat."
}

# --- Main ---
check_curl

COMMAND="${1:-help}"
shift || true   # shift sigur chiar daca nu exista argumente

case "${COMMAND}" in
    status)
        cmd_status
        ;;
    files)
        cmd_files
        ;;
    generate)
        if [ $# -eq 0 ]; then
            echo "Utilizare: $0 generate <dep1> [dep2 ...]"
            exit 1
        fi
        cmd_generate "$@"
        ;;
    demo)
        cmd_demo
        ;;
    help|--help|-h)
        echo "Utilizare: $0 {status|files|generate|demo}"
        echo
        echo "  status              - stare server JSON"
        echo "  files               - lista fisiere uploads/"
        echo "  generate dep1 dep2  - genereaza Dockerfile"
        echo "  demo                - scenariu demonstrativ complet"
        ;;
    *)
        echo "[REST Shell] Comanda necunoscuta: ${COMMAND}" >&2
        echo "Utilizare: $0 {status|files|generate|demo}" >&2
        exit 1
        ;;
esac
