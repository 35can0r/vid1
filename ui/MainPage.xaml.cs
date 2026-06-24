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

        public MainPage()
        {
            InitializeComponent();
            Loaded += MainPage_Loaded;
            Unloaded += MainPage_Unloaded;

            // Setup Playback Timer (30fps = 33.3ms interval)
            _playbackTimer.Interval = TimeSpan.FromMilliseconds(33.3);
            _playbackTimer.Tick += PlaybackTimer_Tick;
        }

        private void MainPage_Loaded(object sender, RoutedEventArgs e)
        {
            InitializeMediaBrowser();
            InitializeMockTimeline();
            InitializeRustFFI();

            // 1. Boot the C++ DX12 Rendering Engine
            _engine = NativeMethods.renderer_create(_timeline.width, _timeline.height);

            // 2. Hook the C++ Render Engine to the WinUI 3 SwapChainPanel
            var panelNative = PreviewCanvas.As<PalmierPro.Engine.ISwapChainPanelNative>();
            nint panelPtr = Marshal.GetIUnknownForObject(panelNative);

            _presenter = NativeMethods.presenter_create(panelPtr, _engine, _timeline.width, _timeline.height);

            // Connect event handlers
            TimelineEditor.ClipPositionChanged += OnClipPositionChanged;
            TimelineEditor.PlayheadPositionChanged += OnPlayheadPositionChanged;
            
            // Set initial playhead timecode
            OnPlayheadPositionChanged(TimelineEditor.PlayheadFrame);
            RenderCurrentFrame();
        }

        private void MainPage_Unloaded(object sender, RoutedEventArgs e)
        {
            FreeRustFFI();
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
            var items = new List<MediaItem>
            {
                new() { Name = "opening_shot.mp4", Duration = "00:00:05.00 (150 frames)", Icon = "\uE714" },
                new() { Name = "interview_audio.wav", Duration = "00:00:15.00 (450 frames)", Icon = "\uE712" },
                new() { Name = "b-roll_city.mp4", Duration = "00:00:10.00 (300 frames)", Icon = "\uE714" },
                new() { Name = "subtitle_english.txt", Duration = "00:00:03.00 (90 frames)", Icon = "\uE8A5" }
            };
            MediaListView.ItemsSource = items;
        }

        private void InitializeMockTimeline()
        {
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

                    // Free old FFI timeline instance and allocate new one representing the mutated state
                    NativeMethods.timeline_free(_timelinePtr);
                    _timelinePtr = NativeMethods.timeline_from_json(json, (nuint)System.Text.Encoding.UTF8.GetByteCount(json));

                    if (_timelinePtr != IntPtr.Zero)
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
            if (_presenter != IntPtr.Zero && _engine != IntPtr.Zero)
            {
                long frame = TimelineEditor.PlayheadFrame;
                nint output = NativeMethods.render_frame(_engine, frame);
                NativeMethods.presenter_present(_presenter, output);
            }
        }

        private void PlaybackTimer_Tick(object sender, object e)
        {
            TimelineEditor.SetPlayheadFrame(TimelineEditor.PlayheadFrame + 1);
            RenderCurrentFrame();
        }

        private void OnPlayClick(object sender, RoutedEventArgs e)
        {
            var btn = (Button)sender;
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

        private void OnUndoClick(object sender, RoutedEventArgs e)
        {
            // Transaction-based Undo trigger mock
            System.Diagnostics.Debug.WriteLine("Undo transaction triggered");
        }

        private void OnRedoClick(object sender, RoutedEventArgs e)
        {
            // Transaction-based Redo trigger mock
            System.Diagnostics.Debug.WriteLine("Redo transaction triggered");
        }
    }
}
