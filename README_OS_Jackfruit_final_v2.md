# OS-Jackfruit Runtime

## Team Information

Arjun Bhat — PES1UG24CS655

---

## Project Overview

OS-Jackfruit is a lightweight multi-container runtime implemented in C that demonstrates core operating system concepts including:

- namespace isolation using `clone()`
- filesystem isolation using `chroot()`
- process lifecycle supervision
- UNIX domain socket IPC
- bounded-buffer logging pipeline
- kernel-space RSS monitoring
- soft-limit warnings and hard-limit enforcement
- scheduler behavior experimentation using `nice` values
- clean teardown guarantees without zombie processes

The runtime consists of a long-running supervisor process that manages containers, a CLI interface for control operations, and a kernel module that enforces memory limits.

---

## Build Instructions

Compile user-space runtime:

```bash
make -C boilerplate ci
```

Compile kernel module:

```bash
make -C boilerplate module
```

Load kernel module:

```bash
sudo insmod boilerplate/monitor.ko
```

Verify the control device:

```bash
ls /dev/container_monitor
```

---

## Root Filesystem Setup

Create the Alpine base root filesystem:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create writable container copies:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

Copy workloads if required:

```bash
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/cpu_hog rootfs-beta/
cp boilerplate/memory_hog rootfs-alpha/
```

---

## Running the Supervisor

Terminal 1:

```bash
cd ~/OS-Jackfruit/boilerplate
sudo ./engine supervisor ../rootfs-base
```

The supervisor listens on:

```text
/tmp/jackfruit.sock
```

---

## CLI Contract

Use this exact interface:

```bash
engine supervisor <base-rootfs>
engine start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

### Semantics

- `start` launches a container in the background and returns after the supervisor accepts the request.
- `run` launches a container and blocks until it exits.
- `ps` lists tracked containers and metadata.
- `logs` prints the per-container log file.
- `stop` requests graceful termination.

Defaults:

- soft limit = `40 MiB`
- hard limit = `64 MiB`

Each live container must use its own writable rootfs directory.

---

## Example Usage

### Start two containers

```bash
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

### 1) Supervisor and Process Lifecycle

The supervisor is a long-running parent process that manages multiple containers at once. It stores metadata for each container, launches them using `clone()`, handles `SIGCHLD`, and reaps children so no zombies remain.

Container metadata includes:

- container ID
- host PID
- start time
- current state
- configured soft and hard limits
- nice value
- log file path
- exit status or terminating signal
- `stop_requested` flag

### 2) Namespace Isolation

Each container is isolated with:

- PID namespace
- UTS namespace
- mount namespace

Filesystem isolation is done with `chroot()` or `pivot_root()`, and `/proc` is mounted inside the container so tools like `ps` work correctly.

### 3) IPC Design

There are two separate IPC paths:

- **Control path:** CLI client ↔ supervisor using a UNIX domain socket
- **Logging path:** container stdout/stderr → supervisor through pipes

This separation keeps command handling independent from log capture.

### 4) Logging Pipeline

Container output is captured using a bounded-buffer producer-consumer design:

```text
container stdout/stderr
        ↓
producer thread
        ↓
bounded buffer
        ↓
consumer thread
        ↓
per-container log file
```

Synchronization uses:

- mutex
- condition variables

### Race conditions prevented

Without synchronization:

- two producer threads could write to the same buffer slot at the same time
- a consumer could read a partially written log entry
- unread entries could be overwritten when the buffer wraps
- a producer could overwrite unread data when the buffer is full

Without condition variables:

- producers could spin or overwrite unread entries
- consumers could read from an empty buffer
- shutdown ordering could break and lose tail logs

Mutexes and condition variables prevent corruption, loss, and deadlock.

### 5) Kernel Memory Monitor

The kernel module registers container PIDs via `ioctl`, stores them in a linked list, and periodically checks RSS. It warns on soft-limit violations and kills a process with `SIGKILL` when the hard limit is exceeded.

---

## Memory Enforcement

