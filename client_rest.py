#!/usr/bin/env python3
"""
Proiect PCD - Dockerfile Generator (Client REST Python)
Client pentru serviciile Web/REST expuse de server (Component 6 - uz ordinar).
Foloseste doar stdlib (urllib), fara dependente externe.

Rulare: python3 client_rest.py
Portul HTTP al serverului: 8082 (implicit) sau SERVER_WEB_PORT din .env
"""

import json
import sys
import os
import urllib.request
import urllib.error


def load_env_file(path: str) -> None:
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


def api_get(base: str, path: str) -> dict | str:
    url = f"{base}{path}"
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            body = resp.read().decode()
            ct = resp.headers.get("Content-Type", "")
            if "json" in ct:
                return json.loads(body)
            return body
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}"}
    except urllib.error.URLError as e:
        print(f"[REST Client] Eroare conexiune: {e.reason}")
        sys.exit(1)


def api_post(base: str, path: str, payload: dict) -> str:
    url  = f"{base}{path}"
    data = json.dumps(payload).encode()
    req  = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return resp.read().decode()
    except urllib.error.HTTPError as e:
        return f"Eroare HTTP {e.code}: {e.read().decode()}"
    except urllib.error.URLError as e:
        print(f"[REST Client] Eroare conexiune: {e.reason}")
        sys.exit(1)


def cmd_status(base: str) -> None:
    result = api_get(base, "/status")
    print("[Status server]")
    if isinstance(result, dict):
        for k, v in result.items():
            print(f"  {k}: {v}")
    else:
        print(result)


def cmd_files(base: str) -> None:
    result = api_get(base, "/files")
    print("[Fisiere in uploads/]")
    if isinstance(result, dict) and "files" in result:
        files = result["files"]
        if not files:
            print("  (director gol)")
        for f in files:
            print(f"  - {f}")
    else:
        print(result)


def cmd_generate(base: str, deps: list[str], envs: list[str],
                 copies: list[str], out_file: str) -> None:
    payload = {"deps": deps, "envs": envs, "copies": copies}
    print(f"[REST Client] Trimit cerere /generate cu {len(deps)} dep(s)...")
    dockerfile = api_post(base, "/generate", payload)
    with open(out_file, "w") as f:
        f.write(dockerfile)
    print(f"[REST Client] Dockerfile salvat in {out_file}")


def print_help() -> None:
    print(
        "Comenzi disponibile:\n"
        "  status                          — stare server\n"
        "  files                           — lista fisiere din uploads/\n"
        "  generate --dep X --env Y --out Z — genereaza Dockerfile\n"
        "  exit                            — iesire\n"
    )


def main() -> None:
    load_env_file(".env")
    host = os.environ.get("SERVER_HOST", "127.0.0.1")
    port = int(os.environ.get("SERVER_WEB_PORT", "8082"))
    base = f"http://{host}:{port}"

    print(f"[REST Client] Conectat la {base}")
    print_help()

    while True:
        try:
            line = input("rest:> ").strip()
        except EOFError:
            break

        if not line:
            continue
        if line in ("exit", "quit"):
            break
        if line == "help":
            print_help()
            continue

        tokens = line.split()
        cmd    = tokens[0]

        if cmd == "status":
            cmd_status(base)

        elif cmd == "files":
            cmd_files(base)

        elif cmd == "generate":
            deps, envs, copies = [], [], []
            out_file = "Dockerfile.gen"
            i = 1
            while i < len(tokens):
                if tokens[i] == "--dep" and i + 1 < len(tokens):
                    i += 1; deps.append(tokens[i])
                elif tokens[i] == "--env" and i + 1 < len(tokens):
                    i += 1; envs.append(tokens[i])
                elif tokens[i] == "--copy" and i + 1 < len(tokens):
                    i += 1; copies.append(tokens[i])
                elif tokens[i] == "--out" and i + 1 < len(tokens):
                    i += 1; out_file = tokens[i]
                i += 1
            if not deps and not envs and not copies:
                print("Folositi: generate --dep X --env Y=Z --out fisier")
            else:
                cmd_generate(base, deps, envs, copies, out_file)

        else:
            print(f"Comanda necunoscuta: {cmd}. Tastati 'help' pentru ajutor.")


if __name__ == "__main__":
    main()
