# Palmier Video Editor

Palmier is a modern, high-performance, non-linear video editor utilizing a specialized cross-language architecture to combine UI flexibility with native performance:

* **Frontend**: C# / WinUI 3 (Desktop UI and timeline controls)
* **Core & Audio**: Rust (Timeline structures, Undo/Redo stack, CPAL audio mixing)
* **Rendering & Decoders**: C++ / Direct3D 12 / D3D11on12 (Hardware-accelerated video decoding via FFmpeg & GPU composting)

```
┌──────────────────────────────┐
│     C# / WinUI 3 Desktop      │
└──────────────┬───────────────┘
               │ (FFI / DLL Import)
┌──────────────▼───────────────┐
│     Rust Core & Audio        │
└──────────────┬───────────────┘
               │ (C++ ABI Bindings)
┌──────────────▼───────────────┐
│  C++ / D3D12 Render Engine   │
└──────────────────────────────┘
```

---

## Core Features

* **High-Performance GPU Compositing**: Video layers are composited on the GPU using custom HLSL compute shaders, with texture interop managed via D3D11on12.
* **Low-Latency Audio Engine**: Implements sequential multi-threaded audio decoding and mixing with CPAL output.
* **FFI Struct Safety**: Layout-aligned structures (`ActiveClipC`) shared across the FFI boundary between Rust and C++ without marshaling overhead.
* **Undo/Redo Checkpointing**: In-memory checkpointing system managed by the Rust core, allowing instant undo/redo actions.
* **Embedded MCP Server**: Communicates with the background using an integrated Model Context Protocol (MCP) server automatically launched by the frontend.

---

## Tech Stack & Prerequisites

Before building Palmier, ensure the following tools are installed:

* **.NET 9.0 SDK** (for WinUI 3 Desktop App)
* **Rust Toolchain** (Cargo/Rustc)
* **CMake** (for C++ compilation)
* **Visual Studio 2022** (with C++ Desktop development workload)
* **vcpkg** (placed at `D:/k50i/vcpkg` or customized via `VCPKG_ROOT` env variable)
  * Triplet used: `arm64-windows-static-md`
  * Libraries: FFmpeg (`avcodec`, `avformat`, `avutil`, `swscale`)

---

## Building and Running

### 1. Build the C++ Rendering Pipeline
Configure and build the native libraries using CMake:
```bash
# From the project root directory
cmake -B render/build -S render -A ARM64 -DCMAKE_BUILD_TYPE=Release
cmake --build render/build --config Release
```

### 2. Build the Rust Engine
Compile the Rust core library and FFI bindings:
```bash
cd engine
cargo build --release
cd ..
```

### 3. Build & Run the UI Application
The WinUI 3 C# project will automatically copy the native dependencies (`palmier_engine.dll` and `mcp.exe`) to the build folder and launch the application:
```bash
dotnet run --project ui/ui.csproj -c Debug -r win-arm64
```

---

## Logging & Diagnostics

* **Native Logs**: Check `palmier_engine.log` in the running directory to monitor struct memory alignments, clip loads, and seek events.
* **Frontend Logs**: Drag/drop events and background subprocess initialization errors are outputted to the C# debugger output window.
