# Palmier Pro Architecture Analysis

This document describes the two core systems of the Palmier Pro video editor: the Timeline Data Structure and the Model Context Protocol (MCP) server implementation. This analysis is intended to guide a cross-platform Rust/C++ rewrite.

## 1. Timeline Data Structure

The timeline is a hierarchical, JSON-serializable structure that represents the video project. It is strictly frame-based.

### Core Hierarchy
- **`Timeline`**: The root object containing project settings (`fps`, `width`, `height`) and an array of `Track` objects.
- **`Track`**: A lane on the timeline holding an array of `Clip` objects. Tracks are typed (`ClipType`) to enforce placement constraints (e.g., video clips on video tracks, audio on audio tracks).
- **`Clip`**: The fundamental unit of media on the timeline. It references a `MediaAsset` (via `mediaRef`) and defines its placement, trimming, and presentation properties.

### Key `Clip` Properties
*   **Identification**: `id`, `mediaRef` (links to the underlying `MediaAsset`).
*   **Typing**: `mediaType`, `sourceClipType` (e.g., `video`, `audio`, `image`, `text`, `lottie`).
*   **Placement (Timeline)**: `startFrame`, `durationFrames`. All timing on the timeline is in absolute frames.
*   **Trimming (Source)**: `trimStartFrame`, `trimEndFrame`. These are source-media offsets, defining which portion of the underlying media is visible.
*   **Playback**: `speed`, `volume`.
*   **Presentation**: `opacity`, `transform` (custom struct for center, size, rotation, flips), `crop` (custom struct for edge insets).
*   **Fades**: `fadeInFrames`, `fadeOutFrames`, `fadeInInterpolation`, `fadeOutInterpolation`.
*   **Grouping**: `linkGroupId`, `captionGroupId`.
*   **Text/Captions**: `textContent`, `textStyle` (for text overlays).

### Animation and Keyframes
Clips support animation via generic `KeyframeTrack` objects for several properties:
*   `opacityTrack`, `volumeTrack`, `rotationTrack` (`KeyframeTrack<Double>`)
*   `positionTrack`, `scaleTrack` (`KeyframeTrack<AnimPair>`)
*   `cropTrack` (`KeyframeTrack<Crop>`)

A `KeyframeTrack` contains a sorted list of `Keyframe<V>` structs, where each keyframe specifies a `frame` (clip-relative), a `value`, and an `interpolationOut` (linear, hold, smooth). The clip "samples" these tracks at specific frames to determine its state (e.g., `opacityAt(frame: Int)`).

### Frame Coordinate System
*   Timeline time is measured in absolute project frames (`fps` is defined at the `Timeline` level).
*   `startFrame` is the absolute position on the timeline.
*   Keyframe times are *clip-relative* (`keyframeOffset = absoluteFrame - startFrame`).
*   Media playback utilizes source frames, derived from absolute frames via `trimStartFrame` and `speed`.

## 2. MCP (Model Context Protocol) Server Implementation

Palmier Pro acts as an MCP server, allowing an AI agent (e.g., Claude) to directly interact with the editor by reading state and executing editing actions.

### Architecture
*   **`MCPHTTPServer`**: A custom, minimal HTTP server built on Apple's `Network` framework (`NWListener`). It binds locally to `127.0.0.1:19789` and processes standard HTTP requests into MCP payloads via `StatelessHTTPServerTransport`.
*   **`MCPService`**: The orchestrator. It instantiates the `MCPHTTPServer` and an MCP `Server` object (from the `MCP` library). It registers the required MCP tools and resources.
*   **`ToolExecutor`**: The bridge between the MCP interface and the application's internal state (`EditorViewModel`). It receives a tool name and arguments, validates them, and mutates the `EditorViewModel` directly on the `MainActor`.

### Registration and Tools
`MCPService` registers available tools defined in `ToolDefinitions`. These definitions include the tool name, description, and an JSON Schema for its arguments. The agent calls these tools to inspect or mutate the project.

Key tools include:
*   **Read State**: `get_timeline`, `get_media`, `inspect_media`, `get_transcript`. The agent uses these to understand the current project state (read-only).
*   **Mutation (Editing)**: `add_clips`, `remove_clips`, `remove_tracks`, `move_clips`, `set_clip_properties`, `set_keyframes`, `split_clip`, `ripple_delete_ranges`. These map directly to human editing gestures.
*   **Generation**: `generate_video`, `generate_image`, `generate_audio`, `upscale_media`. These trigger async background jobs.

### Execution Flow (Agent -> Editor)
1.  **AI Decides**: The AI agent, based on its context and instructions (`AgentInstructions.serverInstructions`), decides to perform an action (e.g., "split clip A at frame 100").
2.  **Tool Call**: The agent sends an MCP `CallTool` request over HTTP.
3.  **HTTP to Server**: `MCPHTTPServer` receives the request, parses it, and hands it to the internal `Server` object.
4.  **Dispatch**: `MCPService` handles the `CallTool` method. It bridges the arguments (`ToolArgsBridge.argsFromMCP`) and calls `ToolExecutor.execute(name:args:)`.
5.  **Validation & Mutation**: `ToolExecutor` routes to the specific implementation (e.g., `ToolExecutor+Clips.swift -> splitClip`). It validates the arguments, finds the target clip in the `EditorViewModel`, performs the mutation (often wrapping it in `withUndoGroup`), and returns a string describing the result.
6.  **Response**: The result is wrapped into an MCP `CallTool.Result` and sent back over HTTP to the agent.

### The EditorViewModel Connection
The `ToolExecutor` holds a reference to the `EditorViewModel`. All timeline mutations triggered by the agent are executed as methods on the `EditorViewModel` on the main thread (e.g., `editor.splitClip(clipId:atFrame:)`). This ensures that AI-driven edits use the exact same code paths as user-driven UI edits, maintaining consistency and populating the undo stack correctly.