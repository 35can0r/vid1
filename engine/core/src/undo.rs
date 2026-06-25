use crate::timeline::{Timeline, Track, Clip, ClipContent};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum Operation {
    InsertTrack {
        index: usize,
        track: Track,
    },
    DeleteTrack {
        index: usize,
        track: Track,
    },
    InsertClip {
        track_id: Uuid,
        clip: Clip,
    },
    DeleteClip {
        track_id: Uuid,
        clip: Clip,
    },
    MoveClip {
        clip_id: Uuid,
        from_track_id: Uuid,
        to_track_id: Uuid,
        from_start_frame: i64,
        to_start_frame: i64,
    },
    TrimClip {
        clip_id: Uuid,
        track_id: Uuid,
        old_start: i64,
        old_duration: i64,
        old_trim_start: i64,
        new_start: i64,
        new_duration: i64,
        new_trim_start: i64,
    },
    SetProperty {
        target_id: Uuid, // Can be track_id or clip_id
        property_path: String,
        old_value: serde_json::Value,
        new_value: serde_json::Value,
    },
}

impl Operation {
    pub fn reverse(&self) -> Operation {
        match self {
            Operation::InsertTrack { index, track } => Operation::DeleteTrack {
                index: *index,
                track: track.clone(),
            },
            Operation::DeleteTrack { index, track } => Operation::InsertTrack {
                index: *index,
                track: track.clone(),
            },
            Operation::InsertClip { track_id, clip } => Operation::DeleteClip {
                track_id: *track_id,
                clip: clip.clone(),
            },
            Operation::DeleteClip { track_id, clip } => Operation::InsertClip {
                track_id: *track_id,
                clip: clip.clone(),
            },
            Operation::MoveClip {
                clip_id,
                from_track_id,
                to_track_id,
                from_start_frame,
                to_start_frame,
            } => Operation::MoveClip {
                clip_id: *clip_id,
                from_track_id: *to_track_id,
                to_track_id: *from_track_id,
                from_start_frame: *to_start_frame,
                to_start_frame: *from_start_frame,
            },
            Operation::TrimClip {
                clip_id,
                track_id,
                old_start,
                old_duration,
                old_trim_start,
                new_start,
                new_duration,
                new_trim_start,
            } => Operation::TrimClip {
                clip_id: *clip_id,
                track_id: *track_id,
                old_start: *new_start,
                old_duration: *new_duration,
                old_trim_start: *new_trim_start,
                new_start: *old_start,
                new_duration: *old_duration,
                new_trim_start: *old_trim_start,
            },
            Operation::SetProperty {
                target_id,
                property_path,
                old_value,
                new_value,
            } => Operation::SetProperty {
                target_id: *target_id,
                property_path: property_path.clone(),
                old_value: new_value.clone(),
                new_value: old_value.clone(),
            },
        }
    }
}

