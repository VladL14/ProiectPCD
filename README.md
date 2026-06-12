# 🐳 Dockerfile Generator - PCD Project

A client-server application developed in C (UNIX/Linux) that enables the automatic generation, configuration, and assembly of Dockerfiles. The system resolves dependencies by querying external APIs (Repology/Homebrew) and supports bidirectional file transfers within a highly concurrent, multiplexed architecture.

---

## 🏗️ System Architecture & Features

The project is structured into multiple threads and modules, covering advanced system programming concepts (Level A/B/C requirements).

### 1. TCP/UDP Server (C/UNIX)

- **I/O Multiplexing** using `poll()` for handling multiple concurrent TCP client connections.
- **Processing Queue** implemented with an anonymous pipe-based FIFO queue to synchronize the I/O thread with the processing worker thread.
- **Directory Monitoring** through `inotify` for real-time tracking of changes in the uploads directory.
- **Concurrency** achieved using `pthread` threads, mutexes, and thread-safe logging.

### 2. Administration Client (UDP/C)

- Interactive terminal interface built with `ncurses`.
- **Exclusive Session Management** with a strict 1:1 administrator lock and a 60-second inactivity timeout.
- **Real-Time Analytics** displaying:
  - Average request processing time
  - Total generated Dockerfiles
  - Active client connections
- **Access Control** features:
  - Force disconnect (Kick)
  - IP ban management (Ban List)

### 3. Standard Clients (TCP)

#### C Client

- Custom command-line interface.
- Supports bidirectional file transfers.
- Chunked upload/download streaming.

#### Python Client

- Alternative implementation (`client_py.py`).
- Demonstrates cross-language interoperability.

### 4. Web / REST API (HTTP)

- Dedicated HTTP server thread running on port `8082`.
- REST endpoints for:
  - Server status monitoring
  - Dockerfile generation via JSON requests
- Includes Python and Bash scripts for API testing.

---

## ⚙️ Prerequisites

The following tools and libraries are required on a Linux/Ubuntu environment:

- `build-essential` (gcc, make)
- `cmake` (version 3.23 or newer)
- `conan` (version 2.x)
- `libncurses5-dev`

---

## 🚀 Build Instructions

The project uses **Conan** for dependency management (`libconfig`, `libcurl`) and **CMake** as the build system.

### 1. Install Dependencies

```bash
conan install . --build=missing
```

### 2. Configure the Project

```bash
cmake --preset conan-release
```

### 3. Build the Executables

```bash
cmake --build build --preset conan-release
```

---

## 💻 Usage

After a successful build, the compiled binaries will be available in:

```text
build/Release/
```

Make sure the `.env` and `demo.cfg` files are present in the working directory before starting the application.

---

### 1. Start the Server

```bash
./build/Release/server
```

The server listens on:

| Port | Protocol | Purpose |
|--------|----------|----------|
| 8080 | TCP | Standard Clients |
| 8081 | UDP | Administration Client |
| 8082 | HTTP | REST API |

---

### 2. Launch the Admin Panel

Open a separate terminal and run:

```bash
./build/Release/admin
```

Controls:

- Arrow Keys → Navigate menu
- Enter → Execute action

To allow another administrator to connect, select **Logout**.

---

### 3. Standard Client (Interactive CLI)

```bash
./build/Release/client
```

Available commands inside the `comanda:>` prompt:

#### Generate Dockerfile

```text
--dep curl --dep git --env PORT=80 --copy file.txt
```

#### Upload File

```text
--upload ./path/to/local/file
```

#### Download File

```text
--get file_on_server.txt
```

#### List Uploaded Files

```text
--list
```

#### Exit Client

```text
exit
```

---

### 4. REST API Client

```bash
python3 client_rest.py
```

Available commands inside the `rest:>` prompt:

#### Server Status

```text
status
```

Returns server statistics in JSON format.

#### List Files

```text
files
```

Returns the list of uploaded files.

#### Generate Dockerfile

```text
generate --dep git --dep wget --env MODE=prod
```

Generates a Dockerfile through the REST API.

---

## 📂 Project Components

| Component | Description |
|------------|------------|
| `server` | Main TCP/UDP/HTTP server |
| `admin` | Administrator dashboard |
| `client` | Interactive TCP client |
| `client_py.py` | Python TCP client |
| `client_rest.py` | REST API client |
| `uploads/` | Uploaded files directory |
| `.env` | Environment configuration |
| `demo.cfg` | Server configuration |

---

## 🔧 Technologies Used

- C (POSIX / UNIX)
- Python
- TCP / UDP Sockets
- HTTP REST API
- `poll()`
- `pthread`
- `inotify`
- `ncurses`
- Conan
- CMake
- libcurl
- libconfig