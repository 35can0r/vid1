use crate::timeline::Timeline;
use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;
use serde_json;

#[unsafe(no_mangle)]
pub extern "C" fn timeline_new(width: u32, height: u32, fps: f64) -> *mut Timeline {
    if !fps.is_finite() {
        return ptr::null_mut();
    }
    Box::into_raw(Box::new(Timeline::new(width, height, fps)))
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_free(ptr: *mut Timeline) {
    if !ptr.is_null() {
        unsafe {
            drop(Box::from_raw(ptr));
        }
    }
}

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

#[unsafe(no_mangle)]
pub extern "C" fn timeline_get_active_clips(
    timeline: *const Timeline,
    frame_number: i64,
    out_clips: *mut *mut ActiveClipC,
) -> i32 {
    if timeline.is_null() || out_clips.is_null() {
        return 0;
    }

    let timeline = unsafe { &*timeline };
    let mut active_clips = Vec::new();

    for (track_idx, track) in timeline.tracks.iter().enumerate() {
        if track.muted || track.hidden || track.track_type != crate::timeline::ClipType::Video {
            continue;
        }

        for clip in &track.clips {
            if frame_number >= clip.start_frame && frame_number < clip.start_frame + clip.duration_frames {
                let local_frame = frame_number - clip.start_frame;
                let mut source_frame = clip.trim_start_frame + (local_frame as f64 * clip.speed).round() as i64;
                if source_frame < 0 {
                    source_frame = 0;
                }

                let mut clip_c = ActiveClipC {
                    clip_id: [0; 37],
                    media_ref: [0; 37],
                    source_frame,
                    track_index: track_idx as u32,
                    center_x: 0.5,
                    center_y: 0.5,
                    width: 1.0,
                    height: 1.0,
                    rotation: 0.0,
                    crop_left: 0.0,
                    crop_top: 0.0,
                    crop_right: 0.0,
                    crop_bottom: 0.0,
                    exposure: 0.0,
                    contrast: 1.0,
                    temperature: 0.0,
                    tint: 0.0,
                    saturation: 1.0,
                    opacity: 1.0,
                };

                let clip_id_str = clip.id.to_string();
                let media_ref_str = &clip.media_ref;

                let clip_id_bytes = clip_id_str.as_bytes();
                for i in 0..clip_id_bytes.len().min(36) {
                    clip_c.clip_id[i] = clip_id_bytes[i];
                }

                let media_ref_bytes = media_ref_str.as_bytes();
                for i in 0..media_ref_bytes.len().min(36) {
                    clip_c.media_ref[i] = media_ref_bytes[i];
                }

                clip_c.opacity = clip.opacity(local_frame) as f32;

                let transform = clip.transform(local_frame);
                clip_c.center_x = transform.center_x as f32;
                clip_c.center_y = transform.center_y as f32;
                clip_c.width = transform.width as f32;
                clip_c.height = transform.height as f32;
                clip_c.rotation = transform.rotation as f32;

                let crop = clip.crop(local_frame);
                clip_c.crop_left = crop.left as f32;
                clip_c.crop_top = crop.top as f32;
                clip_c.crop_right = crop.right as f32;
                clip_c.crop_bottom = crop.bottom as f32;

                active_clips.push(clip_c);
            }
        }
    }

    let count = active_clips.len() as i32;
    if count > 0 {
        let boxed_slice = active_clips.into_boxed_slice();
        let ptr = Box::into_raw(boxed_slice) as *mut ActiveClipC;
        unsafe {
            *out_clips = ptr;
        }
    } else {
        unsafe {
            *out_clips = ptr::null_mut();
        }
    }

    count
}

#[unsafe(no_mangle)]
pub extern "C" fn active_clips_free(ptr: *mut ActiveClipC, count: i32) {
    if !ptr.is_null() && count > 0 {
        unsafe {
            let slice = std::slice::from_raw_parts_mut(ptr, count as usize);
            drop(Box::from_raw(slice));
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_resolve_media(media_ref: *const c_char) -> *mut c_char {
    if media_ref.is_null() {
        return ptr::null_mut();
    }
    let c_str = unsafe { std::ffi::CStr::from_ptr(media_ref) };
    let media_ref_str = match c_str.to_str() {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };

    let manifest_path = "media.json";
    let mut resolved_path = format!("project/media/{}", media_ref_str); // default fallback

    if let Ok(content) = std::fs::read_to_string(manifest_path) {
        if let Ok(json) = serde_json::from_str::<serde_json::Value>(&content) {
            if let Some(path) = json.get(media_ref_str).and_then(|v| v.as_str()) {
                resolved_path = format!("project/media/{}", path);
            }
        }
    }

    match CString::new(resolved_path) {
        Ok(c_str) => c_str.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_total_frames(timeline: *const Timeline) -> i64 {
    if timeline.is_null() {
        return 0;
    }
    unsafe { (*timeline).total_frames() }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_fps(timeline: *const Timeline) -> f64 {
    if timeline.is_null() {
        return 0.0;
    }
    unsafe { (*timeline).fps }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_from_json(json_utf8: *const c_char, len: usize) -> *mut Timeline {
    if json_utf8.is_null() || len == 0 {
        return ptr::null_mut();
    }

    let slice = unsafe { std::slice::from_raw_parts(json_utf8 as *const u8, len) };

    match serde_json::from_slice::<Timeline>(slice) {
        Ok(timeline) => Box::into_raw(Box::new(timeline)),
        Err(_) => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_to_json(timeline: *const Timeline) -> *mut c_char {
    if timeline.is_null() {
        return ptr::null_mut();
    }

    unsafe {
        let timeline_ref = &*timeline;
        match serde_json::to_string(timeline_ref) {
            Ok(json) => {
                match CString::new(json) {
                    Ok(c_str) => c_str.into_raw(),
                    Err(_) => ptr::null_mut(),
                }
            }
            Err(_) => ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn string_free(ptr: *mut c_char) {
    if !ptr.is_null() {
        unsafe {
            drop(CString::from_raw(ptr));
        }
    }
}
