use crate::timeline::Timeline;
use crate::EngineHandle;
use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;
use serde_json;

#[unsafe(no_mangle)]
pub extern "C" fn timeline_new(width: u32, height: u32, fps: f64) -> *mut EngineHandle {
    if !fps.is_finite() {
        return ptr::null_mut();
    }
    Box::into_raw(Box::new(EngineHandle {
        timeline: Timeline::new(width, height, fps),
        undo_stack: crate::undo::UndoRedoStack::new(),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_free(ptr: *mut EngineHandle) {
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
    handle: *const EngineHandle,
    frame_number: i64,
    out_clips: *mut ActiveClipC,
    max_clips: i32,
) -> i32 {
    if handle.is_null() || out_clips.is_null() || max_clips <= 0 {
        return 0;
    }

    let timeline = unsafe { &(*handle).timeline };
    let mut count = 0;

    let out_slice = unsafe { std::slice::from_raw_parts_mut(out_clips, max_clips as usize) };

    for (track_idx, track) in timeline.tracks.iter().enumerate() {
        if track.muted || track.hidden {
            continue;
        }

        let track_kind = match track.track_type {
            crate::timeline::ClipType::Audio => 1,
            crate::timeline::ClipType::Video => 0,
            _ => continue, // We only process video/audio clips this way
        };

        for clip in &track.clips {
            if count >= max_clips as usize {
                break;
            }

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

                out_slice[count] = clip_c;
                count += 1;
            }
        }
    }

    count as i32
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
pub extern "C" fn timeline_total_frames(handle: *const EngineHandle) -> i64 {
    if handle.is_null() {
        return 0;
    }
    unsafe { (*handle).timeline.total_frames() }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_fps(handle: *const EngineHandle) -> f64 {
    if handle.is_null() {
        return 0.0;
    }
    unsafe { (*handle).timeline.fps }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_from_json(json_utf8: *const c_char, len: usize) -> *mut EngineHandle {
    if json_utf8.is_null() || len == 0 {
        return ptr::null_mut();
    }

    let slice = unsafe { std::slice::from_raw_parts(json_utf8 as *const u8, len) };

    match serde_json::from_slice::<Timeline>(slice) {
        Ok(timeline) => Box::into_raw(Box::new(EngineHandle {
            timeline,
            undo_stack: crate::undo::UndoRedoStack::new(),
        })),
        Err(_) => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_to_json(handle: *const EngineHandle) -> *mut c_char {
    if handle.is_null() {
        return ptr::null_mut();
    }

    unsafe {
        let timeline_ref = &(*handle).timeline;
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

// ─── Stub lifecycle functions for back-compat ─────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn audio_engine_create(timeline: *mut EngineHandle) -> *mut dsp::AudioEngine {
    if timeline.is_null() { return std::ptr::null_mut(); }
    Box::into_raw(Box::new(dsp::AudioEngine::new(timeline as *mut dsp::EngineHandle)))
}

#[unsafe(no_mangle)]
pub extern "C" fn audio_engine_play(engine: *mut dsp::AudioEngine, from_frame: i64) {
    if engine.is_null() { return; }
    let engine = unsafe { &mut *engine };
    engine.play(from_frame);
}

#[unsafe(no_mangle)]
pub extern "C" fn audio_engine_pause(engine: *mut dsp::AudioEngine) {
    if engine.is_null() { return; }
    let engine = unsafe { &mut *engine };
    engine.pause();
}

#[unsafe(no_mangle)]
pub extern "C" fn audio_engine_stop(engine: *mut dsp::AudioEngine) {
    if engine.is_null() { return; }
    let engine = unsafe { &mut *engine };
    engine.stop();
}

#[unsafe(no_mangle)]
pub extern "C" fn audio_engine_current_frame(engine: *mut dsp::AudioEngine) -> i64 {
    if engine.is_null() { return 0; }
    let engine = unsafe { &*engine };
    engine.current_frame()
}

#[unsafe(no_mangle)]
pub extern "C" fn audio_engine_destroy(engine: *mut dsp::AudioEngine) {
    if engine.is_null() { return; }
    let mut engine = unsafe { Box::from_raw(engine) };
    engine.destroy();
}

// ─── Stub lifecycle functions for back-compat ─────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn timeline_undo_stack_init(_handle: *mut EngineHandle) {}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_undo_stack_free(_handle: *mut EngineHandle) {}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_checkpoint(_handle: *mut EngineHandle) {}

// ─── Wired Undo / Redo / Update ───────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn timeline_undo(handle: *mut EngineHandle) -> bool {
    if handle.is_null() { return false; }
    let handle = unsafe { &mut *handle };
    match handle.undo_stack.undo(&mut handle.timeline) {
        Ok(_) => {
            if let Ok(json) = serde_json::to_string_pretty(&handle.timeline) {
                let _ = std::fs::write("timeline.json", json);
            }
            true
        }
        Err(_) => false,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_redo(handle: *mut EngineHandle) -> bool {
    if handle.is_null() { return false; }
    let handle = unsafe { &mut *handle };
    match handle.undo_stack.redo(&mut handle.timeline) {
        Ok(_) => {
            if let Ok(json) = serde_json::to_string_pretty(&handle.timeline) {
                let _ = std::fs::write("timeline.json", json);
            }
            true
        }
        Err(_) => false,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn timeline_update_from_json(
    handle: *mut EngineHandle,
    json_utf8: *const c_char,
    len: usize,
) -> bool {
    if handle.is_null() || json_utf8.is_null() || len == 0 {
        return false;
    }
    let handle = unsafe { &mut *handle };
    let slice = unsafe { std::slice::from_raw_parts(json_utf8 as *const u8, len) };
    
    let Ok(new_timeline) = serde_json::from_slice::<Timeline>(slice) else {
        return false;
    };

    let mut operations = Vec::new();

    // 1. Detect moved clips
    for old_track in &handle.timeline.tracks {
        for old_clip in &old_track.clips {
            for new_track in &new_timeline.tracks {
                for new_clip in &new_track.clips {
                    if old_clip.id == new_clip.id {
                        if old_clip.start_frame != new_clip.start_frame || old_track.id != new_track.id {
                            operations.push(crate::undo::Operation::MoveClip {
                                clip_id: old_clip.id,
                                from_track_id: old_track.id,
                                to_track_id: new_track.id,
                                from_start_frame: old_clip.start_frame,
                                to_start_frame: new_clip.start_frame,
                            });
                        }
                    }
                }
            }
        }
    }

    // 2. Detect newly inserted clips
    for new_track in &new_timeline.tracks {
        for new_clip in &new_track.clips {
            let mut found = false;
            for old_track in &handle.timeline.tracks {
                for old_clip in &old_track.clips {
                    if old_clip.id == new_clip.id {
                        found = true;
                        break;
                    }
                }
            }
            if !found {
                operations.push(crate::undo::Operation::InsertClip {
                    track_id: new_track.id,
                    clip: new_clip.clone(),
                });
            }
        }
    }

    // 3. Detect deleted clips
    for old_track in &handle.timeline.tracks {
        for old_clip in &old_track.clips {
            let mut found = false;
            for new_track in &new_timeline.tracks {
                for new_clip in &new_track.clips {
                    if old_clip.id == new_clip.id {
                        found = true;
                        break;
                    }
                }
            }
            if !found {
                operations.push(crate::undo::Operation::DeleteClip {
                    track_id: old_track.id,
                    clip: old_clip.clone(),
                });
            }
        }
    }

    // If there are operations, record them in the undo stack
    if !operations.is_empty() {
        handle.undo_stack.begin_transaction("UI Mutation");
        for op in operations {
            handle.undo_stack.record_operation(op);
        }
        handle.undo_stack.commit_transaction();
    }

    handle.timeline = new_timeline;
    true
}

#[derive(serde::Serialize, serde::Deserialize)]
pub struct MediaEntry {
    #[serde(rename = "Id")]
    pub id: String,
    #[serde(rename = "Name")]
    pub name: String,
    #[serde(rename = "Path")]
    pub path: String,
}

#[unsafe(no_mangle)]
pub extern "C" fn media_get_all_json(_handle: *mut EngineHandle) -> *mut c_char {
    let mut entries = Vec::new();
    if let Ok(content) = std::fs::read_to_string("media.json") {
        if let Ok(json) = serde_json::from_str::<serde_json::Value>(&content) {
            if let Some(obj) = json.as_object() {
                for (k, v) in obj {
                    if let Some(val_str) = v.as_str() {
                        let name = std::path::Path::new(val_str)
                            .file_name()
                            .and_then(|s| s.to_str())
                            .unwrap_or(val_str)
                            .to_string();
                        
                        entries.push(MediaEntry {
                            id: k.clone(),
                            name,
                            path: format!("project/media/{}", val_str),
                        });
                    }
                }
            }
        }
    }

    if let Ok(serialized) = serde_json::to_string(&entries) {
        if let Ok(c_str) = CString::new(serialized) {
            return c_str.into_raw();
        }
    }
    ptr::null_mut()
}

#[unsafe(no_mangle)]
pub extern "C" fn palmier_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        unsafe {
            drop(CString::from_raw(ptr));
        }
    }
}

