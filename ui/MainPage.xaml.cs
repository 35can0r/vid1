using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
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
        }

        private Timeline _timeline = new();
        private IntPtr _timelinePtr = IntPtr.Zero;
        private DispatcherTimer _playbackTimer = new();
        private IntPtr _engine = IntPtr.Zero;
        private IntPtr _presenter = IntPtr.Zero;
        private System.IO.FileSystemWatcher? _mediaWatcher = null;
        private bool _isInitialized = false;
        private bool _firstFrameLogged = false;



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

            System.Diagnostics.Debug.WriteLine($"[Init] renderer_create returned: 0x{_engine.ToInt64():X}");
            Console.WriteLine($"[Init] renderer_create returned: 0x{_engine.ToInt64():X}");

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
                        MediaListView.Items.Clear();
                        foreach (var entry in mediaEntries)
                        {
                            MediaListView.Items.Add(new MediaItem
                            {
                                Name = entry.Name,
                                Icon = "🎬",
                                Duration = ""
                            });
                        }
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
                if (output != IntPtr.Zero)
                {
                    NativeMethods.presenter_present(_presenter, output);
                    if (!_firstFrameLogged)
                    {
                        _firstFrameLogged = true;
                        System.Diagnostics.Debug.WriteLine("[Tick] First frame rendered.");
                        Console.WriteLine("[Tick] First frame rendered.");
                    }
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
            Console.WriteLine($"[Tick] Tick event triggered. _isInitialized={_isInitialized}, _engine={_engine.ToInt64():X}, _presenter={_presenter.ToInt64():X}");
            if (!_isInitialized || _engine == IntPtr.Zero || _presenter == IntPtr.Zero) return;
  
            try
            {
                long totalFrames = 1000; // fallback default
                if (_timelinePtr != IntPtr.Zero)
                {
                    totalFrames = NativeMethods.timeline_total_frames(_timelinePtr);
                }
                Console.WriteLine($"[Tick] totalFrames={totalFrames}, currentPlayheadFrame={TimelineEditor.PlayheadFrame}");
  
                long currentFrame = TimelineEditor.PlayheadFrame;
                if (currentFrame >= totalFrames)
                {
                    Console.WriteLine($"[Tick] currentFrame ({currentFrame}) >= totalFrames ({totalFrames}). Stopping playback.");
                    _playbackTimer.Stop();
                    currentFrame = 0;
                    TimelineEditor.SetPlayheadFrame(0);
                    try { PlayButton.Content = "\uE768"; } catch {} // Play Icon
                }
                else
                {
                    currentFrame += 1;
                    TimelineEditor.SetPlayheadFrame(currentFrame);
                    Console.WriteLine($"[Tick] Incremented playhead to frame {currentFrame}");
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
                _playbackTimer.Stop();
            }
        }

        private void OnPlayClick(object sender, RoutedEventArgs e)
        {
            var btn = (Button)sender;
            Console.WriteLine($"[PlayClick] Button clicked. _playbackTimer.IsEnabled={_playbackTimer.IsEnabled}, Interval={_playbackTimer.Interval.TotalMilliseconds}ms");
            if (_playbackTimer.IsEnabled)
            {
                _playbackTimer.Stop();
                btn.Content = "\uE768"; // Play Icon
            }
            else
            {
                _playbackTimer.Start();
                btn.Content = "\uE769"; // Pause Icon
            }
            Console.WriteLine($"[PlayClick] After action: _playbackTimer.IsEnabled={_playbackTimer.IsEnabled}");
        }

        private void OnPrevFrameClick(object sender, RoutedEventArgs e)
        {
            _playbackTimer.Stop();
            TimelineEditor.SetPlayheadFrame(TimelineEditor.PlayheadFrame - 1);
            RenderCurrentFrame();
        }

        private void OnNextFrameClick(object sender, RoutedEventArgs e)
        {
            _playbackTimer.Stop();
            TimelineEditor.SetPlayheadFrame(TimelineEditor.PlayheadFrame + 1);
            RenderCurrentFrame();
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

                // Rebuild the Rust-side timeline pointer from the restored JSON.
                if (_timelinePtr != IntPtr.Zero)
                {
                    NativeMethods.timeline_undo_stack_free(_timelinePtr);
                    NativeMethods.timeline_free(_timelinePtr);
                }
                _timelinePtr = NativeMethods.timeline_from_json(jsonStr,
                    (nuint)System.Text.Encoding.UTF8.GetByteCount(jsonStr));
                if (_timelinePtr != IntPtr.Zero)
                    NativeMethods.timeline_undo_stack_init(_timelinePtr);

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
    }
 
    public static class TimelineViewModel
    {
        public static int CurrentFrame { get; set; }
        public static int TotalFrames { get; set; }
    }
}
