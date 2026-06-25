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
        public static partial nint render_frame(nint renderer, long frame_number);
    }

    [System.Runtime.InteropServices.ComImport]
    [System.Runtime.InteropServices.Guid("63aad0b8-7c24-40ff-85a8-640d944cc325")]
    [System.Runtime.InteropServices.InterfaceType(System.Runtime.InteropServices.ComInterfaceType.InterfaceIsIUnknown)]
    public interface ISwapChainPanelNative
    {
        void SetSwapChain(IntPtr swapChain);
    }
}