pub fn apply_operation(timeline: &mut Timeline, op: &Operation) -> Result<(), String> {
    match op {
        Operation::InsertTrack { index, track } => {
            if *index >= timeline.tracks.len() {
                timeline.tracks.push(track.clone());
            } else {
                timeline.tracks.insert(*index, track.clone());
            }
            Ok(())
        }
        Operation::DeleteTrack { index, track } => {
            if *index < timeline.tracks.len() && timeline.tracks[*index].id == track.id {
                timeline.tracks.remove(*index);
                Ok(())
            } else if let Some(pos) = timeline.tracks.iter().position(|t| t.id == track.id) {
                timeline.tracks.remove(pos);
                Ok(())
            } else {
                Err(format!("Track with ID {} not found to delete", track.id))
            }
        }
        Operation::InsertClip { track_id, clip } => {
            if let Some(track) = timeline.tracks.iter_mut().find(|t| t.id == *track_id) {
                track.clips.push(clip.clone());
                Ok(())
            } else {
                Err(format!("Track with ID {} not found", track_id))
            }
        }
        Operation::DeleteClip { track_id, clip } => {
            if let Some(track) = timeline.tracks.iter_mut().find(|t| t.id == *track_id) {
                if let Some(pos) = track.clips.iter().position(|c| c.id == clip.id) {
                    track.clips.remove(pos);
                    Ok(())
                } else {
                    Err(format!("Clip with ID {} not found in track {}", clip.id, track_id))
                }
            } else {
                Err(format!("Track with ID {} not found", track_id))
            }
        }
        Operation::MoveClip {
            clip_id,
            from_track_id,
            to_track_id,
            from_start_frame: _,
            to_start_frame,
        } => {
            // Find and remove clip from the source track
            let mut removed_clip = None;
            if let Some(track) = timeline.tracks.iter_mut().find(|t| t.id == *from_track_id) {
                if let Some(pos) = track.clips.iter().position(|c| c.id == *clip_id) {
                    removed_clip = Some(track.clips.remove(pos));
                }
            }
            
            if let Some(mut clip) = removed_clip {
                clip.start_frame = *to_start_frame;
                if let Some(to_track) = timeline.tracks.iter_mut().find(|t| t.id == *to_track_id) {
                    to_track.clips.push(clip);
                    Ok(())
                } else {
                    // Put it back to avoid losing the clip if to_track_id is invalid
                    if let Some(from_track) = timeline.tracks.iter_mut().find(|t| t.id == *from_track_id) {
                        from_track.clips.push(clip);
                    }
                    Err(format!("Target track with ID {} not found", to_track_id))
                }
            } else {
                Err(format!("Clip with ID {} not found in track {}", clip_id, from_track_id))
            }
        }
        Operation::TrimClip {
            clip_id,
            track_id,
            old_start: _,
            old_duration: _,
            old_trim_start: _,
            new_start,
            new_duration,
            new_trim_start,
        } => {
            if let Some(track) = timeline.tracks.iter_mut().find(|t| t.id == *track_id) {
                if let Some(clip) = track.clips.iter_mut().find(|c| c.id == *clip_id) {
                    clip.start_frame = *new_start;
                    clip.duration_frames = *new_duration;
                    clip.trim_start_frame = *new_trim_start;
                    Ok(())
                } else {
                    Err(format!("Clip with ID {} not found in track {}", clip_id, track_id))
                }
            } else {
                Err(format!("Track with ID {} not found", track_id))
            }
        }
        Operation::SetProperty {
            target_id,
            property_path,
            old_value: _,
            new_value,
        } => {
            // Let's search tracks first
            if let Some(track) = timeline.tracks.iter_mut().find(|t| t.id == *target_id) {
                match property_path.as_str() {
                    "muted" => {
                        track.muted = serde_json::from_value(new_value.clone())
                            .map_err(|e| format!("Invalid value for muted: {}", e))?;
                        return Ok(());
                    }
                    "hidden" => {
                        track.hidden = serde_json::from_value(new_value.clone())
                            .map_err(|e| format!("Invalid value for hidden: {}", e))?;
                        return Ok(());
                    }
                    "sync_locked" => {
                        track.sync_locked = serde_json::from_value(new_value.clone())
                            .map_err(|e| format!("Invalid value for sync_locked: {}", e))?;
                        return Ok(());
                    }
                    _ => return Err(format!("Unsupported track property: {}", property_path)),
                }
            }

            // If not a track, let's search for a clip in all tracks
            for track in &mut timeline.tracks {
                if let Some(clip) = track.clips.iter_mut().find(|c| c.id == *target_id) {
                    match property_path.as_str() {
                        "speed" => {
                            clip.speed = serde_json::from_value(new_value.clone())
                                .map_err(|e| format!("Invalid speed: {}", e))?;
                            return Ok(());
                        }
                        "opacity" => {
                            if let crate::timeline::ClipContent::Video { opacity, .. } = &mut clip.content {
                                *opacity = serde_json::from_value(new_value.clone())
                                    .map_err(|e| format!("Invalid opacity: {}", e))?;
                                return Ok(());
                            } else {
                                return Err("Opacity can only be set on video clips".to_string());
                            }
                        }
                        "volume" => {
                            if let crate::timeline::ClipContent::Audio { volume, .. } = &mut clip.content {
                                *volume = serde_json::from_value(new_value.clone())
                                    .map_err(|e| format!("Invalid volume: {}", e))?;
                                return Ok(());
                            } else {
                                return Err("Volume can only be set on audio clips".to_string());
                            }
                        }
                        "text_content" => {
                            if let crate::timeline::ClipContent::Text { text_content, .. } = &mut clip.content {
                                *text_content = serde_json::from_value(new_value.clone())
                                    .map_err(|e| format!("Invalid text_content: {}", e))?;
                                return Ok(());
                            } else {
                                return Err("text_content can only be set on text clips".to_string());
                            }
                        }
                        "transform" => {
                            if let crate::timeline::ClipContent::Video { transform, .. } = &mut clip.content {
                                *transform = serde_json::from_value(new_value.clone())
                                    .map_err(|e| format!("Invalid transform: {}", e))?;
                                return Ok(());
                            } else {
                                return Err("Transform can only be set on video clips".to_string());
                            }
                        }
                        "crop" => {
                            if let crate::timeline::ClipContent::Video { crop, .. } = &mut clip.content {
                                *crop = serde_json::from_value(new_value.clone())
                                    .map_err(|e| format!("Invalid crop: {}", e))?;
                                return Ok(());
                            } else {
                                return Err("Crop can only be set on video clips".to_string());
                            }
                        }
                        _ => return Err(format!("Unsupported clip property: {}", property_path)),
                    }
                }
            }

            Err(format!("Target with ID {} not found in timeline", target_id))
        }
    }
}

