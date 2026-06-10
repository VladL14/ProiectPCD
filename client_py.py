#!/usr/bin/env python3
"""
Proiect PCD - Dockerfile Generator (Client Python)
Client ordinar in limbaj alternativ (Nivel B - componenta 4 obligatorie).
Ofera aceeasi interfata ca si client.c: --dep, --env, --copy, --out, --upload, --get.
"""

import socket
import sys
import os


def receive_until_eof(sock: socket.socket) -> str:
    """Primeste date de la server pana la marcajul ===EOF===."""
    data = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if b"===EOF===" in data:
            idx = data.index(b"===EOF===")
            return data[:idx].decode("utf-8", errors="replace")
    return data.decode("utf-8", errors="replace")


def load_env_file(path: str) -> None:
    """Incarca variabilele din fisierul .env in mediul procesului curent."""
    try:
        with open(path) as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, _, value = line.partition("=")
                    os.environ.setdefault(key.strip(), value.strip())
    except FileNotFoundError:
        pass


def main() -> None:
    load_env_file(".env")

    host = os.environ.get("SERVER_HOST", "127.0.0.1")
    port = int(os.environ.get("SERVER_PORT", "8080"))

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((host, port))
    except ConnectionRefusedError:
        print(f"[Client Python] Eroare: nu ma pot conecta la {host}:{port}")
        sys.exit(1)

    print(f"[Client Python] Conectat la server {host}:{port}")

    while True:
        try:
            line = input("comanda:> ").strip()
        except EOFError:
            break

        if not line:
            continue
        if line == "exit":
            break

        tokens = line.split()
        payload_parts: list[str] = []
        output_file = "Dockerfile.gen"
        upload_path = ""
        get_filename = ""
        do_list = False
        delete_filename = ""

        i = 0
        while i < len(tokens):
            tok = tokens[i]
            if tok == "--dep" and i + 1 < len(tokens):
                i += 1
                payload_parts.append(f"D:{tokens[i]}")
            elif tok == "--env" and i + 1 < len(tokens):
                i += 1
                payload_parts.append(f"E:{tokens[i]}")
            elif tok == "--copy" and i + 1 < len(tokens):
                i += 1
                payload_parts.append(f"C:{tokens[i]}")
            elif tok == "--out" and i + 1 < len(tokens):
                i += 1
                output_file = tokens[i]
            elif tok == "--upload" and i + 1 < len(tokens):
                i += 1
                upload_path = tokens[i]
            elif tok == "--get" and i + 1 < len(tokens):
                i += 1
                get_filename = tokens[i]
            elif tok == "--list":
                do_list = True
            elif tok == "--delete" and i + 1 < len(tokens):
                i += 1
                delete_filename = tokens[i]
            i += 1

        # --- upload: trimitem un fisier local catre server ---
        if upload_path:
            try:
                file_size = os.path.getsize(upload_path)
            except OSError as e:
                print(f"[Client Python] Eroare stat: {e}")
                continue

            basename = os.path.basename(upload_path)
            header = f"UPLOAD:{basename}:{file_size}".encode()
            sock.sendall(header)

            try:
                with open(upload_path, "rb") as f:
                    while True:
                        chunk = f.read(4096)
                        if not chunk:
                            break
                        sock.sendall(chunk)
            except OSError as e:
                print(f"[Client Python] Eroare la citire fisier: {e}")
                continue

            resp = receive_until_eof(sock)
            print(f"[Client Python] {resp.strip()}")
            continue

        # --- download: cerem un fisier de pe server ---
        if get_filename:
            sock.sendall(f"DOWNLOAD:{get_filename}".encode())
            data = receive_until_eof(sock)
            if data.startswith("ERR:"):
                print(f"[Client Python] {data.strip()}")
            else:
                try:
                    with open(get_filename, "w") as f:
                        f.write(data)
                    print(f"[Client Python] Fisier descarcat: {get_filename}")
                except OSError as e:
                    print(f"[Client Python] Eroare la salvare: {e}")
            continue

        # --- list: lista fisierelor din uploads/ ---
        if do_list:
            sock.sendall(b"LIST")
            listing = receive_until_eof(sock)
            print("[Client Python] Fisiere pe server:")
            print(listing.strip() if listing.strip() else "  (director gol)")
            continue

        # --- delete: stergem un fisier de pe server ---
        if delete_filename:
            sock.sendall(f"DELETE:{delete_filename}".encode())
            resp = receive_until_eof(sock)
            print(f"[Client Python] {resp.strip()}")
            continue

        # --- generare Dockerfile ---
        if not payload_parts:
            print("[Client Python] Comanda invalida. Folositi --dep x --env y --copy z --list --delete f")
            continue

        payload = " ".join(payload_parts)
        sock.sendall(payload.encode())

        dockerfile_content = receive_until_eof(sock)
        try:
            with open(output_file, "w") as f:
                f.write(dockerfile_content)
            print(f"[Client Python] Dockerfile salvat in {output_file}")
        except OSError as e:
            print(f"[Client Python] Eroare la salvare Dockerfile: {e}")

    sock.close()


if __name__ == "__main__":
    main()
