# TinyOS v3 — Operating Systems Coursework

Semester coursework based on **TinyOS v3**, an educational operating-system framework used in the Operating Systems course at the **Technical University of Crete** (circa 2019–2020).

## Project overview

This repository preserves a coursework-era snapshot of TinyOS with modifications in operating-system kernel components. The work involved C programming and concepts such as process and thread management, scheduling, and inter-process communication.

### Areas with changes relative to the included upstream Git baseline

- **Threads:** `kernel_threads.c`
- **Processes:** `kernel_proc.c`, `kernel_proc.h`
- **Scheduling:** `kernel_sched.c`, `kernel_sched.h`
- **Pipes and streams:** `kernel_pipe.c`, `kernel_streams.c`, `kernel_streams.h`
- **Sockets:** `kernel_socket.c` — contains incomplete/commented-out implementation work; **socket support is not claimed to be functional**.

These are file-level differences in the preserved coursework copy, not a verified line-by-line attribution of individual authorship.

## Build

On a compatible Linux environment with GCC, Make and pthread support:

```bash
make
```

The original framework README is preserved in [`UPSTREAM_README.md`](UPSTREAM_README.md). **Build and tests have not been revalidated for this archived coursework snapshot.**

## Attribution and license

Based on the original [TinyOS v3 repository by vsamtuc](https://github.com/vsamtuc/tinyos3), provided as a teaching framework. The included Git snapshot points to upstream commit `3d78d08` (2019-10-10). This is **not** a from-scratch operating system. Original framework code and course modifications are distributed with the included GNU GPL v2 license (`LICENSE`).

This repository is a historical academic coursework archive, not production-ready software.
