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
    }
}