pub struct Transaction {
    pub name: String,
    pub operations: Vec<Operation>,
}

pub struct UndoRedoStack {
    pub undo_stack: Vec<Transaction>,
    pub redo_stack: Vec<Transaction>,
    pub active_transaction: Option<Transaction>,
}

impl UndoRedoStack {
    pub fn new() -> Self {
        Self {
            undo_stack: Vec::new(),
            redo_stack: Vec::new(),
            active_transaction: None,
        }
    }

    pub fn begin_transaction(&mut self, name: &str) {
        if self.active_transaction.is_some() {
            // Commit current before starting new
            self.commit_transaction();
        }
        self.active_transaction = Some(Transaction {
            name: name.to_string(),
            operations: Vec::new(),
        });
    }

    pub fn commit_transaction(&mut self) {
        if let Some(tx) = self.active_transaction.take() {
            if !tx.operations.is_empty() {
                self.undo_stack.push(tx);
                self.redo_stack.clear();
            }
        }
    }

    pub fn rollback_transaction(&mut self) {
        self.active_transaction = None;
    }

    pub fn record_operation(&mut self, op: Operation) {
        if let Some(ref mut tx) = self.active_transaction {
            tx.operations.push(op);
        } else {
            // If no active transaction, create an automatic single-operation transaction
            let mut tx = Transaction {
                name: "Auto-Transaction".to_string(),
                operations: Vec::new(),
            };
            tx.operations.push(op);
            self.undo_stack.push(tx);
            self.redo_stack.clear();
        }
    }

    pub fn undo(&mut self, timeline: &mut Timeline) -> Result<(), String> {
        if let Some(tx) = self.undo_stack.pop() {
            // Apply operations in reverse order, applying their reverse operation
            for op in tx.operations.iter().rev() {
                let rev_op = op.reverse();
                apply_operation(timeline, &rev_op)?;
            }
            self.redo_stack.push(tx);
            Ok(())
        } else {
            Err("Nothing to undo".to_string())
        }
    }

