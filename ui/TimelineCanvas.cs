using System;
using System.Collections.Generic;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using PalmierPro.UI.Models;
using Windows.Foundation;
using Windows.UI;

namespace PalmierPro.UI.Controls
{
    public class TimelineCanvas : Canvas
    {
        public static readonly DependencyProperty TimelineProperty =
            DependencyProperty.Register(nameof(TimelineData), typeof(Timeline), typeof(TimelineCanvas),
                new PropertyMetadata(null, OnTimelineChanged));

        public Timeline TimelineData
        {
            get => (Timeline)GetValue(TimelineProperty);
            set => SetValue(TimelineProperty, value);
        }

        public double ZoomFactor { get; set; } = 2.0; // Pixels per frame
        public long PlayheadFrame { get; set; } = 0;
        public Func<string, string?>? GetMediaNameCallback { get; set; }
        public Guid? SelectedClipId { get; set; }
 
        public event Action<Clip, long>? ClipPositionChanged;
        public event Action<long>? PlayheadPositionChanged;
        public event Action<Clip?>? ClipSelected;

        private const double TrackHeight = 70;
        private const double TrackSpacing = 10;
        private const double HeaderHeight = 40;

        private Border? _playheadLine;
        private Border? _playheadHandle;
        private Border? _headerBackground;
        private readonly List<FrameworkElement> _trackBackgrounds = new();
        private readonly List<FrameworkElement> _ticks = new();

        public TimelineCanvas()
        {
            Background = new SolidColorBrush(Color.FromArgb(255, 24, 24, 26)); // Dark background
            PointerWheelChanged += OnPointerWheelChanged;
            PointerPressed += OnCanvasPointerPressed;
            PointerMoved += OnCanvasPointerMoved;
            PointerReleased += OnCanvasPointerReleased;
            SizeChanged += (s, e) => RebuildTimelineUI();
        }

