use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use uuid::Uuid;

// Lerp trait
pub trait Lerp {
    fn lerp(&self, other: &Self, t: f64) -> Self;
}

impl Lerp for f64 {
    fn lerp(&self, other: &Self, t: f64) -> Self {
        self + (other - self) * t
    }
}

impl<A: Lerp, B: Lerp> Lerp for (A, B) {
    fn lerp(&self, other: &Self, t: f64) -> Self {
        (self.0.lerp(&other.0, t), self.1.lerp(&other.1, t))
    }
}

// Bounding layouts and coordinates
#[derive(Debug, Serialize, Deserialize, Clone, Copy, Default, PartialEq)]
pub struct BoundingLayout {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

impl Lerp for BoundingLayout {
    fn lerp(&self, other: &Self, t: f64) -> Self {
        Self {
            x: self.x.lerp(&other.x, t),
            y: self.y.lerp(&other.y, t),
            width: self.width.lerp(&other.width, t),
            height: self.height.lerp(&other.height, t),
        }
    }
}

#[derive(Debug, Serialize, Deserialize, Clone, Copy, PartialEq)]
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

impl Lerp for Transform {
    fn lerp(&self, other: &Self, t: f64) -> Self {
        Self {
            center_x: self.center_x.lerp(&other.center_x, t),
            center_y: self.center_y.lerp(&other.center_y, t),
            width: self.width.lerp(&other.width, t),
            height: self.height.lerp(&other.height, t),
            rotation: self.rotation.lerp(&other.rotation, t),
        }
    }
}

#[derive(Debug, Serialize, Deserialize, Clone, Copy, Default, PartialEq)]
pub struct Crop {
    pub left: f64,
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
}

impl Lerp for Crop {
    fn lerp(&self, other: &Self, t: f64) -> Self {
        Self {
            left: self.left.lerp(&other.left, t),
            top: self.top.lerp(&other.top, t),
            right: self.right.lerp(&other.right, t),
            bottom: self.bottom.lerp(&other.bottom, t),
        }
    }
}

// Animation and Keyframes
#[derive(Debug, Serialize, Deserialize, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Interpolation {
    Linear,
    Hold,
    Smooth,
}

#[derive(Debug, Serialize, Deserialize, Clone, PartialEq)]
pub struct Keyframe<T> {
    pub value: T,
    pub interpolation_out: Interpolation,
}

#[derive(Debug, Serialize, Deserialize, Clone, Default, PartialEq)]
pub struct KeyframeTrack<T> {
    pub keyframes: BTreeMap<i64, Keyframe<T>>,
}

impl<T> KeyframeTrack<T> {
    pub fn new() -> Self {
        Self {
            keyframes: BTreeMap::new(),
        }
    }
}

impl<T: Lerp + Clone> KeyframeTrack<T> {
    pub fn evaluate(&self, frame: i64) -> Option<T> {
        if self.keyframes.is_empty() {
            return None;
        }
        
        let mut before = self.keyframes.range(..=frame);
        let mut after = self.keyframes.range(frame..);
        
        let k1 = before.next_back();
        let k2 = after.next();
        
        match (k1, k2) {
            (None, Some((_, first_kf))) => Some(first_kf.value.clone()),
            (Some((_, last_kf)), None) => Some(last_kf.value.clone()),
            (Some((f1, k1_kf)), Some((f2, k2_kf))) => {
                if f1 == f2 {
                    return Some(k1_kf.value.clone());
                }
                let t = (frame - *f1) as f64 / (*f2 - *f1) as f64;
                match k1_kf.interpolation_out {
                    Interpolation::Hold => Some(k1_kf.value.clone()),
                    Interpolation::Linear => Some(k1_kf.value.lerp(&k2_kf.value, t)),
                    Interpolation::Smooth => {
                        let t_smooth = t * t * (3.0 - 2.0 * t);
                        Some(k1_kf.value.lerp(&k2_kf.value, t_smooth))
                    }
                }
            }
            (None, None) => None,
        }
    }
}

// Timeline structure definitions
#[derive(Debug, Serialize, Deserialize, Clone, PartialEq)]
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

#[derive(Debug, Serialize, Deserialize, Clone, PartialEq)]
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

#[derive(Debug, Serialize, Deserialize, Clone, PartialEq)]
pub struct Clip {
    pub id: Uuid,
    pub media_ref: String,

    // Timing
    pub start_frame: i64,
    pub duration_frames: i64,
    pub trim_start_frame: i64,
    #[serde(default = "default_speed", skip_serializing_if = "is_default_speed")]
    pub speed: f64,

    // Payload depending on clip type
    #[serde(flatten)]
    pub content: ClipContent,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub link_group_id: Option<Uuid>,
}

impl Clip {
    pub fn end_frame(&self) -> i64 {
        self.start_frame + self.duration_frames
    }

    pub fn source_media_consumed(&self) -> i64 {
        (self.duration_frames as f64 * self.speed).round() as i64
    }

    pub fn opacity(&self, frame: i64) -> f64 {
        match &self.content {
            ClipContent::Video { opacity, opacity_track, .. } => {
                opacity_track.as_ref()
                    .and_then(|t| t.evaluate(frame))
                    .unwrap_or(*opacity)
            }
            _ => 1.0,
        }
    }

    pub fn volume(&self, frame: i64) -> f64 {
        match &self.content {
            ClipContent::Audio { volume, volume_track } => {
                volume_track.as_ref()
                    .and_then(|t| t.evaluate(frame))
                    .unwrap_or(*volume)
            }
            _ => 1.0,
        }
    }

