# High-Performance Standalone ADB Client for Linux (C++)

Enterprise-grade, lightweight ADB client extracted and optimized from AOSP sources. Designed specifically for massive device fleet management (Digital Signage, Hotel TV, Kiosks) where standard Android SDK tools are too heavy, unstable, or lack concurrency support.

## 🚀 Key Features & Production Proven
- **Massive Concurrency:** Successfully stress-tested and deployed managing **3,500+ simultaneous TCP connections** without memory leaks or thread starvation.
- **Zero Bloat:** Lightweight `.so` library. No need to install the massive, dependency-heavy Android SDK on your servers.
- **Robust Multithreading:** Custom thread pool and async I/O handling, rigorously tested with AddressSanitizer (ASan).
- **CI/CD Ready:** Built with modern CMake and GCC. Easily integrates into automated build pipelines for reproducible builds.
- **Secure:** Designed to work alongside custom PKI/hardware token licensing systems.

## 🛠 Tech Stack
- C++17
- CMake / GCC
- Linux (Debian 12/13, Ubuntu 24.04/26.04)
- POSIX threads / Async I/O

## 📦 Documentation

Technical documentation and build instructions are available in Russian. See [README-RU.md](./README-RU.md) for details.

### 💼 Available for Contract Work & Consulting

**I am the author of this system.** I have 26 years of total experience in the IT industry. While my specific tech stack has evolved over the decades, my core specialization remains constant: **optimizing high-load, mission-critical systems that operate in strict 24/7 production environments.**

If your engineering team is struggling with:
- Memory leaks, race conditions, or scaling bottlenecks in C++/Linux backends.
- Managing device fleets beyond hundreds of units (especially custom AOSP/non-GMS devices).
- Setting up robust, reproducible CI/CD pipelines for Debian/Ubuntu packages (including secure, offline GPG signing).

I am available for **part-time contract work (20–30 hrs/week)** or targeted architectural audits.

**How I work:**
- **Asynchronous & Focused:** I prefer text-based communication (Jira, GitHub, email) to maintain deep focus and avoid meeting fatigue.
- **Efficiency:** I leverage modern AI orchestration to accelerate routine tasks, delivering senior-level results faster without compromising quality.
- **Reliability:** All code is backed by rigorous QA and designed specifically for zero-downtime environments.

**Contact:** [@mgkp_mgnkp](https://t.me/mgkp_mgnkp)