        private static void OnTimelineChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is TimelineCanvas canvas)
            {
                canvas.RebuildTimelineUI();
            }
        }

        public void SetZoom(double newZoom)
        {
            ZoomFactor = Math.Clamp(newZoom, 0.1, 20.0);
            RebuildTimelineUI();
        }

        public void SetPlayheadFrame(long frame)
        {
            PlayheadFrame = Math.Max(0, frame);
            UpdatePlayheadPosition();
            PlayheadPositionChanged?.Invoke(PlayheadFrame);
        }

        private bool _isDraggingPlayhead = false;

        private void OnCanvasPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            var pt = e.GetCurrentPoint(this);
            if (pt.Position.Y < HeaderHeight)
            {
                // Clicked on header: position playhead
                CapturePointer(e.Pointer);
                _isDraggingPlayhead = true;
                long frame = (long)Math.Round(pt.Position.X / ZoomFactor);
                SetPlayheadFrame(frame);
                e.Handled = true;
            }
            else
            {
                // Clicked on canvas body: clear selected clip
                SelectedClipId = null;
                ClipSelected?.Invoke(null);
                RebuildTimelineUI();
            }
        }

        private void OnCanvasPointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingPlayhead)
            {
                var pt = e.GetCurrentPoint(this);
                long frame = (long)Math.Round(pt.Position.X / ZoomFactor);
                SetPlayheadFrame(frame);
                e.Handled = true;
            }
        }

        private void OnCanvasPointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingPlayhead)
            {
                ReleasePointerCapture(e.Pointer);
                _isDraggingPlayhead = false;
                e.Handled = true;
            }
        }

        private void OnPointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            var pt = e.GetCurrentPoint(this);
            var keyModifiers = e.KeyModifiers;
            if (keyModifiers.HasFlag(Windows.System.VirtualKeyModifiers.Control))
            {
                // Ctrl + MouseWheel: Zoom
                double oldZoom = ZoomFactor;
                double multiplier = pt.Properties.MouseWheelDelta > 0 ? 1.15 : 1.0 / 1.15;
                SetZoom(ZoomFactor * multiplier);
                
                // Adjust horizontal scroll to keep mouse position stable if parent is ScrollViewer
                e.Handled = true;
            }
        }

        public void RebuildTimelineUI()
        {
            Children.Clear();
            _trackBackgrounds.Clear();
            _ticks.Clear();

            if (TimelineData == null) return;

            double canvasWidth = Math.Max(ActualWidth, 3000);
            if (TimelineData.tracks.Count > 0)
            {
                long maxFrame = 0;
                foreach (var track in TimelineData.tracks)
                {
                    foreach (var clip in track.clips)
                    {
                        maxFrame = Math.Max(maxFrame, clip.start_frame + clip.duration_frames);
                    }
                }
                canvasWidth = Math.Max(canvasWidth, (maxFrame + 200) * ZoomFactor);
            }

            // Adjust Canvas Size
            Width = canvasWidth;
            Height = HeaderHeight + TimelineData.tracks.Count * (TrackHeight + TrackSpacing) + 50;

            // 1. Draw Header Background
            _headerBackground = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(255, 36, 36, 38)),
                Height = HeaderHeight,
                Width = canvasWidth
            };
            SetTop(_headerBackground, 0);
            SetLeft(_headerBackground, 0);
            Children.Add(_headerBackground);

            // 2. Draw Ticks on Header
            long tickStep = ZoomFactor < 0.5 ? 150 : (ZoomFactor < 1.5 ? 60 : 30);
            for (long frame = 0; frame * ZoomFactor < canvasWidth; frame += tickStep)
            {
                var tick = new Border
                {
                    Width = 1,
                    Height = frame % (tickStep * 2) == 0 ? 15 : 8,
                    Background = new SolidColorBrush(Color.FromArgb(120, 255, 255, 255))
                };
                SetLeft(tick, frame * ZoomFactor);
                SetTop(tick, HeaderHeight - tick.Height);
                Children.Add(tick);
                _ticks.Add(tick);

                if (frame % (tickStep * 2) == 0)
                {
                    var text = new TextBlock
                    {
                        Text = $"{frame}",
                        FontSize = 9,
                        Foreground = new SolidColorBrush(Color.FromArgb(180, 255, 255, 255))
                    };
                    SetLeft(text, frame * ZoomFactor + 4);
                    SetTop(text, 5);
                    Children.Add(text);
                }
            }

            // 3. Draw Track Backgrounds
            for (int i = 0; i < TimelineData.tracks.Count; i++)
            {
                var bg = new Border
                {
                    Background = new SolidColorBrush(i % 2 == 0 ? Color.FromArgb(255, 30, 30, 32) : Color.FromArgb(255, 24, 24, 26)),
                    Height = TrackHeight,
                    Width = canvasWidth,
                    BorderThickness = new Thickness(0, 1, 0, 1),
                    BorderBrush = new SolidColorBrush(Color.FromArgb(20, 255, 255, 255))
                };
                SetTop(bg, HeaderHeight + i * (TrackHeight + TrackSpacing));
                SetLeft(bg, 0);
                Children.Add(bg);
                _trackBackgrounds.Add(bg);
            }

            // 4. Draw Clips
            for (int trackIndex = 0; trackIndex < TimelineData.tracks.Count; trackIndex++)
            {
                var track = TimelineData.tracks[trackIndex];
                foreach (var clip in track.clips)
                {
                    bool isSelected = clip.id == SelectedClipId;
                    var border = new Border
                    {
                        CornerRadius = new CornerRadius(6),
                        BorderThickness = isSelected ? new Thickness(2.5) : new Thickness(1),
                        BorderBrush = isSelected ? new SolidColorBrush(Colors.Yellow) : new SolidColorBrush(Color.FromArgb(50, 255, 255, 255))
                    };

                    // Alternate colors based on track type
                    if (track.track_type == "video")
                        border.Background = new SolidColorBrush(Color.FromArgb(255, 42, 103, 163));
                    else if (track.track_type == "audio")
                        border.Background = new SolidColorBrush(Color.FromArgb(255, 46, 125, 50));
                    else
                        border.Background = new SolidColorBrush(Color.FromArgb(255, 123, 42, 105));

                    var grid = new Grid();
                    var textBlock = new TextBlock
                    {
                        Text = GetMediaNameCallback?.Invoke(clip.media_ref) ?? clip.media_ref,
                        Foreground = new SolidColorBrush(Colors.White),
                        FontSize = 11,
                        FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                        VerticalAlignment = VerticalAlignment.Center,
                        HorizontalAlignment = HorizontalAlignment.Left,
                        Margin = new Thickness(10, 0, 10, 0),
                        TextTrimming = TextTrimming.CharacterEllipsis
                    };
                    grid.Children.Add(textBlock);
                    border.Child = grid;

                    // Set coordinates
                    SetLeft(border, clip.start_frame * ZoomFactor);
                    SetTop(border, HeaderHeight + trackIndex * (TrackHeight + TrackSpacing) + 5);
                    border.Width = Math.Max(10, clip.duration_frames * ZoomFactor);
                    border.Height = TrackHeight - 10;

                    // Drag-and-Drop Variables
                    bool isDragging = false;
                    Point initialMousePosition = new Point();
                    long initialStartFrame = 0;

                    border.PointerPressed += (s, ev) =>
                    {
                        SelectedClipId = clip.id;
                        ClipSelected?.Invoke(clip);
                        RebuildTimelineUI();

                        border.CapturePointer(ev.Pointer);
                        isDragging = true;
                        initialMousePosition = ev.GetCurrentPoint(this).Position;
                        initialStartFrame = clip.start_frame;
                        ev.Handled = true;
                    };

                    border.PointerMoved += (s, ev) =>
                    {
                        if (isDragging)
                        {
                            var currentMousePosition = ev.GetCurrentPoint(this).Position;
                            double deltaX = currentMousePosition.X - initialMousePosition.X;
                            long deltaFrames = (long)Math.Round(deltaX / ZoomFactor);
                            long newStartFrame = Math.Max(0, initialStartFrame + deltaFrames);

                            double relativeY = currentMousePosition.Y - HeaderHeight - 5;
                            int newTrackIdx = (int)Math.Round(relativeY / (TrackHeight + TrackSpacing));
                            newTrackIdx = Math.Max(0, Math.Min(newTrackIdx, TimelineData.tracks.Count - 1));

                            bool trackChanged = false;
                            int oldTrackIdx = -1;

                            for (int ti = 0; ti < TimelineData.tracks.Count; ti++)
                            {
                                if (TimelineData.tracks[ti].clips.Contains(clip))
                                {
                                    oldTrackIdx = ti;
                                    break;
                                }
                            }

                            if (oldTrackIdx != -1 && newTrackIdx != oldTrackIdx)
                            {
                                TimelineData.tracks[oldTrackIdx].clips.Remove(clip);
                                TimelineData.tracks[newTrackIdx].clips.Add(clip);
                                trackChanged = true;
                            }

                            if (newStartFrame != clip.start_frame || trackChanged)
                            {
                                clip.start_frame = newStartFrame;
                                SetLeft(border, clip.start_frame * ZoomFactor);
                                SetTop(border, HeaderHeight + newTrackIdx * (TrackHeight + TrackSpacing) + 5);
                                ClipPositionChanged?.Invoke(clip, clip.start_frame);
                            }
                            ev.Handled = true;
                        }
                    };

                    border.PointerReleased += (s, ev) =>
                    {
                        if (isDragging)
                        {
                            border.ReleasePointerCapture(ev.Pointer);
                            isDragging = false;
                            ev.Handled = true;
                            RebuildTimelineUI(); // Snap and refresh
                        }
                    };

                    Children.Add(border);
                }
            }

            // 5. Draw Playhead
            _playheadLine = new Border
            {
                Width = 2,
                Background = new SolidColorBrush(Colors.Red),
                Height = Height
            };
            Children.Add(_playheadLine);

            _playheadHandle = new Border
            {
                Width = 14,
                Height = 14,
                CornerRadius = new CornerRadius(7),
                Background = new SolidColorBrush(Colors.Red)
            };
            Children.Add(_playheadHandle);

            UpdatePlayheadPosition();
        }

        private void UpdatePlayheadPosition()
        {
            if (_playheadLine != null && _playheadHandle != null)
            {
                SetLeft(_playheadLine, PlayheadFrame * ZoomFactor - 1);
                SetTop(_playheadLine, 0);

                SetLeft(_playheadHandle, PlayheadFrame * ZoomFactor - 7);
                SetTop(_playheadHandle, HeaderHeight - 7);
            }
        }
    }
}
