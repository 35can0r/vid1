use serde::{Deserialize, Serialize};
use uuid::Uuid;

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Timeline {
    pub width: u32,
    pub height: u32,
    pub fps: f64,
    pub tracks: Vec<Track>,
}

impl Timeline {
    pub fn new(width: u32, height: u32, fps: f64) -> Self {
        Self {
            width,
            height,
            fps,
            tracks: Vec::new(),
        }
    }

    pub fn total_frames(&self) -> i64 {
        self.tracks
            .iter()
            .map(|t| t.end_frame())
            .max()
            .unwrap_or(0)
    }
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Track {
    pub id: Uuid,
    pub track_type: ClipType,
    pub muted: bool,
    pub hidden: bool,
    pub sync_locked: bool,
    pub clips: Vec<Clip>,
}

impl Track {
    pub fn new(track_type: ClipType) -> Self {
        Self {
            id: Uuid::new_v4(),
            track_type,
            muted: false,
            hidden: false,
            sync_locked: false,
            clips: Vec::new(),
        }
    }

    pub fn end_frame(&self) -> i64 {
        self.clips
            .iter()
            .map(|c| c.end_frame())
            .max()
            .unwrap_or(0)
    }
}

#[derive(Debug, Serialize, Deserialize, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ClipType {
    Video,
    Audio,
    Image,
    Text,
    Lottie,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Clip {
    pub id: Uuid,
    pub media_ref: String,

    // Timing
    pub start_frame: i64,
    pub duration_frames: i64,
    pub trim_start_frame: i64,
    #[serde(default = "default_speed")]
    pub speed: f64,

    // Payload depending on clip type
    #[serde(flatten)]
    pub content: ClipContent,

    pub link_group_id: Option<Uuid>,
}

impl Clip {
    pub fn end_frame(&self) -> i64 {
        self.start_frame + self.duration_frames
    }

    pub fn source_media_consumed(&self) -> i64 {
        (self.duration_frames as f64 * self.speed).round() as i64
    }
}

fn default_speed() -> f64 { 1.0 }
fn default_opacity() -> f64 { 1.0 }
fn default_volume() -> f64 { 1.0 }

#[derive(Debug, Serialize, Deserialize, Clone)]
#[serde(untagged)]
pub enum ClipContent {
    Video {
        #[serde(default)]
        transform: Transform,
        #[serde(default)]
        crop: Crop,
        #[serde(default = "default_opacity")]
        opacity: f64,
    },
    Audio {
        #[serde(default = "default_volume")]
        volume: f64,
    },
    Text {
        text_content: String,
        // text_style: TextStyle, // simplified for scaffolding
    },
}

#[derive(Debug, Serialize, Deserialize, Clone, Copy)]
pub struct Transform {
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
}

impl Default for Transform {
    fn default() -> Self {
        Self {
            center_x: 0.5,
            center_y: 0.5,
            width: 1.0,
            height: 1.0,
            rotation: 0.0,
        }
    }
}

#[derive(Debug, Serialize, Deserialize, Clone, Copy, Default)]
pub struct Crop {
    pub left: f64,
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
}
