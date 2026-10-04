# KytyPS5 - Initial Release (Xx-LiDAF-xX's Fork)

Welcome to **KytyPS5**, Xx-LiDAF-xX's optimized and streamlined fork of the PlayStation 5 emulator!

### 🌟 What makes this release unique?
Unlike other forks that feature bloated, complicated graphics menus, this release focuses on a streamlined, plug-and-play console experience. We have:
1. **Simplified "MODE" Presets:** Removed all confusing granular graphics settings (Anisotropy, Resolution Scale, etc.). The UI now features a single, clean dropdown: **MODE**, with strictly **Quality Mode** or **Performance Mode** options.
2. **Built-in Shader Cache Progress:** Instead of guessing when shaders are compiling and dealing with unexpected stutters, the OSD Performance Overlay now natively tracks and displays **Shader Cache %**. You can finally see exactly when it hits `100%`!
3. **Intel CPU Compatibility:** Many other releases crash instantly with "illegal instruction" errors on older Intel CPUs when booting modern PS5 titles. This build includes an active `x64InstructionEmulator` that safely intercepts and translates AMD-specific CPU instructions (like `SHA-NI` and `SSE4a`) on the fly, allowing Intel processors to run the emulator reliably!
4. **ClangCL Optimizations:** Built entirely using the ClangCL compiler with full Link Time Optimizations (LTO) to extract the absolute maximum performance from the backend engine.

### 🔧 Fixes & Upgrades
* **Astro Bot Memory Stability Patch:** We've implemented a targeted runtime memory stability patch. Crucially, this patch now fully supports the **US Region (PPSA21564)** alongside the EU version (`PPSA21567`), dynamically rewriting the title memory during boot to prevent notorious memory allocation crashes.
* **Auto-Update Feed Linked:** The built-in updater feed has been officially re-linked to point directly to `Xx-LiDAF-xX/KytyPS5`, ensuring that you will automatically receive any future patches pushed to this repository!
* **Global Branding Overhaul:** All previous `BryKytyPS5` branding has been scrubbed from the emulator, Vulkan renderer, and crash handler to reflect the official **KytyPS5** name.
