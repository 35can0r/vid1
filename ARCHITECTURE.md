# Palmier Pro Architecture Analysis

This document describes the two core systems of the Palmier Pro video editor: the Timeline Data Structure and the Model Context Protocol (MCP) server implementation. This analysis is intended to guide a cross-platform Rust/C++ rewrite.

## 1. Project Package & Where Everything Lives

A project is saved as a macOS File Package (a directory that appears as a single file) with the `.palmier` extension. Inside:

- `project.json`: Serialized Timeline (tracks, clips, keyframes). This encodes the entire edit state.
- `media.json`: MediaManifest (asset metadata, source paths). Maps opaque `mediaRef` IDs found in `project.json` to actual file paths on disk.
- `generation-log.json`: History of AI generation requests.
- `thumbnail.jpg`: Preview image shown in the HomeView.
- `media/`: Directory of assets imported into the project.
- `chats/`: One JSON file per Agent chat session.

**Rewrite Note:** `project.json` and `media.json` are read and written together, linked via UUIDs.

## 2. Timeline Data Structure

The timeline is a hierarchical, JSON-serializable structure that represents the video project. It is strictly frame-based.

### Core Hierarchy
- **`Timeline`**: The root object containing project settings (`fps`, `width`, `height`) and an array of `Track` objects.
  - **Rewrite Note:** Use a struct with `Vec<Track>`. Timing relies on `fps` (`seconds = frame / fps`).
- **`Track`**: A lane on the timeline holding an array of `Clip` objects. Tracks are typed (`ClipType`: `video` or `audio`) to enforce placement constraints.
  - **Rewrite Note:** Gaps are implicit (unoccupied frame ranges). Enforce non-overlapping invariants in the mutation layer.
- **`Clip`**: The fundamental unit of media on the timeline. It references a `MediaAsset` (via `mediaRef`) and defines its placement, trimming, and presentation properties.
  - **Rewrite Note:** Use a struct with an enum payload (`enum ClipContent { Video { transform, crop }, Audio { volume }, Text { content, style, layout } }`). Timing fields live on the outer struct.

### Key `Clip` Properties
*   **Identification**: `id`, `mediaRef` (links to the `MediaManifest`).
*   **Placement (Timeline)**: `startFrame`, `durationFrames`. All timing on the timeline is in absolute frames.
*   **Trimming (Source)**: `trimStartFrame`, `trimEndFrame`. Source-media offsets defining the visible portion.
*   **Playback**: `speed`, `volume`.
*   **Presentation**: `opacity`, `transform` (normalized 0.0-1.0 coords for center, size, rotation, flips), `crop` (normalized 0.0-1.0 edge insets).
*   **Linking**: `linkGroupId`, `captionGroupId`.

### Animation and Keyframes
Clips support animation via generic `KeyframeTrack` objects.
A `KeyframeTrack<T>` is a sorted list of `Keyframe<V>` structs, where each keyframe specifies a `frame` (clip-relative), a `value`, and an `interpolationOut` (linear, hold, smooth).

**Rewrite Note:** Implement a generic `KeyframeTrack<T: Lerp>` with a `BTreeMap<i64, Keyframe<T>>`.

### JSON Serialization & Compaction
When writing to disk or sending to the LLM (via `get_timeline`), the JSON is compacted to reduce size/token count:
- UUIDs are lowercase hyphenated strings.
- Omit all fields that equal their default value (e.g., opacity: 1.0, speed: 1.0).
- Keyframe tracks are omitted if empty.
- For LLM context, individual caption clips are replaced with a `CaptionGroup` summary per track.

## 3. MCP (Model Context Protocol) Server Implementation

Palmier Pro acts as an MCP server, allowing an AI agent to directly interact with the editor.

### Architecture
*   **`MCPHTTPServer`**: A minimal HTTP server binding locally to `127.0.0.1:19789`. It processes HTTP requests into MCP payloads.
  - **Rewrite Note:** Use `axum` or `actix-web` with a JSON-RPC 2.0 handler at `POST /mcp`. Bind to loopback only.
*   **`MCPService`**: Registers tools (`ToolDefinitions`) and tears down the server when a project closes.
*   **`ToolExecutor`**: The single entry point for tool execution. Validates arguments and dispatches to the correct implementation. Wraps operations in `withUndoGroup`.
*   **Claude Desktop Bridging (`mcpb`)**: Claude Desktop only supports `stdio` transport. The app bundles a Node.js shim (`mcpb/server/index.js` using `mcp-remote`) that translates `stdio` to HTTP and forwards requests to `127.0.0.1:19789/mcp`.

### Execution Flow (Agent -> Editor)
1.  **Tool Call**: The agent sends an MCP `CallTool` request over HTTP.
2.  **Validation Layer**:
    - **Unknown key check**: Compares JSON keys to schema properties.
    - **Type decode**: Maps raw dict to internal structs.
    - **Finiteness check**: Rejects `NaN`/`Infinity` in float fields.
    - **Rewrite Note**: Replicate this strict 3-step validation. Return structured error strings for LLM self-correction.
3.  **Mutation**: Dispatches to specific handlers (e.g., `add_clips`, `split_clip`). Mutations occur on the main thread via `EditorViewModel`.
    - **Rewrite Note**: The Swift app uses `NSUndoManager`. Implement a Command Stack in Rust where each entry is an enum of reversible operations (e.g., `InsertClip`, `MoveClip`).
    - **Rewrite Note:** For threading, use `Arc<Mutex<Timeline>>` or the actor pattern via `tokio`. The HTTP handler should send commands through a channel to the "engine thread".
4.  **Response**: Returns `ToolResult` back over HTTP.

### In-App Agent (`AgentService`)
The in-app chat panel uses `AgentService` instead of `MCPService`. It manages conversation history, handles `@mention` syntax, and supports Anthropic API keys or the Palmier backend. It converges on the exact same `ToolExecutor.execute()` pipeline as external MCP clients.