pub mod timeline;
pub mod compositor;
pub mod undo;
pub mod ffi;

#[repr(C)]
pub struct EngineHandle {
    pub timeline: timeline::Timeline,
    pub undo_stack: undo::UndoRedoStack,
}

