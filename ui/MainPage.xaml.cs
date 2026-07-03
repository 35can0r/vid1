using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Diagnostics;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;
using Windows.ApplicationModel.DataTransfer;
using PalmierPro.Engine;
using PalmierPro.UI.Models;
using WinRT;

namespace ui
{
    public sealed partial class MainPage : Page
    {
        public class MediaItem
        {
            public string Name { get; set; } = "";
            public string Duration { get; set; } = "";
            public string Icon { get; set; } = "";
            public string Id { get; set; } = "";
            public string Path { get; set; } = "";
        }

        private Timeline _timeline = new();
        private IntPtr _timelinePtr = IntPtr.Zero;
        private DispatcherTimer _playbackTimer = new();
        private IntPtr _engine = IntPtr.Zero;
        private IntPtr _audioEngine = IntPtr.Zero;
        private IntPtr _presenter = IntPtr.Zero;
        private System.IO.FileSystemWatcher? _mediaWatcher = null;
        private bool _isInitialized = false;
        private bool _firstFrameLogged = false;
        private List<MediaEntry> _mediaItems = new();
        private Clip? _selectedClip = null;

        private void Log(string message)
        {
            System.Diagnostics.Debug.WriteLine(message);
            Console.WriteLine(message);
            Console.Out.Flush();
        }

        public MainPage()
        {
            // Set current directory to the project workspace root
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            while (!string.IsNullOrEmpty(dir))
            {
                if (System.IO.File.Exists(System.IO.Path.Combine(dir, "media.json")))
                {
                    try
                    {
                        System.IO.Directory.SetCurrentDirectory(dir);
                        System.Diagnostics.Debug.WriteLine($"[Init] Working directory set to: {dir}");
                        Console.WriteLine($"[Init] Working directory set to: {dir}");
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"[Init] Failed to set directory: {ex.Message}");
                    }
                    break;
                }
                dir = System.IO.Path.GetDirectoryName(dir);
            }

            InitializeComponent();

            // Start background mcp.exe server if not already running
            try
            {
                string mcpPath = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mcp.exe");
                if (System.IO.File.Exists(mcpPath))
                {
                    var existing = System.Diagnostics.Process.GetProcessesByName("mcp");
                    if (existing.Length == 0)
                    {
                        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                        {
                            FileName = mcpPath,
                            CreateNoWindow = true,
                            UseShellExecute = false
                        });
                        System.Diagnostics.Debug.WriteLine("[Init] Launched background mcp.exe server.");
                        Console.WriteLine("[Init] Launched background mcp.exe server.");
                    }
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[Init] Failed to start background mcp.exe: {ex.Message}");
                Console.WriteLine($"[Init] Failed to start background mcp.exe: {ex.Message}");
            }
            PreviewCanvas.SizeChanged += OnPreviewCanvasSizeChanged;
            this.Loaded += OnPageLoaded;
            Unloaded += MainPage_Unloaded;

            // Setup Playback Timer (30fps = 33.3ms interval)
            _playbackTimer.Interval = TimeSpan.FromMilliseconds(33.3);
            _playbackTimer.Tick += PlaybackTimer_Tick;
        }