    pub fn transform(&self, frame: i64) -> Transform {
        match &self.content {
            ClipContent::Video { transform, transform_track, .. } => {
                transform_track.as_ref()
                    .and_then(|t| t.evaluate(frame))
                    .unwrap_or(*transform)
            }
            _ => Transform::default(),
        }
    }

    pub fn crop(&self, frame: i64) -> Crop {
        match &self.content {
            ClipContent::Video { crop, crop_track, .. } => {
                crop_track.as_ref()
                    .and_then(|t| t.evaluate(frame))
                    .unwrap_or(*crop)
            }
            _ => Crop::default(),
        }
    }
}

fn default_speed() -> f64 { 1.0 }
fn default_opacity() -> f64 { 1.0 }
fn default_volume() -> f64 { 1.0 }

pub fn is_default_speed(s: &f64) -> bool { *s == 1.0 }
pub fn is_default_opacity(o: &f64) -> bool { *o == 1.0 }
pub fn is_default_volume(v: &f64) -> bool { *v == 1.0 }

pub fn is_default_transform(t: &Transform) -> bool {
    t.center_x == 0.5 && t.center_y == 0.5 && t.width == 1.0 && t.height == 1.0 && t.rotation == 0.0
}

pub fn is_default_crop(c: &Crop) -> bool {
    c.left == 0.0 && c.top == 0.0 && c.right == 0.0 && c.bottom == 0.0
}

pub fn is_empty_transform_track(t: &Option<KeyframeTrack<Transform>>) -> bool {
    t.as_ref().map_or(true, |track| track.keyframes.is_empty())
}

pub fn is_empty_crop_track(c: &Option<KeyframeTrack<Crop>>) -> bool {
    c.as_ref().map_or(true, |track| track.keyframes.is_empty())
}

pub fn is_empty_f64_track(t: &Option<KeyframeTrack<f64>>) -> bool {
    t.as_ref().map_or(true, |track| track.keyframes.is_empty())
}

#[derive(Debug, Serialize, Deserialize, Clone, PartialEq)]
pub struct TextGroupItem {
    pub start_frame: i64,
    pub duration_frames: i64,
    pub text_content: String,
}

#[derive(Debug, Serialize, Deserialize, Clone, PartialEq)]
#[serde(untagged)]
pub enum ClipContent {
    Video {
        #[serde(default, skip_serializing_if = "is_default_transform")]
        transform: Transform,
        #[serde(default, skip_serializing_if = "is_default_crop")]
        crop: Crop,
        #[serde(default = "default_opacity", skip_serializing_if = "is_default_opacity")]
        opacity: f64,
        #[serde(default, skip_serializing_if = "is_empty_transform_track")]
        transform_track: Option<KeyframeTrack<Transform>>,
        #[serde(default, skip_serializing_if = "is_empty_crop_track")]
        crop_track: Option<KeyframeTrack<Crop>>,
        #[serde(default, skip_serializing_if = "is_empty_f64_track")]
        opacity_track: Option<KeyframeTrack<f64>>,
    },
    Audio {
        #[serde(default = "default_volume", skip_serializing_if = "is_default_volume")]
        volume: f64,
        #[serde(default, skip_serializing_if = "is_empty_f64_track")]
        volume_track: Option<KeyframeTrack<f64>>,
    },
    Text {
        text_content: String,
    },
    TextGroup {
        #[serde(rename = "text_group")]
        items: Vec<TextGroupItem>,
    },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_lerp_f64() {
        assert_eq!(0.0.lerp(&10.0, 0.5), 5.0);
        assert_eq!(2.0.lerp(&4.0, 0.25), 2.5);
    }

    #[test]
    fn test_lerp_pair() {
        let p1 = (0.0, 10.0);
        let p2 = (10.0, 20.0);
        assert_eq!(p1.lerp(&p2, 0.5), (5.0, 15.0));
    }

    #[test]
    fn test_lerp_bounding_layout() {
        let b1 = BoundingLayout { x: 0.0, y: 0.0, width: 100.0, height: 100.0 };
        let b2 = BoundingLayout { x: 10.0, y: 20.0, width: 200.0, height: 300.0 };
        let res = b1.lerp(&b2, 0.5);
        assert_eq!(res, BoundingLayout { x: 5.0, y: 10.0, width: 150.0, height: 200.0 });
    }

    #[test]
    fn test_keyframe_track_evaluation() {
        let mut track = KeyframeTrack::<f64>::new();
        
        // Linear
        track.keyframes.insert(0, Keyframe { value: 0.0, interpolation_out: Interpolation::Linear });
        track.keyframes.insert(10, Keyframe { value: 100.0, interpolation_out: Interpolation::Linear });
        
        // Hold
        track.keyframes.insert(20, Keyframe { value: 200.0, interpolation_out: Interpolation::Hold });
        track.keyframes.insert(30, Keyframe { value: 300.0, interpolation_out: Interpolation::Linear });

        // Smooth
        track.keyframes.insert(40, Keyframe { value: 400.0, interpolation_out: Interpolation::Smooth });
        track.keyframes.insert(50, Keyframe { value: 500.0, interpolation_out: Interpolation::Linear });

        // Test before first
        assert_eq!(track.evaluate(-5), Some(0.0));
        // Test exactly first
        assert_eq!(track.evaluate(0), Some(0.0));
        // Test linear mid-point
        assert_eq!(track.evaluate(5), Some(50.0));
        // Test hold mid-point
        assert_eq!(track.evaluate(25), Some(200.0));
        // Test smooth mid-point (smoothstep(0.5) is 0.5 * 0.5 * (3 - 2 * 0.5) = 0.25 * 2 = 0.5)
        assert_eq!(track.evaluate(45), Some(450.0));
        // Test after last
        assert_eq!(track.evaluate(60), Some(500.0));
    }
}

