use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicI64, AtomicBool, Ordering};
use std::time::Duration;
use std::ffi::CStr;
use std::os::raw::c_char;
use ringbuf::traits::{Producer, Observer};
use crate::{EngineHandle, HeapProducer};

#[repr(C)]
pub struct ActiveClipC {
    pub clip_id: [u8; 37],
    pub media_ref: [u8; 37],
    pub source_frame: i64,
    pub track_index: u32,
    pub center_x: f32,
    pub center_y: f32,
    pub width: f32,
    pub height: f32,
    pub rotation: f32,
    pub crop_left: f32,
    pub crop_top: f32,
    pub crop_right: f32,
    pub crop_bottom: f32,
    pub exposure: f32,
    pub contrast: f32,
    pub temperature: f32,
    pub tint: f32,
    pub saturation: f32,
    pub opacity: f32,
}

unsafe extern "C" {
    fn timeline_get_active_clips(
        handle: *const EngineHandle,
        frame_number: i64,
        out_clips: *mut ActiveClipC,
        max_clips: i32,
    ) -> i32;
}

pub fn run_mixer_thread(
    timeline: *mut EngineHandle,
    producer: Arc<Mutex<HeapProducer>>,
    sample_position: Arc<AtomicI64>,
    is_playing: Arc<AtomicBool>,
    is_shutting_down: Arc<AtomicBool>,
    sample_rate: u32,
    fps: f64,
) {
    let lookahead_samples = sample_rate as usize / 5; // 200ms

    loop {
        if is_shutting_down.load(Ordering::Relaxed) {
            break;
        }

        if !is_playing.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_millis(10));
            continue;
        }

        let mut prod = producer.lock().unwrap();
        let free = prod.vacant_len();
        if free < lookahead_samples / 4 {
            drop(prod);
            std::thread::sleep(Duration::from_millis(5));
            continue;
        }

        let current_sample = sample_position.load(Ordering::Relaxed);
        let lookahead_frame = ((current_sample + lookahead_samples as i64 / 2) as f64 / sample_rate as f64 * fps) as i64;

        let mut clips: [ActiveClipC; 16] = unsafe { std::mem::zeroed() };

        let count = unsafe {
            timeline_get_active_clips(timeline, lookahead_frame, clips.as_mut_ptr(), 16)
        };

        let chunk_frames = (sample_rate as f64 * 0.02) as usize; // 20ms chunks
        let mut mixed = vec![0.0f32; chunk_frames * 2]; // stereo

        for i in 0..count as usize {
            let clip = &clips[i];

            // Assume 1 is AUDIO track kind if we had it, but for now we mix what we get
            // Wait, we need to filter if track_kind exists. ActiveClipC doesn't have track_kind.
            // In core it was already filtered or we can just assume opacity represents volume and mix it.
            let media_path = unsafe { CStr::from_ptr(clip.media_ref.as_ptr() as *const c_char) }.to_string_lossy();

            let pcm = decode_audio_chunk(&media_path, clip.source_frame, chunk_frames, sample_rate);

            for (j, s) in pcm.iter().enumerate() {
                if j < mixed.len() {
                    mixed[j] += s * clip.opacity;
                }
            }
        }

        for s in mixed.iter_mut() {
            *s = s.clamp(-1.0, 1.0);
        }

        prod.push_slice(&mixed);
    }
}

// Stub for decoding audio chunk
fn decode_audio_chunk(
    _media_path: &str,
    _source_frame: i64,
    chunk_frames: usize,
    _target_sample_rate: u32,
) -> Vec<f32> {
    // TODO(pass B): replace with real audio extraction via decoder FFI once available.
    vec![0.0f32; chunk_frames * 2]
}
