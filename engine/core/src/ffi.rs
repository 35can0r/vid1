use crate::timeline::Timeline;
use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

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