        private void OnPreviewCanvasSizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (_isInitialized && _presenter != IntPtr.Zero)
            {
                NativeMethods.presenter_resize(_presenter, (uint)e.NewSize.Width, (uint)e.NewSize.Height);
            }
        }

        private async void OnPageLoaded(object sender, RoutedEventArgs e)
        {
            System.Diagnostics.Debug.WriteLine("[Init] Page loaded. Starting D3D12 init...");
            Console.WriteLine("[Init] Page loaded. Starting D3D12 init...");

            InitializeMediaBrowser();
            InitializeMockTimeline();
            InitializeRustFFI();
            TimelineEditor.GetMediaNameCallback = GetMediaName;
            TimelineEditor.ClipSelected += (clip) => { _selectedClip = clip; };
            RefreshMediaBrowser();
            SetupMediaWatcher();

            uint canvasWidth = (uint)PreviewCanvas.ActualWidth;
            uint canvasHeight = (uint)PreviewCanvas.ActualHeight;

            // Fallback if size is 0 initially
            if (canvasWidth == 0) canvasWidth = (uint)_timeline.width;
            if (canvasHeight == 0) canvasHeight = (uint)_timeline.height;

            // XAML has now rendered. Initialize D3D12 off the UI thread.
            await Task.Run(() => {
                _engine = NativeMethods.renderer_create(canvasWidth, canvasHeight);
            });

            _audioEngine = NativeMethods.audio_engine_create(_timelinePtr);
            System.Diagnostics.Debug.WriteLine($"[Init] renderer_create returned: 0x{_engine.ToInt64():X}, audio_engine: 0x{_audioEngine.ToInt64():X}");
            Console.WriteLine($"[Init] renderer_create returned: 0x{_engine.ToInt64():X}, audio_engine: 0x{_audioEngine.ToInt64():X}");
            if (_audioEngine == IntPtr.Zero)
            {
                Console.WriteLine("[Init] WARNING: audio_engine_create returned null — audio will be silent.");
                System.Diagnostics.Debug.WriteLine("[Init] WARNING: audio_engine_create returned null — audio will be silent.");
            }
            else
            {
                Console.WriteLine($"[Init] Audio engine ready at 0x{_audioEngine.ToInt64():X}.");
            }

            if (_engine == IntPtr.Zero)
            {
                System.Diagnostics.Debug.WriteLine("[Init] ERROR: renderer_create returned null!");
                Console.WriteLine("[Init] ERROR: renderer_create returned null!");
                return;
            }

            try
            {
                // Back on UI thread: attach swapchain NOW that panel has valid size
                nint panelNativePtr = IntPtr.Zero;
                unsafe
                {
                    var guid = typeof(PalmierPro.Engine.ISwapChainPanelNative).GUID;
                    var unk = Marshal.GetIUnknownForObject(PreviewCanvas);
                    Marshal.QueryInterface(unk, ref guid, out panelNativePtr);
                    Marshal.Release(unk);
                }

                _presenter = NativeMethods.presenter_create(panelNativePtr, _engine, canvasWidth, canvasHeight);
                System.Diagnostics.Debug.WriteLine($"[Init] presenter_create returned: 0x{_presenter.ToInt64():X}");
                Console.WriteLine($"[Init] presenter_create returned: 0x{_presenter.ToInt64():X}");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[Init] Exception during presenter creation: {ex}");
                Console.WriteLine($"[Init] Exception during presenter creation: {ex}");
            }

            // Connect event handlers
            TimelineEditor.ClipPositionChanged += OnClipPositionChanged;
            TimelineEditor.PlayheadPositionChanged += OnPlayheadPositionChanged;
            
            // Set initial playhead timecode
            OnPlayheadPositionChanged(TimelineEditor.PlayheadFrame);

            if (_timelinePtr != IntPtr.Zero)
            {
                double fps = NativeMethods.timeline_fps(_timelinePtr);
                if (fps > 0.0 && !double.IsNaN(fps) && !double.IsInfinity(fps))
                {
                    _playbackTimer.Interval = TimeSpan.FromSeconds(1.0 / fps);
                }
            }

            _isInitialized = true;
            RenderCurrentFrame();
        }

        private void MainPage_Unloaded(object sender, RoutedEventArgs e)
        {
            FreeRustFFI();
            if (_mediaWatcher != null)
            {
                _mediaWatcher.EnableRaisingEvents = false;
                _mediaWatcher.Dispose();
                _mediaWatcher = null;
            }
            if (_presenter != IntPtr.Zero)
            {
                NativeMethods.presenter_destroy(_presenter);
                _presenter = IntPtr.Zero;
            }
            if (_engine != IntPtr.Zero)
            {
                NativeMethods.renderer_destroy(_engine);
                _engine = IntPtr.Zero;
            }
            if (_audioEngine != IntPtr.Zero)
            {
                NativeMethods.audio_engine_destroy(_audioEngine);
                _audioEngine = IntPtr.Zero;
            }
            _playbackTimer.Stop();
        }

        private void InitializeMediaBrowser()
        {
            // Do not bind hardcoded ItemsSource so we can use Items.Add manually.
        }

        public record MediaEntry(string Id, string Name, string Path);

        private void SetupMediaWatcher()
        {
            try
            {
                _mediaWatcher = new System.IO.FileSystemWatcher(".", "media.json")
                {
                    NotifyFilter = System.IO.NotifyFilters.LastWrite | System.IO.NotifyFilters.Size | System.IO.NotifyFilters.FileName
                };
                System.IO.FileSystemEventHandler handler = (s, e) =>
                {
                    Console.WriteLine($"[MediaWatcher] Event '{e.ChangeType}' detected on {e.Name}. Queuing refresh...");
                    this.DispatcherQueue.TryEnqueue(() => {
                        RefreshMediaBrowser();
                    });
                };
                _mediaWatcher.Changed += handler;
                _mediaWatcher.Created += handler;
                _mediaWatcher.Deleted += handler;
                _mediaWatcher.Renamed += (s, e) => {
                    Console.WriteLine($"[MediaWatcher] Event 'Renamed' detected. Queuing refresh...");
                    this.DispatcherQueue.TryEnqueue(() => {
                        RefreshMediaBrowser();
                    });
                };
                _mediaWatcher.EnableRaisingEvents = true;
                Console.WriteLine("[Init] FileSystemWatcher for media.json initialized.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Init] Failed to setup media watcher: {ex.Message}");
            }
        }

        private string GetMediaName(string mediaRef)
        {
            return _mediaItems.Find(m => m.Id == mediaRef)?.Name ?? mediaRef;
        }

        private void RefreshMediaBrowser()
        {
            if (_timelinePtr == IntPtr.Zero)
            {
                Console.WriteLine("[MediaBrowser] Cannot refresh: _timelinePtr is null.");
                return;
            }

            IntPtr jsonPtr = NativeMethods.media_get_all_json(_timelinePtr);
            if (jsonPtr != IntPtr.Zero)
            {
                try
                {
                    string json = Marshal.PtrToStringAnsi(jsonPtr) ?? "[]";
                    var mediaEntries = JsonSerializer.Deserialize<List<MediaEntry>>(json);
                    if (mediaEntries != null)
                    {
                        _mediaItems = mediaEntries;
                        MediaListView.Items.Clear();
                        foreach (var entry in mediaEntries)
                        {
                            MediaListView.Items.Add(new MediaItem
                            {
                                Name = entry.Name,
                                Icon = "🎬",
                                Duration = "",
                                Id = entry.Id,
                                Path = entry.Path
                            });
                        }
                        TimelineEditor.RebuildTimelineUI();
                        Console.WriteLine($"[MediaBrowser] Refreshed {mediaEntries.Count} items.");
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[MediaBrowser] Error refreshing: {ex.Message}");
                }
                finally
                {
                    NativeMethods.palmier_free_string(jsonPtr);
                }
            }
            else
            {
                Console.WriteLine("[MediaBrowser] media_get_all_json returned null.");
            }
        }

        private void InitializeMockTimeline()
        {
            if (System.IO.File.Exists("timeline.json"))
            {
                try
                {
                    string json = System.IO.File.ReadAllText("timeline.json");
                    var loadedTimeline = JsonSerializer.Deserialize<Timeline>(json);
                    if (loadedTimeline != null)
                    {
                        _timeline = loadedTimeline;

                        int videoTrackCount = 0;
                        int audioTrackCount = 0;
                        foreach (var track in _timeline.tracks)
                        {
                            if (track.track_type == "video") videoTrackCount++;
                            else if (track.track_type == "audio") audioTrackCount++;
                        }

                        bool timelineMutated = false;
                        while (videoTrackCount < 2)
                        {
                            _timeline.tracks.Add(new Track("video"));
                            videoTrackCount++;
                            timelineMutated = true;
                        }
                        while (audioTrackCount < 1)
                        {
                            _timeline.tracks.Add(new Track("audio"));
                            audioTrackCount++;
                            timelineMutated = true;
                        }

                        if (timelineMutated)
                        {
                            string jsonOut = JsonSerializer.Serialize(_timeline, new JsonSerializerOptions { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull });
                            System.IO.File.WriteAllText("timeline.json", jsonOut);
                        }

                        TimelineEditor.TimelineData = _timeline;
                        System.Diagnostics.Debug.WriteLine("[Init] Loaded timeline from timeline.json");
                        Console.WriteLine("[Init] Loaded timeline from timeline.json");
                        return;
                    }
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"[Init] Error loading timeline.json: {ex.Message}");
                    Console.WriteLine($"[Init] Error loading timeline.json: {ex.Message}");
                }
            }

            _timeline = new Timeline
            {
                width = 1920,
                height = 1080,
                fps = 30.0,
                tracks = new List<Track>
                {
                    new("video")
                    {
                        clips = new List<Clip>
                        {
                            new()
                            {
                                media_ref = "opening_shot.mp4",
                                start_frame = 0,
                                duration_frames = 150,
                                trim_start_frame = 0,
                                speed = 1.0,
                                opacity = 1.0,
                                transform = new PalmierPro.UI.Models.Transform()
                            },
                            new()
                            {
                                media_ref = "b-roll_city.mp4",
                                start_frame = 200,
                                duration_frames = 300,
                                trim_start_frame = 0,
                                speed = 1.0,
                                opacity = 0.8,
                                transform = new PalmierPro.UI.Models.Transform()
                            }
                        }
                    },
                    new("audio")
                    {
                        clips = new List<Clip>
                        {
                            new()
                            {
                                media_ref = "interview_audio.wav",
                                start_frame = 30,
                                duration_frames = 450,
                                trim_start_frame = 0,
                                speed = 1.0,
                                volume = 0.9
                            }
                        }
                    },
                    new("text")
                    {
                        clips = new List<Clip>
                        {
                            new()
                            {
                                media_ref = "subtitle_english.txt",
                                start_frame = 60,
                                duration_frames = 90,
                                trim_start_frame = 0,
                                speed = 1.0,
                                text_content = "Welcome to Palmier Pro!"
                            }
                        }
                    }
                }
            };

            TimelineEditor.TimelineData = _timeline;
        }

        private void InitializeRustFFI()
        {
            try
            {
                string json = JsonSerializer.Serialize(_timeline, new JsonSerializerOptions
                {
                    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
                });

                _timelinePtr = NativeMethods.timeline_from_json(json, (nuint)System.Text.Encoding.UTF8.GetByteCount(json));

                if (_timelinePtr != IntPtr.Zero)
                {
                    // Register a fresh undo stack for this timeline instance.
                    NativeMethods.timeline_undo_stack_init(_timelinePtr);
                    FFIStatusText.Text = $"Rust FFI: Active (ptr: 0x{_timelinePtr.ToInt64():X})";
                    FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Green);
                }
                else
                {
                    FFIStatusText.Text = "Rust FFI: Error (timeline_from_json returned null)";
                    FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Red);
                }
            }
            catch (DllNotFoundException)
            {
                FFIStatusText.Text = "Rust FFI: Library 'palmier_engine.dll' not found";
                FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Red);
            }
            catch (Exception ex)
            {
                FFIStatusText.Text = $"Rust FFI Error: {ex.Message}";
                FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Red);
            }
        }

        private void FreeRustFFI()
        {
            if (_timelinePtr != IntPtr.Zero)
            {
                NativeMethods.timeline_undo_stack_free(_timelinePtr);
                NativeMethods.timeline_free(_timelinePtr);
                _timelinePtr = IntPtr.Zero;
            }
        }

        private void OnClipPositionChanged(Clip clip, long newStartFrame)
        {
            System.Diagnostics.Debug.WriteLine($"Clip '{clip.media_ref}' dragged to frame {newStartFrame}");

            if (_timelinePtr != IntPtr.Zero)
            {
                try
                {
                    string json = JsonSerializer.Serialize(_timeline, new JsonSerializerOptions
                    {
                        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
                    });

                    // Save mutated state to timeline.json
                    try
                    {
                        System.IO.File.WriteAllText("timeline.json", json);
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"[Sync] Failed to write timeline.json: {ex.Message}");
                    }

                    // Update the Rust-side timeline in-place. This diffs and pushes the operation to the Rust undo stack.
                    bool success = NativeMethods.timeline_update_from_json(_timelinePtr, json, (nuint)System.Text.Encoding.UTF8.GetByteCount(json));

                    if (success)
                    {
                        FFIStatusText.Text = $"Rust FFI: Active (Clip moved, ptr: 0x{_timelinePtr.ToInt64():X})";
                        FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Green);
                    }
                    else
                    {
                        FFIStatusText.Text = "Rust FFI: Mutation Error";
                        FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Red);
                    }
                }
                catch (Exception ex)
                {
                    FFIStatusText.Text = $"FFI Sync Error: {ex.Message}";
                    FFIStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.Red);
                }
            }
        }

        private void OnPlayheadPositionChanged(long frame)
        {
            double fps = _timeline.fps;
            long totalSeconds = (long)(frame / fps);
            long frames = frame % (long)fps;
            long hours = totalSeconds / 3600;
            long minutes = (totalSeconds % 3600) / 60;
            long seconds = totalSeconds % 60;

            PlayheadTimecode.Text = $"{hours:D2}:{minutes:D2}:{seconds:D2}.{frames:D2}";
        }

        private void RenderCurrentFrame()
        {
            if (!_isInitialized || _presenter == IntPtr.Zero || _engine == IntPtr.Zero) return;

            try
            {
                long frame = TimelineEditor.PlayheadFrame;
                nint output = NativeMethods.render_frame(_engine, _timelinePtr, frame);
                if (output == IntPtr.Zero) return;  // no clip at this frame — skip
                NativeMethods.presenter_present(_presenter, output);
                if (!_firstFrameLogged)
                {
                    _firstFrameLogged = true;
                    System.Diagnostics.Debug.WriteLine("[Tick] First frame rendered.");
                    Console.WriteLine("[Tick] First frame rendered.");
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[Render Error] {ex.Message}");
                Console.WriteLine($"[Render Error] {ex.Message}");
            }
        }

        private void PlaybackTimer_Tick(object sender, object e)
        {
            if (!_isInitialized || _engine == IntPtr.Zero || _presenter == IntPtr.Zero) return;
  
            try
            {
                long totalFrames = 1000; // fallback default
                if (_timelinePtr != IntPtr.Zero)
                {
                    totalFrames = NativeMethods.timeline_total_frames(_timelinePtr);
                }
  
                long currentFrame = TimelineEditor.PlayheadFrame + 1;

                // Soft A/V sync: if audio clock is more than 2 frames ahead,
                // nudge video forward to catch up (don't stall)
                if (_audioEngine != IntPtr.Zero)
                {
                    long audioFrame = NativeMethods.audio_engine_current_frame(_audioEngine);
                    if (audioFrame > currentFrame + 2)
                    {
                        currentFrame = audioFrame;
                    }
                }

                if (currentFrame >= totalFrames)
                {
                    Console.WriteLine($"[Tick] currentFrame ({currentFrame}) >= totalFrames ({totalFrames}). Stopping playback.");
                    if (_audioEngine != IntPtr.Zero)
                    {
                        NativeMethods.audio_engine_stop(_audioEngine);
                    }
                    _playbackTimer.Stop();
                    currentFrame = 0;
                    TimelineEditor.SetPlayheadFrame(0);
                    try { PlayButton.Content = "\uE768"; } catch {} // Play Icon
                }
                else
                {
                    TimelineEditor.SetPlayheadFrame(currentFrame);
                }
  
                RenderCurrentFrame();
  
                // Sync the UI state
                TimelineViewModel.CurrentFrame = (int)currentFrame;
                TimelineViewModel.TotalFrames = (int)totalFrames;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[Tick Error] {ex.Message}");
                Console.WriteLine($"[Tick Error] {ex.Message}");
                if (_audioEngine != IntPtr.Zero)
                {
                    NativeMethods.audio_engine_pause(_audioEngine);
                }
                _playbackTimer.Stop();
            }
        }

        private void OnPlayClick(object sender, RoutedEventArgs e)
        {
            var btn = (Button)sender;
            Console.WriteLine($"[Play] OnPlayClick: _audioEngine=0x{_audioEngine.ToInt64():X}, timer.IsEnabled={_playbackTimer.IsEnabled}, playhead={TimelineEditor.PlayheadFrame}");
            if (_playbackTimer.IsEnabled)
            {
                // Pause
                if (_audioEngine != IntPtr.Zero)
                {
                    Console.WriteLine("[Play] Calling audio_engine_pause.");
                    NativeMethods.audio_engine_pause(_audioEngine);
                }
                else
                {
                    Console.WriteLine("[Play] Skipping audio_engine_pause — _audioEngine is null.");
                }
                _playbackTimer.Stop();
                btn.Content = "\uE768"; // Play Icon
            }
            else
            {
                // Play
                if (_audioEngine != IntPtr.Zero)
                {
                    Console.WriteLine($"[Play] Calling audio_engine_play from frame {TimelineEditor.PlayheadFrame}.");
                    NativeMethods.audio_engine_play(_audioEngine, TimelineEditor.PlayheadFrame);
                    Console.WriteLine("[Play] audio_engine_play returned.");
                }
                else
                {
                    Console.WriteLine("[Play] Skipping audio_engine_play — _audioEngine is null. CPAL will NOT start.");
                }
                _playbackTimer.Start();
                btn.Content = "\uE769"; // Pause Icon
            }
        }

        private void OnPrevFrameClick(object sender, RoutedEventArgs e)
        {
            if (_audioEngine != IntPtr.Zero)
            {
                NativeMethods.audio_engine_pause(_audioEngine);
            }
            _playbackTimer.Stop();
            TimelineEditor.SetPlayheadFrame(TimelineEditor.PlayheadFrame - 1);
            RenderCurrentFrame();
            if (PlayButton != null) PlayButton.Content = "\uE768";
        }

        private void OnNextFrameClick(object sender, RoutedEventArgs e)
        {
            if (_audioEngine != IntPtr.Zero)
            {
                NativeMethods.audio_engine_pause(_audioEngine);
            }
            _playbackTimer.Stop();
            TimelineEditor.SetPlayheadFrame(TimelineEditor.PlayheadFrame + 1);
            RenderCurrentFrame();
            if (PlayButton != null) PlayButton.Content = "\uE768";
        }

        private void OnZoomSliderChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (TimelineEditor != null)
            {
                TimelineEditor.SetZoom(ZoomSlider.Value);
            }
        }

        /// Reload timeline from a JSON string and sync the UI.
        private void RefreshTimelineUIFromJson(string jsonStr)
        {
            try
            {
                var reloaded = JsonSerializer.Deserialize<Timeline>(jsonStr);
                if (reloaded == null) return;
                _timeline = reloaded;
                TimelineEditor.TimelineData = _timeline;

                // Sync the restored JSON to the Rust-side timeline pointer in-place.
                if (_timelinePtr != IntPtr.Zero)
                {
                    NativeMethods.timeline_update_from_json(_timelinePtr, jsonStr, (nuint)System.Text.Encoding.UTF8.GetByteCount(jsonStr));
                }

                // Persist to disk.
                System.IO.File.WriteAllText("timeline.json", jsonStr);

                RenderCurrentFrame();
                System.Diagnostics.Debug.WriteLine("[Undo/Redo] Timeline UI refreshed.");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[Undo/Redo] Refresh error: {ex.Message}");
            }
        }

        /// Reload timeline.json written by Rust after an undo/redo and sync the UI.
        private void RefreshTimelineUI()
        {
            try
            {
                string json = System.IO.File.ReadAllText("timeline.json");
                RefreshTimelineUIFromJson(json);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"[Undo/Redo] RefreshTimelineUI error: {ex.Message}");
            }
        }

        private void OnUndoClick(object sender, RoutedEventArgs e)
        {
            if (_timelinePtr == IntPtr.Zero) return;

            if (NativeMethods.timeline_undo(_timelinePtr))
            {
                RefreshTimelineUI();
            }
        }

        private void OnRedoClick(object sender, RoutedEventArgs e)
        {
            if (_timelinePtr == IntPtr.Zero) return;

            if (NativeMethods.timeline_redo(_timelinePtr))
            {
                RefreshTimelineUI();
            }
        }

        private async void OnImportClicked(object sender, RoutedEventArgs e)
        {
            try
            {
                Log("[Import] Button clicked");

                var picker = new Windows.Storage.Pickers.FileOpenPicker();
                picker.SuggestedStartLocation =
                    Windows.Storage.Pickers.PickerLocationId.VideosLibrary;
                picker.FileTypeFilter.Add(".mp4");
                picker.FileTypeFilter.Add(".mov");
                picker.FileTypeFilter.Add(".mkv");
                picker.FileTypeFilter.Add(".avi");
                picker.FileTypeFilter.Add(".mp3");
                picker.FileTypeFilter.Add(".wav");

                // CRITICAL: must use Window, not Page or App
                var hwnd = WinRT.Interop.WindowNative
                               .GetWindowHandle(App.MainAppWindow);
                Log($"[Import] hwnd = 0x{hwnd:X}");
                if (hwnd == IntPtr.Zero)
                {
                    Log("[Import] ERROR: hwnd is zero — window not ready");
                    return;
                }
                WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);

                Log("[Import] Showing picker...");
                var file = await picker.PickSingleFileAsync();
                Log($"[Import] Picker returned: {file?.Path ?? "null (cancelled)"}");
                if (file == null) return;

                var escapedPath = file.Path.Replace("\\", "\\\\");
                var body = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\"," +
                           "\"id\":1,\"params\":{\"name\":\"import_media\"," +
                           "\"arguments\":{\"source_path\":\"" + escapedPath + "\"}}}";

                using var http = new System.Net.Http.HttpClient();
                http.Timeout = TimeSpan.FromSeconds(10);
                Log($"[Import] POST to MCP: {body}");
                var resp = await http.PostAsync("http://127.0.0.1:19789/mcp",
                    new System.Net.Http.StringContent(
                        body, System.Text.Encoding.UTF8, "application/json"));
                var result = await resp.Content.ReadAsStringAsync();
                Log($"[Import] MCP response: {result}");
                // FileSystemWatcher auto-refreshes the browser
            }
            catch (Exception ex)
            {
                Log($"[Import] EXCEPTION {ex.GetType().Name}: {ex.Message}");
            }
        }

        private void OnMediaItemDragStarting(object sender,
            DragItemsStartingEventArgs e)
        {
            try
            {
                Log($"[Drag] DragItemsStarting fired, count={e.Items.Count}");
                if (e.Items.Count == 0) return;
                
                // Get the media item's ID
                var item = e.Items[0];
                string mediaId = "";
                if (item is MediaItem mi) mediaId = mi.Id;
                else if (item is string s) mediaId = s;
                else mediaId = item?.ToString() ?? "";
                
                Log($"[Drag] Setting mediaRef = {mediaId}");
                e.Data.SetText(mediaId);
                e.Data.RequestedOperation =
                    Windows.ApplicationModel.DataTransfer.DataPackageOperation.Copy;
            }
            catch (Exception ex)
            {
                Log($"[Drag] EXCEPTION: {ex}");
            }
        }

        private void OnTimelineDragOver(object sender, DragEventArgs e)
        {
            try
            {
                Log("[Drop] DragOver fired");
                e.AcceptedOperation =
                    Windows.ApplicationModel.DataTransfer.DataPackageOperation.Copy;
                e.DragUIOverride.Caption = "Add to Timeline";
                e.DragUIOverride.IsGlyphVisible = false;
                e.Handled = true;
            }
            catch (Exception ex)
            {
                Log($"[DragOver] EXCEPTION: {ex}");
            }
        }

        private async void OnTimelineDrop(object sender, DragEventArgs e)
        {
            Log("[Drop] Drop fired");
            try
            {
                if (!e.DataView.Contains(
                    Windows.ApplicationModel.DataTransfer
                           .StandardDataFormats.Text))
                {
                    Log("[Drop] No text data in drop");
                    return;
                }
                var mediaRef = await e.DataView.GetTextAsync();
                Log($"[Drop] mediaRef = {mediaRef}");
                e.Handled = true;

                // Drop position → frame number
                var pos = e.GetPosition(TimelineEditor);
                long startFrame = Math.Max(0,
                    (long)(pos.X / TimelineEditor.ZoomFactor));
                Log($"[Drop] startFrame = {startFrame}");

                // Call add_clips via MCP
                var body = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\"," +
                           "\"id\":1,\"params\":{\"name\":\"add_clips\"," +
                           "\"arguments\":{\"clips\":[{\"mediaRef\":\"" +
                           mediaRef + "\",\"startFrame\":" + startFrame +
                           ",\"durationFrames\":150}]}}}";
                Log($"[Drop] POST: {body}");
                using var http = new System.Net.Http.HttpClient();
                http.Timeout = TimeSpan.FromSeconds(5);
                var resp = await http.PostAsync("http://127.0.0.1:19789/mcp",
                    new System.Net.Http.StringContent(
                        body, System.Text.Encoding.UTF8, "application/json"));
                var result = await resp.Content.ReadAsStringAsync();
                Log($"[Drop] MCP response: {result}");
                RefreshTimelineUI();
            }
            catch (Exception ex)
            {
                Log($"[Drop] EXCEPTION: {ex}");
            }
        }

        private void OnDeleteAcceleratorInvoked(Microsoft.UI.Xaml.Input.KeyboardAccelerator sender, Microsoft.UI.Xaml.Input.KeyboardAcceleratorInvokedEventArgs args)
        {
            DeleteSelectedClip();
            args.Handled = true;
        }

        private async void DeleteSelectedClip()
        {
            var selectedId = TimelineEditor.SelectedClipId;
            if (selectedId != null && _timelinePtr != IntPtr.Zero)
            {
                Track? targetTrack = null;
                Clip? selectedClipObj = null;
                foreach (var track in _timeline.tracks)
                {
                    foreach (var clip in track.clips)
                    {
                        if (clip.id == selectedId)
                        {
                            targetTrack = track;
                            selectedClipObj = clip;
                            break;
                        }
                    }
                    if (targetTrack != null) break;
                }

                if (targetTrack != null && selectedClipObj != null)
                {
                    try
                    {
                        using var client = new System.Net.Http.HttpClient();
                        client.Timeout = TimeSpan.FromSeconds(5);
                        
                        var body = System.Text.Json.JsonSerializer.Serialize(new
                        {
                            jsonrpc = "2.0",
                            method = "tools/call",
                            id = 1,
                            @params = new
                            {
                                name = "apply_operations",
                                arguments = new
                                {
                                    operations = new[]
                                    {
                                        new
                                        {
                                            DeleteClip = new
                                            {
                                                track_id = targetTrack.id,
                                                clip = selectedClipObj
                                            }
                                        }
                                    }
                                }
                            }
                        });
                        
                        var content = new System.Net.Http.StringContent(
                            body, System.Text.Encoding.UTF8, "application/json");
                        var response = await client.PostAsync("http://127.0.0.1:19789/mcp", content);
                        var result = await response.Content.ReadAsStringAsync();
                        Debug.WriteLine($"[Delete] MCP response: {result}");
                        
                        // Mutate local C# timeline
                        targetTrack.clips.Remove(selectedClipObj);
                        _selectedClip = null;
                        
                        // Sync mutated C# timeline to FFI and save to file
                        string json = JsonSerializer.Serialize(_timeline, new JsonSerializerOptions
                        {
                            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
                        });
                        System.IO.File.WriteAllText("timeline.json", json);
                        NativeMethods.timeline_update_from_json(_timelinePtr, json, (nuint)System.Text.Encoding.UTF8.GetByteCount(json));

                        TimelineEditor.SelectedClipId = null;
                        RefreshTimelineUI();
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"[Delete] Failed: {ex.Message}");
                    }
                }
            }
        }

        private void OnPageKeyDown(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Delete || e.Key == Windows.System.VirtualKey.Back)
            {
                DeleteSelectedClip();
            }
        }
    }
 
    public static class TimelineViewModel
    {
        public static int CurrentFrame { get; set; }
        public static int TotalFrames { get; set; }
    }
}
