using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PalmierPro.UI.Models
{
    public class Timeline
    {
        public uint width { get; set; } = 1920;
        public uint height { get; set; } = 1080;
        public double fps { get; set; } = 30.0;
        public List<Track> tracks { get; set; } = new();
    }

    public class Track
    {
        public Guid id { get; set; } = Guid.NewGuid();
        public string track_type { get; set; } = "video";
        public bool muted { get; set; }
        public bool hidden { get; set; }
        public bool sync_locked { get; set; }
        public List<Clip> clips { get; set; } = new();

        public Track() { }

        public Track(string type)
        {
            track_type = type.ToLower();
        }
    }

    public class Clip
    {
        public Guid id { get; set; } = Guid.NewGuid();
        public string media_ref { get; set; } = "";
        public long start_frame { get; set; }
        public long duration_frames { get; set; }
        public long trim_start_frame { get; set; }
        public double speed { get; set; } = 1.0;

        // Flattened ClipContent fields
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public double? opacity { get; set; }

        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public double? volume { get; set; }

        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public string? text_content { get; set; }

        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public Transform? transform { get; set; }

        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public Crop? crop { get; set; }
    }

    public class Transform
    {
        public double center_x { get; set; } = 0.5;
        public double center_y { get; set; } = 0.5;
        public double width { get; set; } = 1.0;
        public double height { get; set; } = 1.0;
        public double rotation { get; set; } = 0.0;
    }

    public class Crop
    {
        public double left { get; set; }
        public double top { get; set; }
        public double right { get; set; }
        public double bottom { get; set; }
    }
}
