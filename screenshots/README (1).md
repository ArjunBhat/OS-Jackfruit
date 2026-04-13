# OS-Jackfruit: Multi-Container Runtime

**Team Size:** 2 Students

## Team Information

- **Arjun Bhat** — **PES1UG24CS655**
- **Partner Name** — **Partner SRN**

> If you are submitting solo, remove the second line above.

## Project Overview

OS-Jackfruit is a lightweight Linux container runtime implemented in C. It has two connected parts:

1. **User-space runtime and supervisor (`engine.c`)**  
   A long-running supervisor manages multiple containers, exposes a small CLI, captures logs with a bounded-buffer pipeline, tracks metadata, and handles lifecycle signals correctly.

2. **Kernel-space memory monitor (`monitor.c`)**  
   A Linux kernel module tracks registered container processes, periodically checks RSS usage, warns on soft-limit breaches, and kills processes on hard-limit breaches.

The project demonstrates core operating systems concepts: namespaces, `chroot`, `clone()`, UNIX domain socket IPC, producer-consumer synchronization, kernel monitoring, and scheduling experiments.

---

## Repository Layout

```text
OS-Jackfruit/
├── boilerplate/
│   ├── engine.c
│   ├── monitor.c
│   ├── monitor_ioctl.h
│   └── Makefile
├── workloads/
│   ├── memory_hog
│   └── cpu_hog
├── screenshots/
├── README.md
└── project-guide.md
```

> The `rootfs-base`, `rootfs-alpha`, and `rootfs-beta` directories are runtime artifacts. They should be created locally for testing, but they do not need to be committed to GitHub.

---

## Environment

This project is designed for:

- Ubuntu 22.04 or 24.04 in a VM
- Secure Boot **off**
- No WSL

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## Build Instructions

From the repository root:

```bash
make -C boilerplate ci
make -C boilerplate module
```

The `ci` target is a user-space smoke build and should work without `sudo`, kernel module loading, or rootfs setup.

To rebuild from scratch:

```bash
cd boilerplate
make clean
make
```

---

## Prepare the Root Filesystems

Create the base Alpine root filesystem and two writable copies:

```bash
cd ~/OS-Jackfruit
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

If you want to run a workload binary inside a container, copy it into that container rootfs before launch:

```bash
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/memory_hog rootfs-alpha/
```

---

## Load the Kernel Module

From the repository root:

```bash
cd boilerplate
sudo insmod monitor.ko
ls /dev/container_monitor
```

You should see:

```text
/dev/container_monitor
```

To unload later:

```bash
sudo rmmod monitor
```

---

## Run the Supervisor

Open **Terminal 1** and start the supervisor:

```bash
cd ~/OS-Jackfruit/boilerplate
sudo ./engine supervisor ../rootfs-base
```

The supervisor stays alive and manages containers through a UNIX domain socket.

---

## CLI Contract

Use this exact interface:

```bash
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

### Semantics

- `start` launches a container in the background and returns after the supervisor accepts the request.
- `run` launches a container and blocks until it exits.
- `ps` lists tracked containers and metadata.
- `logs` prints the container log file.
- `stop` requests graceful termination.

Defaults:

- soft limit = **40 MiB**
- hard limit = **64 MiB**

The container rootfs must be unique for each live container.

---

## Example Usage

### Start two containers

Open **Terminal 2**:

```bash
cd ~/OS-Jackfruit/boilerplate
sudo ./engine start alpha ../rootfs-alpha /bin/sleep 120
sudo ./engine start beta ../rootfs-beta /bin/sleep 120
sudo ./engine ps
```

### Run a foreground container

```bash
sudo ./engine run logtest ../rootfs-alpha /bin/echo hello
sudo ./engine logs logtest
```

### Stop a container

```bash
sudo ./engine stop alpha
sudo ./engine ps
```

---

## Architecture

### 1) Supervisor and container lifecycle

The supervisor is a long-running parent process. It keeps container metadata in memory, creates containers with `clone()`, handles `SIGCHLD`, and reaps children to avoid zombies.

Each container tracks:

- container ID
- host PID
- start time
- state
- soft limit
- hard limit
- nice value
- log file path
- exit code / signal
- `stop_requested`

### 2) Namespace isolation

Each container uses:

- PID namespace
- UTS namespace
- mount namespace

Filesystem isolation is implemented with `chroot()` or `pivot_root()`, then `/proc` is mounted inside the container so tools like `ps` work.

### 3) IPC design

There are two separate IPC paths:

- **Control path:** CLI client ↔ supervisor over a UNIX domain socket
- **Logging path:** container stdout/stderr → pipes → supervisor logging pipeline

This keeps command handling separate from log capture.

### 4) Logging pipeline

Container output is captured through a bounded-buffer producer-consumer design:

- producer thread reads from container pipe
- shared bounded buffer stores log data
- consumer thread writes to per-container log files

Synchronization uses:

- mutex
- condition variables

This prevents race conditions, dropped output, and deadlock when the buffer fills.

### 5) Kernel memory monitor

The kernel module registers container host PIDs via `ioctl`, stores them in a linked list, and periodically checks RSS. It prints a warning when a process first crosses the soft limit and sends `SIGKILL` when the hard limit is exceeded.

---

## Memory Enforcement

The kernel module is responsible for memory enforcement because RSS accounting and process termination must be done by the kernel safely and accurately.

Behavior:

- **Soft limit:** warning only
- **Hard limit:** terminate the process with `SIGKILL`

The supervisor records the final reason as:

- `normal_exit`
- `manually_stopped`
- `hard_limit_killed`

---

## Scheduling Experiment

The runtime supports `--nice` to adjust CPU scheduling priority.

Example:

