# OS-Jackfruit Runtime

## Team Information

Arjun Bhat — PES1UG24CS655

Abhishek Aithal - PES1UG24CS651

---

## Project Overview

OS-Jackfruit is a lightweight multi-container runtime implemented in C. It demonstrates core operating system concepts including:

- namespace isolation using `clone()`
- filesystem isolation using `chroot()`
- process lifecycle supervision
- UNIX domain socket IPC
- bounded-buffer logging pipeline
- kernel-space RSS monitoring
- soft-limit warnings and hard-limit enforcement
- scheduler behavior experimentation using `nice` values
- clean teardown without zombie processes

The runtime consists of a long-running supervisor process that manages containers, a CLI interface for control operations, and a kernel module that enforces memory limits.

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
│   ├── cpu_hog
│   └── memory_hog
├── screenshots/
├── README.md
└── project-guide.md
```

`rootfs-base`, `rootfs-alpha`, `rootfs-beta`, and any other runtime rootfs copies are created locally for testing and are not committed to GitHub.

---

## Environment Setup

Required environment:

- Ubuntu 22.04 or 24.04 in a VM
- Secure Boot disabled
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

The `ci` target is the smoke-test build for user space and should succeed without rootfs setup, kernel module loading, or `sudo`.

To rebuild from scratch:

```bash
cd boilerplate
make clean
make
```

---

## Full Reproducible Workflow

This is the end-to-end sequence used for local testing on the VM.

### 1) Build user space and kernel module

```bash
make -C boilerplate ci
make -C boilerplate module
```

### 2) Load the kernel module

```bash
cd boilerplate
sudo insmod monitor.ko
ls /dev/container_monitor
```

### 3) Prepare the base rootfs and container copies

```bash
cd ~/OS-Jackfruit
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### 4) Start the supervisor

Open Terminal 1:

```bash
cd ~/OS-Jackfruit/boilerplate
sudo ./engine supervisor ../rootfs-base
```

### 5) Start containers and inspect metadata

Open Terminal 2:

```bash
cd ~/OS-Jackfruit/boilerplate
sudo ./engine start alpha ../rootfs-alpha /bin/sleep 120
sudo ./engine start beta ../rootfs-beta /bin/sleep 120
sudo ./engine ps
```

### 6) Inspect logs

```bash
sudo ./engine run logtest ../rootfs-alpha /bin/echo hello
sudo ./engine logs logtest
```

### 7) Stop containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine ps
```

### 8) Inspect kernel messages

```bash
sudo dmesg | tail
```

### 9) Unload the module

```bash
sudo rmmod monitor
```

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
- `logs` prints the per-container log file.
- `stop` requests graceful termination.

Defaults:

- soft limit = `40 MiB`
- hard limit = `64 MiB`

Each live container must use its own writable rootfs directory.

### Required `run` behavior

The `run` client waits until the container exits and returns the final status. If the `run` client receives `SIGINT` or `SIGTERM`, it forwards a stop request to the supervisor and continues waiting for final container status.

### Exit reason attribution rule

The supervisor records termination using an internal `stop_requested` flag:

- if `stop_requested` is set before exit handling, the final reason is `manually_stopped`
- if the process exits normally, the final reason is `normal_exit`
- if the process is killed by `SIGKILL` without `stop_requested`, the final reason is `hard_limit_killed`

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

## Engineering Analysis

This section explains why the operating system behaves the way it does and how the project exercises those mechanisms.

### 1) Isolation Mechanisms

The runtime uses Linux namespaces to isolate containers. PID namespaces create separate process trees, UTS namespaces isolate hostname-related state, and mount namespaces isolate filesystem mount points. `chroot()` or `pivot_root()` changes the container’s view of the filesystem so it sees its assigned rootfs as `/`.

The host kernel is still shared. Containers are isolated at the namespace level, not as separate kernels. That is the key OS idea the project exercises: strong separation of process and mount state, but one common kernel still manages scheduling, memory, system calls, and device access.

### 2) Supervisor and Process Lifecycle

A long-running supervisor is useful because it can manage multiple containers simultaneously, keep metadata for each one, and reap child processes correctly. This prevents zombies. The parent-child relationship matters because signals such as `SIGCHLD` are delivered to the parent, and the parent is responsible for calling `waitpid()` and updating container state.

The project demonstrates how lifecycle management works in real container runtimes: containers do not exist on their own; the supervisor tracks them, classifies their exit reasons, and controls stop requests.

### 3) IPC, Threads, and Synchronization

The project uses two IPC mechanisms: a UNIX domain socket for control commands and pipes for log capture. The logging subsystem uses a bounded buffer shared by producer and consumer threads.

Without synchronization, the following races would occur:

- two producer threads could write to the same buffer slot at the same time
- a consumer could read a partially written log entry
- unread entries could be overwritten when the buffer wraps
- a producer could overwrite unread data when the buffer is full
- a consumer could miss the shutdown signal and block forever

To prevent this, the implementation uses:

- a mutex to protect buffer access
- condition variables to block producers when the buffer is full
- condition variables to block consumers when the buffer is empty

This prevents corruption, loss, and deadlock. It also ensures that shutdown is orderly and that remaining log data is flushed before threads exit.

### 4) Memory Management and Enforcement

RSS measures the amount of resident physical memory currently in RAM. It does not include everything a process has mapped or allocated conceptually.

Soft and hard limits are different policies:

- soft limit = warn and continue
- hard limit = enforce termination

The enforcement belongs in kernel space because the kernel has the accurate memory accounting information and the authority to terminate a process safely. User space can observe memory usage, but kernel space is the correct place to enforce process-level memory constraints.

### 5) Scheduling Behavior

The `--nice` experiment shows how Linux scheduling reacts to priority values. Lower nice values correspond to higher scheduler priority, which changes how much CPU time a workload receives when competing with another workload.

This demonstrates the tradeoff between fairness and throughput: higher-priority tasks complete sooner, while lower-priority tasks receive less CPU share. The project exercises the Linux scheduler directly by setting process priority before `execvp()`.

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
sudo ./engine start cpu1 ../rootfs-alpha /cpu_hog --nice -5
sudo ./engine start cpu2 ../rootfs-beta /cpu_hog --nice 10
sudo ./engine ps
```

