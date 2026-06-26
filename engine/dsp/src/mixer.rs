use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicI64, AtomicBool, Ordering};
use std::time::Duration;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use ringbuf::traits::{Producer, Observer};
use rubato::{Resampler, SincFixedIn, SincInterpolationType, SincInterpolationParameters, WindowFunction};
use crate::{EngineHandle, HeapProducer};

#[repr(C)]
pub struct ActiveClipC {
    pub clip_id: [u8; 37],
    pub media_ref: [u8; 37],
    pub source_frame: i64,
    pub track_index: u32,
    pub track_kind: u32, // 0 = Video, 1 = Audio
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

/// Mirrors the AudioInfo struct from video_decoder.h
#[repr(C)]
pub struct AudioInfo {
    pub sample_rate: i32,
    pub channels: i32,
}

/// Opaque C decoder handle (matches DecoderHandle in video_decoder.h)
#[repr(C)]
pub struct DecoderHandleOpaque {
    _private: [u8; 0],
}

unsafe extern "C" {
    fn timeline_get_active_clips(
        handle: *const EngineHandle,
        frame_number: i64,
        out_clips: *mut ActiveClipC,
        max_clips: i32,
    ) -> i32;

    fn active_clips_free(ptr: *mut ActiveClipC, count: i32);

    // Resolve media UUID -> absolute path (reads media.json). Caller must free with palmier_free_string.
    fn timeline_resolve_media(media_ref: *const c_char) -> *mut c_char;

    // Free strings allocated by Rust FFI (palmier_free_string is defined in ffi.rs)
    fn palmier_free_string(ptr: *mut c_char);

    // C++ decoder ABI
    fn decoder_open(path: *const c_char) -> *mut DecoderHandleOpaque;
    fn decoder_get_audio_info(h: *mut DecoderHandleOpaque) -> AudioInfo;
    fn decoder_decode_audio_frame(
        h: *mut DecoderHandleOpaque,
        source_frame: i64,
        out_pcm: *mut f32,
        out_sample_count: *mut i32,
        target_sample_rate: u32,
    ) -> i32;
    fn decoder_close(h: *mut DecoderHandleOpaque);
}

/// Resolve a media UUID (null-terminated ASCII in the clip struct) to a file path CString.
/// Returns None if the UUID is empty or the path can't be resolved.
unsafe fn resolve_media_path(media_ref_bytes: &[u8; 37]) -> Option<CString> {
    let len = media_ref_bytes.iter().position(|&b| b == 0).unwrap_or(37);
    if len == 0 {
        return None;
    }
    let c_media_ref = CString::new(&media_ref_bytes[..len]).ok()?;
    let resolved_ptr = unsafe { timeline_resolve_media(c_media_ref.as_ptr()) };
    if resolved_ptr.is_null() {
        return None;
    }
    let path_str = unsafe { CStr::from_ptr(resolved_ptr) }.to_str().ok()?.to_owned();
    unsafe { palmier_free_string(resolved_ptr) };
    CString::new(path_str).ok()
}

/// Decode one audio chunk for a single clip, returning stereo f32 interleaved PCM
/// at `target_sample_rate`. Falls back to silence on any error.
unsafe fn decode_audio_chunk(
    media_ref_bytes: &[u8; 37],
    source_frame: i64,
    chunk_frames: usize,
    target_sample_rate: u32,
) -> Vec<f32> {
    let silence = || vec![0.0f32; chunk_frames * 2];

    let path_cstr = match unsafe { resolve_media_path(media_ref_bytes) } {
        Some(p) => p,
        None => return silence(),
    };

    let decoder = unsafe { decoder_open(path_cstr.as_ptr()) };
    if decoder.is_null() {
        return silence();
    }

    // Query native audio format so we can resample if needed
    let audio_info = unsafe { decoder_get_audio_info(decoder) };
    let native_rate = audio_info.sample_rate as u32;

    // Allocate a large enough buffer for the native-rate chunk plus extra
    // (native rate may differ from target — allocate generously)
    let native_chunk = if native_rate > 0 {
        (chunk_frames as u64 * native_rate as u64 / target_sample_rate.max(1) as u64 + 1024) as i32
    } else {
        (chunk_frames as i32) + 1024
    };

    let mut raw_pcm = vec![0.0f32; native_chunk as usize * 2];
    let mut out_count = native_chunk;

    let ret = unsafe {
        decoder_decode_audio_frame(
            decoder,
            source_frame,
            raw_pcm.as_mut_ptr(),
            &mut out_count,
            target_sample_rate, // informational, not used by C++ yet
        )
    };
    unsafe { decoder_close(decoder) };

    if ret < 0 || out_count <= 0 || native_rate == 0 {
        return silence();
    }

    let sample_count = out_count as usize;
    raw_pcm.truncate(sample_count * 2);

    // If native rate matches target, no resampling needed
    if native_rate == target_sample_rate {
        let mut out = vec![0.0f32; chunk_frames * 2];
        let copy_pairs = sample_count.min(chunk_frames);
        out[..copy_pairs * 2].copy_from_slice(&raw_pcm[..copy_pairs * 2]);
        return out;
    }

    // Resample using rubato SincFixedIn
    // De-interleave raw_pcm into [L_channel, R_channel]
    let mut left: Vec<f64>  = Vec::with_capacity(sample_count);
    let mut right: Vec<f64> = Vec::with_capacity(sample_count);
    for i in 0..sample_count {
        left.push(raw_pcm[i * 2] as f64);
        right.push(raw_pcm[i * 2 + 1] as f64);
    }

    let ratio = target_sample_rate as f64 / native_rate as f64;
    let params = SincInterpolationParameters {
        sinc_len: 128,
        f_cutoff: 0.925,
        interpolation: SincInterpolationType::Linear,
        oversampling_factor: 128,
        window: WindowFunction::BlackmanHarris2,
    };

    let mut resampler = match SincFixedIn::<f64>::new(
        ratio,
        2.0,
        params,
        sample_count,
        2, // stereo
    ) {
        Ok(r) => r,
        Err(_) => return silence(),
    };

    let resampled = match resampler.process(&[left, right], None) {
        Ok(out) => out,
        Err(_) => return silence(),
    };

    // Re-interleave back to f32 stereo
    let out_len = resampled[0].len().min(chunk_frames);
    let mut out = vec![0.0f32; chunk_frames * 2];
    for i in 0..out_len {
        out[i * 2]     = resampled[0][i] as f32;
        out[i * 2 + 1] = resampled[1][i] as f32;
    }
    out
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
        let lookahead_frame = ((current_sample + lookahead_samples as i64 / 2) as f64
            / sample_rate as f64
            * fps) as i64;

        let mut clips: [ActiveClipC; 16] = unsafe { std::mem::zeroed() };

        let count = unsafe {
            timeline_get_active_clips(timeline, lookahead_frame, clips.as_mut_ptr(), 16)
        };

        let chunk_frames = (sample_rate as f64 * 0.02) as usize; // 20ms chunks
        let mut mixed = vec![0.0f32; chunk_frames * 2]; // stereo

        for i in 0..count as usize {
            let clip = &clips[i];

            for clip in clips_slice {
                // FIX 1: Only process audio tracks (track_kind == 1)
                if clip.track_kind != 1 {
                    continue;
                }

                // FIX 2: Decode real audio via FFmpeg C ABI decoder + rubato resampling
                let pcm = unsafe {
                    decode_audio_chunk(
                        &clip.media_ref,
                        clip.source_frame,
                        chunk_frames,
                        sample_rate,
                    )
                };

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