RSS measures the amount of resident physical memory currently in RAM. It does not include everything a process has mapped or allocated conceptually.

Behavior:

- **Soft limit:** warning only
- **Hard limit:** terminate the process

The supervisor records the final reason as:

- `normal_exit`
- `manually_stopped`
- `hard_limit_killed`

---

## Scheduler Experiment

The runtime supports `--nice` to adjust scheduling priority.

Example:

```bash
sudo ./engine start cpu1 ../rootfs-alpha --nice -5 /cpu_hog
sudo ./engine start cpu2 ../rootfs-beta --nice 10 /cpu_hog
sudo ./engine ps
```

The metadata shows the configured nice values and the scheduler experiment can be compared using these priorities.

---

## Scheduler Experiment Results

Two CPU-bound workloads were launched concurrently with different priorities.

Observed concurrent run:

- `fast` used `--nice -5`
- `slow` used `--nice 10`

Observed completion order:

| Container | Nice | Observed Result |
|-----------|------|-----------------|
| fast | -5 | finished first |
| slow | 10 | finished second |

### Interpretation

Linux’s Completely Fair Scheduler allocates CPU proportionally based on nice values. Lower nice values receive greater CPU share, demonstrating priority-based scheduling behavior.

The observed result confirms that the higher-priority container (`nice = -5`) completed before the lower-priority container (`nice = 10`) during concurrent execution, which is the expected outcome when both workloads contend for CPU time.

---

## Clean Teardown Guarantees

The supervisor ensures:

- SIGCHLD reaping
- descriptor cleanup
- thread joins
- metadata cleanup
- kernel unregister calls

Verification:

```bash
ps aux | grep defunct
```

No zombie processes should remain.

---

## Design Decisions and Tradeoffs

### Namespace Isolation

**Choice:** `clone()` with PID, UTS, and mount namespaces  
**Tradeoff:** This is simpler than a full container stack, but it does not add network namespace isolation.  
**Justification:** The selected namespaces provide the exact isolation concepts needed for the assignment while keeping implementation complexity manageable.

### IPC Control Channel

**Choice:** UNIX domain socket  
**Tradeoff:** It is local-only communication and cannot be used across hosts.  
**Justification:** The supervisor and CLI always run on the same machine, so a UNIX socket gives lower overhead and a simpler design than TCP.

### Logging Pipeline

**Choice:** bounded-buffer producer-consumer threads  
**Tradeoff:** This requires explicit synchronization logic.  
**Justification:** The design prevents log corruption and blocking writes while preserving output ordering and correctness under concurrent load.

### Kernel Monitor

**Choice:** timer-based RSS enforcement in a kernel module  
**Tradeoff:** This is more complex than user-space polling.  
**Justification:** The kernel is the correct place for accurate memory accounting and safe termination authority.

### Scheduling Experiment

**Choice:** `nice`-based priority control  
**Tradeoff:** It is coarser than CPU affinity or custom scheduler tuning.  
**Justification:** `nice` directly interacts with Linux’s scheduler, is easy to demonstrate, and clearly shows priority-based CPU allocation behavior.

---

## Screenshots Included

Each screenshot should be annotated with a short caption in the repository.

| Feature | Screenshot |
|---------|------------|
| Multi-container supervision | `screenshots/multicontainer.png` |
| Metadata tracking | `screenshots/metadata.png` |
| Logging pipeline | `screenshots/logging.png` |
| CLI IPC stop attribution | `screenshots/ipc.png` |
| Soft-limit warning | `screenshots/soft_limit.png` |
| Hard-limit enforcement | `screenshots/hard_limit.png` |
| Scheduling experiment | `screenshots/scheduling.png` |
| Clean teardown | `screenshots/cleanup.png` |

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

This runtime demonstrates:

- concurrent container supervision
- namespace isolation
- CLI IPC control channel
- bounded-buffer logging pipeline
- kernel RSS monitoring
- soft/hard memory enforcement
- scheduler priority propagation
- clean teardown without zombies
- correct metadata tracking for lifecycle and exit reasons
