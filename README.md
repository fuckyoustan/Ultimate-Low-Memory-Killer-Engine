# Ultimate Low Memory Killer Engine

A lightweight userspace low-memory killer daemon for Android, written in pure C with zero external dependencies. `lmk_engine` runs alongside Android's stock `lmkd` to keep frequently used applications resident in memory under sustained pressure—without the CPU overhead of shell-based tweaks or the UI stutter caused by indiscriminate process kills.

---

## Purpose

Stock Android memory management relies primarily on static `oom_score_adj` tiers and strict Least Recently Used (LRU) ordering. On memory-constrained devices, this frequently evicts daily-driver apps the moment a heavy foreground task or background service spikes RAM usage. Meanwhile, common Magisk "RAM booster" modules rely on one-shot `sysctl` writes or polling shell scripts that spawn subshells and burn CPU cycles.

`lmk_engine` solves this at the native daemon layer by combining behavioral app retention, graduated memory reclamation, and active ZRAM/swap management in a single compiled binary.

---

## How It Differs from Other Solutions

| Aspect | Shell-Based RAM Modules | Stock Android `lmkd` | `lmk_engine` |
| :--- | :--- | :--- | :--- |
| **Runtime** | Shell loops (`sh`/`ash`) spawning external binaries | Native daemon (`lmkd`) | Single compiled C binary (no subshells or external libs) |
| **Retention Logic** | Static `oom_score_adj` overrides or blind cache drops | Strictly recency-based (LRU) and OOM score tiers | Behavioral app pinning based on real usage patterns |
| **Reclamation** | Periodic `drop_caches` (causes reload stutter) | Threshold-triggered kills | Graduated, multi-stage cleanup proportional to pressure |
| **ZRAM & Swap** | Static boot-time ratios | Passive kernel paging | Active ZRAM awareness + on-demand swapfile provisioning |
| **Observability** | None or raw logcat spam | Minimal kernel/logcat events | Built-in CLI diagnostics (`--status`, `--explain <pkg>`) |

---

## Key Features

- **Behavioral App Pinning** — Tracks real foreground usage patterns to protect your most-used applications from premature eviction, rather than judging apps solely by last-opened timestamp.
- **Graduated Adaptive Clean** — Reclaims memory from idle background processes incrementally as pressure rises, avoiding sudden mass kills that degrade frame pacing and responsiveness.
- **RAM & ZRAM Footprint Awareness** — Evaluates both physical RSS and ZRAM consumption when selecting reclamation candidates, preventing compressed memory thrashing.
- **System Partition Protection** — Distinguishes core OS components (`/system`, `/system_ext`, `/product`, `/vendor`, `/odm`, `/apex`) from third-party user apps to maintain system stability.
- **ZRAM & Dynamic Swap Management** — Tunes ZRAM behavior and lets you allocate additional swap space on the fly when physical RAM is chronically tight.
- **Per-Package Introspection** — Inspect why any specific app is pinned, scored, or targeted for reclamation in real time using `--explain <pkg>`.

---

## How It Works

```text
Boot (Magisk / KernelSU / APatch)
  ├── Applies system.prop (relaxes stock lmkd thresholds)
  └── Starts lmk_engine daemon
            │
            ▼
  [1. Classification & Pinning] ──► Separates system partitions from user apps
            │                       Tracks foreground usage to pin frequent apps
            ▼
  [2. Pressure Monitoring]      ──► Samples available RAM, ZRAM usage, and swap headroom
            │
            ▼
  [3. Adaptive Clean]           ──► Incrementally kills unpinned background apps by RAM/ZRAM cost
            │                       Preserves pinned apps; stock lmkd remains as failsafe
            ▼
  [4. ZRAM & Swap Balancing]    ──► Manages ZRAM and utilizes backing swapfile when configured
```

---

## Usage

It works automatically after flashing the module, but you can run this in the terminal for diagnostics:

```
lmk_engine --start              start the daemon
lmk_engine --stop               stop the daemon
lmk_engine --status             show memory/ZRAM/swap/pin status
lmk_engine --log                tail the live log
lmk_engine --swap <size in mb>  add swap
lmk_engine --explain <pkg>      see what the daemon knows about one app
```

---

## Requirements

- Rooted Android device (Magisk, KernelSU, or APatch)
- `clang` or `gcc` (only required if compiling from source)

---

## Installation

1. Download the latest release ZIP from [**Releases**](../../releases).
2. Flash the ZIP in your root manager (Magisk, KernelSU, or APatch) and reboot.

---

## Building from Source

Compile the daemon directly on-device (e.g., via Termux) or with an Android NDK toolchain:

```bash
clang -O2 -Wall -Wextra -o lmk_engine src/lmk_engine.c -lm
```

## License

MIT — see `LICENSE`.