```bash
sudo ./engine start cpu1 ../rootfs-alpha --nice -5 /cpu_hog
sudo ./engine start cpu2 ../rootfs-beta --nice 10 /cpu_hog
sudo ./engine ps
```

The `ps` output shows the configured nice values, and the workloads can be compared using completion time or order of completion.

---

## Clean Teardown

The supervisor:

- reaps children correctly
- closes file descriptors
- joins logging threads
- frees metadata
- unregisters kernel entries

A clean `ps aux | grep defunct` should not show leftover zombies.

---

## Screenshots

Place annotated screenshots in `screenshots/` and include a short caption for each one in this section.

### 1. Multi-container supervision

**File:** `screenshots/supervision.png`  
Shows two containers running at the same time under one supervisor.

### 2. Metadata tracking

**File:** `screenshots/supervision.png`  
Shows `engine ps` output with live metadata for multiple containers.

### 3. Logging pipeline

**File:** `screenshots/logging.png`  
Shows `engine logs <id>` output with data captured from the container.

### 4. CLI and IPC

**File:** `screenshots/ipc.png`  
Shows `engine stop <id>` and the container state changing to `manually_stopped`.

### 5. Soft-limit warning

**File:** `screenshots/soft_limit.png`  
Shows `dmesg` output with a soft-limit warning from `container_monitor`.

### 6. Hard-limit enforcement

**File:** `screenshots/hard_limit.png`  
Shows `engine ps` and `dmesg` output indicating `hard_limit_killed`.

### 7. Scheduling experiment

**File:** `screenshots/scheduling.png`  
Shows two CPU-bound workloads with different `--nice` values.

### 8. Clean teardown

**File:** `screenshots/cleanup.png`  
Shows that no zombie processes remain after shutdown.

---

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime uses Linux namespaces to isolate processes and the filesystem. PID namespaces create separate process trees, UTS namespaces isolate hostname-related state, and mount namespaces isolate filesystem mounts. `chroot()` or `pivot_root()` changes the root filesystem view so the container sees its own rootfs as `/`. The host kernel is still shared, but container-visible process and mount state are isolated.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is useful because it can manage multiple containers at once, keep metadata for each container, and reap children reliably. This avoids zombies and makes it possible to distinguish normal exit, manual stop, and hard-limit termination.

### 3. IPC, Threads, and Synchronization

The project uses two IPC mechanisms: a UNIX domain socket for control commands and pipes for log capture. The logging path uses a bounded buffer with mutexes and condition variables so producers and consumers do not race, block incorrectly, or lose logs. The supervisor’s shared metadata table is protected separately from the log buffer.

### 4. Memory Management and Enforcement

RSS measures resident physical memory currently mapped in RAM. It does not measure everything a process has allocated conceptually, and it can differ from virtual memory usage. Soft and hard limits are different policies: soft limit warns, hard limit terminates. Kernel-space enforcement is appropriate because only the kernel has accurate process memory visibility and the authority to terminate a process safely.

### 5. Scheduling Behavior

The `--nice` experiment shows how Linux scheduler priority affects CPU allocation. Lower nice values should receive higher priority, which can change completion time or responsiveness when two CPU-bound workloads compete. This demonstrates fairness and throughput tradeoffs in the scheduler.

---

## Design Decisions and Tradeoffs

### Namespace isolation
- **Choice:** `clone()` with PID, UTS, and mount namespaces
- **Tradeoff:** simpler than a full container stack, but still enough to demonstrate core isolation concepts

### IPC and control
- **Choice:** UNIX domain socket
- **Tradeoff:** local-only communication, but simple and appropriate for a supervisor running on the same host

### Logging
- **Choice:** bounded-buffer producer-consumer pipeline
- **Tradeoff:** more code than writing directly to files, but much safer and better for concurrent workloads

### Kernel monitor
- **Choice:** periodic RSS checks in a kernel module
- **Tradeoff:** more complex than user-space polling, but it gives direct control over memory enforcement

### Scheduling experiment
- **Choice:** `--nice`
- **Tradeoff:** less granular than CPU affinity tuning, but easy to demonstrate and explain

---

## Verification Commands

### Multi-container supervision

```bash
sudo ./engine start alpha ../rootfs-alpha /bin/sleep 120
sudo ./engine start beta ../rootfs-beta /bin/sleep 120
sudo ./engine ps
```

### Logging

```bash
sudo ./engine run logtest ../rootfs-alpha /bin/echo hello
sudo ./engine logs logtest
```

### Stop attribution

```bash
sudo ./engine start stoptest ../rootfs-alpha /bin/sleep 120
sudo ./engine stop stoptest
sudo ./engine ps
```

### Soft-limit warning

```bash
cp memory_hog ../rootfs-alpha/
sudo ./engine start softtest ../rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 200
sudo dmesg | tail
```

### Hard-limit enforcement

```bash
sudo ./engine start hardtest ../rootfs-alpha /memory_hog --hard-mib 16
sudo ./engine ps
sudo dmesg | tail
```

### Scheduling

```bash
sudo ./engine start cpu1 ../rootfs-alpha --nice -5 /cpu_hog
sudo ./engine start cpu2 ../rootfs-beta --nice 10 /cpu_hog
sudo ./engine ps
```

### Cleanup

```bash
ps aux | grep defunct
sudo rmmod monitor
```

---

## Results Summary

The implementation demonstrates:

- multi-container supervision
- correct command handling through a supervisor
- producer-consumer logging
- soft and hard memory enforcement
- scheduler priority propagation using `--nice`
- clean teardown with no zombies

---

## Notes

- Rootfs directories are intentionally kept out of GitHub because they are runtime artifacts.
- `make -C boilerplate ci` is the CI-safe smoke test target.
- `make -C boilerplate module` builds the kernel module for the local VM environment.

