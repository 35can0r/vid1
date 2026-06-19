use crate::timeline::{ClipType, Timeline, Track};

use std::os::raw::c_char;
use std::ptr;

#[unsafe(no_mangle)]
pub extern "C" fn palmier_timeline_new(width: u32, height: u32, fps: f64) -> *mut Timeline {
    Box::into_raw(Box::new(Timeline::new(width, height, fps)))
}

#[unsafe(no_mangle)]
pub extern "C" fn palmier_timeline_free(ptr: *mut Timeline) {
    if !ptr.is_null() {
        unsafe {
            drop(Box::from_raw(ptr));
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn palmier_timeline_add_track(
    timeline: *mut Timeline,
    track_type: i32,
) -> bool {
    if timeline.is_null() {
        return false;
    }

    let ttype = match track_type {
        0 => ClipType::Video,
        1 => ClipType::Audio,
        2 => ClipType::Image,
        3 => ClipType::Text,
        4 => ClipType::Lottie,
        _ => return false,
    };

    unsafe {
        let timeline_ref = &mut *timeline;
        timeline_ref.tracks.push(Track::new(ttype));
    }
    true
}

// In a full implementation we would also expose the Compositor initialization
// and the execution of the compute shader with proper native Texture handles
// (e.g. from Metal or Vulkan interop).

#[unsafe(no_mangle)]
pub extern "C" fn palmier_timeline_to_json(timeline: *const Timeline) -> *mut c_char {
    if timeline.is_null() {
        return ptr::null_mut();
    }

    unsafe {
        let timeline_ref = &*timeline;
        match serde_json::to_string(timeline_ref) {
            Ok(json) => {
                let c_str = std::ffi::CString::new(json).unwrap();
                c_str.into_raw()
            }
            Err(_) => ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn palmier_string_free(s: *mut c_char) {
    if !s.is_null() {
        unsafe {
            drop(std::ffi::CString::from_raw(s));
        }
    }
}