The metadata shows the configured nice values and the scheduler experiment can be compared using these priorities.

---

## Scheduler Experiment Results

Two CPU-bound workloads were launched concurrently with different priorities.

Observed concurrent run:

- `fast` used `--nice -5`
- `slow` used `--nice 10`

Observed metadata snapshot from `engine ps`:

| Container | Nice Value | State During Observation | Timestamp |
|-----------|-----------:|--------------------------|-----------|
| fast | -5 | running | 2026-04-14T04:08:06 |
| slow | 10 | running | 2026-04-14T04:08:10 |

Observed completion order from the same run:

| Container | Nice Value | Result |
|-----------|-----------:|--------|
| fast | -5 | exited first |
| slow | 10 | exited later |

### Raw supporting measurement

A foreground baseline check returned:

```text
real    0m5.010s
```

for `sudo ./engine run runcheck ../rootfs-alpha /bin/sleep 5`.

### Interpretation

Linux’s Completely Fair Scheduler allocates CPU proportionally based on nice values. Lower nice values receive greater CPU share, demonstrating priority-based scheduling behavior.

The observed result confirms that the higher-priority container (`nice = -5`) completed before the lower-priority container (`nice = 10`) during concurrent execution, which is the expected outcome when both workloads contend for CPU time. The exact wall-clock difference can vary across VMs and host load, but the exit order and concurrent metadata snapshot are the important evidence.

---

## Cleanup Guarantees

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

Use the verified hard-limit run:

```bash
cp -a rootfs-base rootfs-hardtest
cp boilerplate/memory_hog rootfs-hardtest/
sudo ./engine start hardkill ../rootfs-hardtest /memory_hog --soft-mib 10 --hard-mib 16
sleep 6
sudo ./engine ps | grep hardkill
sudo dmesg | tail -n 20
```

Observed result:

```text
hardkill 40774 exited hard_limit_killed 10 16 - 2026-04-14T04:20:51 -1 9 0 logs/hardkill.log
```

Kernel log evidence:

```text
[container_monitor] Registering container=hardkill pid=40774 soft=10485760 hard=16777216
[container_monitor] SOFT LIMIT container=hardkill pid=40774 rss=17244160 limit=10485760
[container_monitor] HARD LIMIT container=hardkill pid=40774 rss=17244160 limit=16777216
[container_monitor] Removing hard-limit entry container=hardkill pid=40774
```

### Scheduling

```bash
sudo ./engine start cpu1 ../rootfs-alpha --nice -5 /cpu_hog
sudo ./engine start cpu2 ../rootfs-beta /cpu_hog --nice 10
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