    pub fn redo(&mut self, timeline: &mut Timeline) -> Result<(), String> {
        if let Some(tx) = self.redo_stack.pop() {
            // Apply operations in forward order
            for op in &tx.operations {
                apply_operation(timeline, op)?;
            }
            self.undo_stack.push(tx);
            Ok(())
        } else {
            Err("Nothing to redo".to_string())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::*;
    use std::ffi::CString;

    #[test]
    fn test_ffi_undo_redo_clip_move() {
        // Create a timeline
        let handle = timeline_new(1920, 1080, 30.0);
        assert!(!handle.is_null());

        // Mutate the timeline: let's add a track and a clip manually
        let mut tl = unsafe { &(*handle).timeline }.clone();
        let track_id = uuid::Uuid::new_v4();
        let clip_id = uuid::Uuid::new_v4();
        let track = crate::timeline::Track {
            id: track_id,
            track_type: crate::timeline::ClipType::Video,
            muted: false,
            hidden: false,
            sync_locked: false,
            clips: vec![crate::timeline::Clip {
                id: clip_id,
                media_ref: "test.mp4".to_string(),
                start_frame: 0,
                duration_frames: 100,
                trim_start_frame: 0,
                speed: 1.0,
                content: crate::timeline::ClipContent::Video {
                    opacity: 1.0,
                    transform: crate::timeline::Transform::default(),
                    crop: crate::timeline::Crop::default(),
                    opacity_track: None,
                    transform_track: None,
                    crop_track: None,
                },
                link_group_id: None,
            }],
        };
        tl.tracks.push(track);

        // Update the FFI handle with the new JSON containing the clip
        let mutated_json_1 = serde_json::to_string(&tl).unwrap();
        let c_json_1 = CString::new(mutated_json_1.clone()).unwrap();
        let ok = timeline_update_from_json(handle, c_json_1.as_ptr(), mutated_json_1.len());
        assert!(ok);

        // Move the clip to start_frame = 50
        tl.tracks[0].clips[0].start_frame = 50;
        let mutated_json_2 = serde_json::to_string(&tl).unwrap();
        let c_json_2 = CString::new(mutated_json_2.clone()).unwrap();
        let ok = timeline_update_from_json(handle, c_json_2.as_ptr(), mutated_json_2.len());
        assert!(ok);

        // Check it moved in the active handle
        unsafe {
            let tl_ref = &*handle;
            assert_eq!(tl_ref.timeline.tracks[0].clips[0].start_frame, 50);
        }

        // Undo the move
        let undone = timeline_undo(handle);
        assert!(undone);
        unsafe {
            let tl_ref = &*handle;
            assert_eq!(tl_ref.timeline.tracks[0].clips[0].start_frame, 0);
        }

        // Redo the move
        let redone = timeline_redo(handle);
        assert!(redone);
        unsafe {
            let tl_ref = &*handle;
            assert_eq!(tl_ref.timeline.tracks[0].clips[0].start_frame, 50);
        }

        // Clean up
        timeline_free(handle);
    }

    #[test]
    fn test_media_get_all_json() {
        // Save the old media.json content if it exists
        let old_content = std::fs::read_to_string("media.json").ok();

        // Write a test media.json
        let test_json = r#"{"test-uuid-123": "my_sample_video.mp4"}"#;
        std::fs::write("media.json", test_json).unwrap();

        // Call the FFI function
        let handle = timeline_new(1920, 1080, 30.0);
        let ptr = media_get_all_json(handle);
        assert!(!ptr.is_null());

        let c_str = unsafe { std::ffi::CStr::from_ptr(ptr) };
        let json_str = c_str.to_str().unwrap();
        
        // Parse the returned JSON
        let entries: Vec<MediaEntry> = serde_json::from_str(json_str).unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].id, "test-uuid-123");
        assert_eq!(entries[0].name, "my_sample_video.mp4");
        assert_eq!(entries[0].path, "project/media/my_sample_video.mp4");

        // Clean up FFI string and handle
        palmier_free_string(ptr);
        timeline_free(handle);

        // Restore or delete media.json
        match old_content {
            Some(content) => std::fs::write("media.json", content).unwrap(),
            None => { let _ = std::fs::remove_file("media.json"); }
        }
    }
}

