using System;
using System.Runtime.InteropServices;

namespace PalmierPro.Engine
{
    internal static partial class NativeMethods
    {
        private const string LibraryName = "palmier_engine";

        [LibraryImport(LibraryName)]
        public static partial IntPtr timeline_new(uint width, uint height, double fps);

        [LibraryImport(LibraryName)]
        public static partial void timeline_free(IntPtr ptr);

        [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
        public static partial IntPtr timeline_from_json(string jsonUtf8, nuint len);

        [LibraryImport(LibraryName)]
        public static partial IntPtr timeline_to_json(IntPtr timeline);

        [LibraryImport(LibraryName)]
        public static partial void string_free(IntPtr ptr);

        [LibraryImport(LibraryName)]
        public static partial long timeline_total_frames(IntPtr handle);

        [LibraryImport(LibraryName)]
        public static partial double timeline_fps(IntPtr handle);

        [LibraryImport(LibraryName)]
        public static partial nint renderer_create(uint canvas_width, uint canvas_height);

        [LibraryImport(LibraryName)]
        public static partial void renderer_destroy(nint renderer);

        [LibraryImport(LibraryName)]
        public static partial nint presenter_create(nint panelNative, nint renderer, uint w, uint h);

        [LibraryImport(LibraryName)]
        public static partial void presenter_present(nint presenter, nint outputTexture);

        [LibraryImport(LibraryName)]
        public static partial void presenter_resize(nint presenter, uint width, uint height);

        [LibraryImport(LibraryName)]
        public static partial void presenter_destroy(nint presenter);

        [LibraryImport(LibraryName)]
        public static partial nint render_frame(nint renderer, nint timeline, long frame_number);

        // ── Undo / Redo stack lifecycle ──────────────────────────────────────
        // Call timeline_undo_stack_init immediately after timeline_from_json to
        // register a fresh UndoRedoStack for this handle. Call
        // timeline_undo_stack_free just before timeline_free to avoid leaking.
        [LibraryImport(LibraryName)]
        public static partial void timeline_undo_stack_init(nint handle);

        [LibraryImport(LibraryName)]
        public static partial void timeline_undo_stack_free(nint handle);

        /// <summary>
        /// Save a before-snapshot of the current timeline state.
        /// Call this BEFORE any mutation so that Ctrl+Z can restore it.
        /// </summary>
        [LibraryImport(LibraryName)]
        public static partial void timeline_checkpoint(nint handle);

        [LibraryImport(LibraryName)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static partial bool timeline_undo(nint handle);

        [LibraryImport(LibraryName)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static partial bool timeline_redo(nint handle);

        [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
        [return: MarshalAs(UnmanagedType.U1)]
        public static partial bool timeline_update_from_json(nint handle, string jsonUtf8, nuint len);

        [LibraryImport(LibraryName)]
        public static partial IntPtr media_get_all_json(nint handle);

        [LibraryImport(LibraryName)]
        public static partial void palmier_free_string(IntPtr ptr);
    }

    [System.Runtime.InteropServices.ComImport]
    [System.Runtime.InteropServices.Guid("63aad0b8-7c24-40ff-85a8-640d944cc325")]
    [System.Runtime.InteropServices.InterfaceType(System.Runtime.InteropServices.ComInterfaceType.InterfaceIsIUnknown)]
    public interface ISwapChainPanelNative
    {
        void SetSwapChain(IntPtr swapChain);
    }
}
