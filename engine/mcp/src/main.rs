use axum::{
    routing::post,
    extract::State,
    Json,
    Router,
};
use core_crate::timeline::{Timeline, Clip, ClipContent, TextGroupItem};
use undo::UndoRedoStack;
use serde::{Deserialize, Serialize};
use std::sync::{Arc, Mutex};
use std::net::SocketAddr;
use uuid::Uuid;
use tower_http::cors::CorsLayer;

#[derive(Debug, Deserialize)]
struct JsonRpcRequest {
    jsonrpc: String,
    method: String,
    params: Option<serde_json::Value>,
    id: Option<serde_json::Value>,
}

#[derive(Debug, Serialize)]
struct JsonRpcResponse {
    jsonrpc: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    result: Option<serde_json::Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<JsonRpcError>,
    id: serde_json::Value,
}

#[derive(Debug, Serialize)]
struct JsonRpcError {
    code: i32,
    message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    data: Option<serde_json::Value>,
}

struct ServerState {
    timeline: Mutex<Timeline>,
    undo_stack: Mutex<UndoRedoStack>,
}

fn make_error_response(code: i32, message: String, data: Option<serde_json::Value>, id: serde_json::Value) -> JsonRpcResponse {
    JsonRpcResponse {
        jsonrpc: "2.0".to_string(),
        result: None,
        error: Some(JsonRpcError { code, message, data }),
        id,
    }
}

// 3-step validation: Step 3 (Finiteness and rejection of null-encoded NaN/Infinity)
fn check_finiteness(value: &serde_json::Value) -> Result<(), String> {
    match value {
        serde_json::Value::Number(n) => {
            if let Some(f) = n.as_f64() {
                if !f.is_finite() {
                    return Err(format!("Invalid number: float value must be finite, found {}", f));
                }
            }
        }
        serde_json::Value::Array(arr) => {
            for val in arr {
                check_finiteness(val)?;
            }
        }
        serde_json::Value::Object(obj) => {
            // Check for float fields that are null (due to NaN/Infinity serialization)
            let float_keys = &[
                "opacity", "volume", "speed", "center_x", "center_y", "width", "height", "rotation",
                "left", "top", "right", "bottom"
            ];
            for (key, val) in obj {
                if float_keys.contains(&key.as_str()) && val.is_null() {
                    return Err(format!("Float field '{}' cannot be null/NaN/Infinity", key));
                }
                
                // If it is a float track, validate its keyframes do not have null values
                if (key == "opacity_track" || key == "volume_track") && !val.is_null() {
                    if let Some(track_obj) = val.as_object() {
                        if let Some(keyframes) = track_obj.get("keyframes") {
                            if let Some(kf_obj) = keyframes.as_object() {
                                for (kf_time, kf_val) in kf_obj {
                                    if let Some(kf_val_obj) = kf_val.as_object() {
                                        if let Some(v) = kf_val_obj.get("value") {
                                            if v.is_null() {
                                                return Err(format!("Keyframe value at time {} in track '{}' cannot be null/NaN/Infinity", kf_time, key));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if let Err(e) = check_finiteness(val) {
                    return Err(format!("At key '{}': {}", key, e));
                }
            }

            // Check for SetProperty operation payload
            if let (Some(path_val), Some(new_val)) = (obj.get("property_path"), obj.get("new_value")) {
                if let Some(path_str) = path_val.as_str() {
                    let float_paths = &[
                        "opacity", "volume", "speed", "center_x", "center_y", "width", "height", "rotation",
                        "left", "top", "right", "bottom"
                    ];
                    if float_paths.contains(&path_str) && new_val.is_null() {
                        return Err(format!("Property '{}' cannot be null/NaN/Infinity", path_str));
                    }
                }
            }
            if let (Some(path_val), Some(old_val)) = (obj.get("property_path"), obj.get("old_value")) {
                if let Some(path_str) = path_val.as_str() {
                    let float_paths = &[
                        "opacity", "volume", "speed", "center_x", "center_y", "width", "height", "rotation",
                        "left", "top", "right", "bottom"
                    ];
                    if float_paths.contains(&path_str) && old_val.is_null() {
                        return Err(format!("Property '{}' cannot be null/NaN/Infinity", path_str));
                    }
                }
            }
        }
        _ => {}
    }
    Ok(())
}

// 3-step validation: Step 1 (Unauthorized properties)
fn check_keys(value: &serde_json::Value, allowed: &[&str]) -> Result<(), String> {
    if let Some(obj) = value.as_object() {
        for key in obj.keys() {
            if !allowed.contains(&key.as_str()) {
                return Err(format!("Unauthorized property '{}'", key));
            }
        }
    }
    Ok(())
}

fn validate_keyframes_rec(track_value: &serde_json::Value, value_schema: &str) -> Result<(), String> {
    if let Some(obj) = track_value.as_object() {
        if let Some(keyframes) = obj.get("keyframes") {
            if let Some(kf_obj) = keyframes.as_object() {
                for (frame_str, kf) in kf_obj {
                    if frame_str.parse::<i64>().is_err() {
                        return Err(format!("Keyframe time must be an integer, found '{}'", frame_str));
                    }
                    check_keys(kf, &["value", "interpolation_out"])?;
                    if let Some(v) = kf.get("value") {
                        if value_schema != "f64" {
                            validate_no_unknown_keys_rec(v, value_schema)?;
                        }
                    }
                }
            }
        }
    }
    Ok(())
}

fn validate_no_unknown_keys_rec(value: &serde_json::Value, schema_type: &str) -> Result<(), String> {
    match schema_type {
        "Timeline" => {
            let allowed = &["width", "height", "fps", "tracks"];
            check_keys(value, allowed)?;
            if let Some(obj) = value.as_object() {
                if let Some(tracks) = obj.get("tracks") {
                    if let Some(arr) = tracks.as_array() {
                        for track in arr {
                            validate_no_unknown_keys_rec(track, "Track")?;
                        }
                    }
                }
            }
        }
        "Track" => {
            let allowed = &["id", "track_type", "muted", "hidden", "sync_locked", "clips"];
            check_keys(value, allowed)?;
            if let Some(obj) = value.as_object() {
                if let Some(clips) = obj.get("clips") {
                    if let Some(arr) = clips.as_array() {
                        for clip in arr {
                            validate_no_unknown_keys_rec(clip, "Clip")?;
                        }
                    }
                }
            }
        }
        "Clip" => {
            let allowed = &[
                "id", "media_ref", "start_frame", "duration_frames", "trim_start_frame", "speed", "link_group_id",
                "transform", "crop", "opacity", "volume", "text_content", "transform_track", "crop_track", "opacity_track", "volume_track"
            ];
            check_keys(value, allowed)?;
            if let Some(obj) = value.as_object() {
                if let Some(t) = obj.get("transform") {
                    validate_no_unknown_keys_rec(t, "Transform")?;
                }
                if let Some(c) = obj.get("crop") {
                    validate_no_unknown_keys_rec(c, "Crop")?;
                }
                if let Some(tt) = obj.get("transform_track") {
                    validate_no_unknown_keys_rec(tt, "TransformTrack")?;
                }
                if let Some(ct) = obj.get("crop_track") {
                    validate_no_unknown_keys_rec(ct, "CropTrack")?;
                }
                if let Some(ot) = obj.get("opacity_track") {
                    validate_no_unknown_keys_rec(ot, "F64Track")?;
                }
                if let Some(vt) = obj.get("volume_track") {
                    validate_no_unknown_keys_rec(vt, "F64Track")?;
                }
            }
        }
        "Transform" => {
            check_keys(value, &["center_x", "center_y", "width", "height", "rotation"])?;
        }
        "Crop" => {
            check_keys(value, &["left", "top", "right", "bottom"])?;
        }
        "TransformTrack" => {
            check_keys(value, &["keyframes"])?;
            validate_keyframes_rec(value, "Transform")?;
        }
        "CropTrack" => {
            check_keys(value, &["keyframes"])?;
            validate_keyframes_rec(value, "Crop")?;
        }
        "F64Track" => {
            check_keys(value, &["keyframes"])?;
            validate_keyframes_rec(value, "f64")?;
        }
        "Operation" => {
            if let Some(obj) = value.as_object() {
                if obj.len() != 1 {
                    return Err(format!("Operation must contain exactly one variant key, found {}", obj.len()));
                }
                let (variant, payload) = obj.iter().next().unwrap();
                match variant.as_str() {
                    "InsertTrack" | "DeleteTrack" => {
                        check_keys(payload, &["index", "track"])?;
                        if let Some(track) = payload.get("track") {
                            validate_no_unknown_keys_rec(track, "Track")?;
                        }
                    }
                    "InsertClip" | "DeleteClip" => {
                        check_keys(payload, &["track_id", "clip"])?;
                        if let Some(clip) = payload.get("clip") {
                            validate_no_unknown_keys_rec(clip, "Clip")?;
                        }
                    }
                    "MoveClip" => {
                        check_keys(payload, &["clip_id", "from_track_id", "to_track_id", "from_start_frame", "to_start_frame"])?;
                    }
                    "TrimClip" => {
                        check_keys(payload, &["clip_id", "track_id", "old_start", "old_duration", "old_trim_start", "new_start", "new_duration", "new_trim_start"])?;
                    }
                    "SetProperty" => {
                        check_keys(payload, &["target_id", "property_path", "old_value", "new_value"])?;
                    }
                    _ => return Err(format!("Unknown Operation variant: {}", variant)),
                }
            }
        }
        _ => {}
    }
    Ok(())
}

fn validate_apply_operations_args(args: &serde_json::Value) -> Result<(), String> {
    // Step 3: Finiteness Check
    check_finiteness(args)?;

    // Step 1: Unauthorized keys at root
    check_keys(args, &["operations"])?;

    // Step 1 recursively for operations
    if let Some(obj) = args.as_object() {
        if let Some(ops) = obj.get("operations") {
            if let Some(arr) = ops.as_array() {
                for op in arr {
                    validate_no_unknown_keys_rec(op, "Operation")?;
                }
            } else {
                return Err("operations must be an array".to_string());
            }
        } else {
            return Err("Missing operations parameter".to_string());
        }
    }
    Ok(())
}

// Step 6: get_timeline compaction logic
fn perform_compaction(timeline: &Timeline) -> serde_json::Value {
    let mut compacted = timeline.clone();
    for track in &mut compacted.tracks {
        if track.track_type == core_crate::timeline::ClipType::Text {
            let mut compacted_clips: Vec<Clip> = Vec::new();
            let mut current_group: Vec<Clip> = Vec::new();

            let mut sorted_clips = track.clips.clone();
            sorted_clips.sort_by_key(|c| c.start_frame);

            for clip in sorted_clips {
                let is_short_text = match &clip.content {
                    ClipContent::Text { text_content } => text_content.len() < 100,
                    _ => false,
                };

                if is_short_text {
                    current_group.push(clip);
                } else {
                    flush_group(&mut compacted_clips, &mut current_group);
                    compacted_clips.push(clip);
                }
            }
            flush_group(&mut compacted_clips, &mut current_group);
            track.clips = compacted_clips;
        }
    }
    serde_json::to_value(&compacted).unwrap_or(serde_json::Value::Null)
}

fn flush_group(compacted: &mut Vec<Clip>, group: &mut Vec<Clip>) {
    if group.is_empty() {
        return;
    }
    if group.len() == 1 {
        compacted.push(group.remove(0));
        return;
    }
    
    let min_start = group.iter().map(|c| c.start_frame).min().unwrap();
    let max_end = group.iter().map(|c| c.end_frame()).max().unwrap();
    
    let mut items = Vec::new();
    for c in group.iter() {
        if let ClipContent::Text { text_content } = &c.content {
            items.push(TextGroupItem {
                start_frame: c.start_frame,
                duration_frames: c.duration_frames,
                text_content: text_content.clone(),
            });
        }
    }
    
    let group_clip = Clip {
        id: Uuid::new_v4(),
        media_ref: "group".to_string(),
        start_frame: min_start,
        duration_frames: max_end - min_start,
        trim_start_frame: 0,
        speed: 1.0,
        content: ClipContent::TextGroup { items },
        link_group_id: None,
    };
    
    compacted.push(group_clip);
    group.clear();
}

async fn handle_rpc_call(
    state: Arc<ServerState>,
    req: JsonRpcRequest,
) -> JsonRpcResponse {
    let id = req.id.unwrap_or(serde_json::Value::Null);
    
    let (method_name, method_args) = if req.method == "tools/call" {
        if let Some(ref params) = req.params {
            let tool_name = params.get("name").and_then(|v| v.as_str()).unwrap_or("");
            let tool_args = params.get("arguments").cloned().unwrap_or(serde_json::Value::Null);
            (tool_name.to_string(), tool_args)
        } else {
            (req.method.clone(), serde_json::Value::Null)
        }
    } else if req.method == "tools/list" {
        ("tools/list".to_string(), serde_json::Value::Null)
    } else {
        (req.method.clone(), req.params.unwrap_or(serde_json::Value::Null))
    };

    match method_name.as_str() {
        "tools/list" => {
            let tools = serde_json::json!({
                "tools": [
                    {
                        "name": "get_timeline",
                        "description": "Get the current timeline, compacted for LLM context.",
                        "inputSchema": {
                            "type": "object",
                            "properties": {}
                        }
                    },
                    {
                        "name": "apply_operations",
                        "description": "Apply a list of timeline operations in a single undo group.",
                        "inputSchema": {
                            "type": "object",
                            "properties": {
                                "operations": {
                                    "type": "array",
                                    "items": {
                                        "type": "object"
                                    }
                                }
                            },
                            "required": ["operations"]
                        }
                    },
                    {
                        "name": "undo",
                        "description": "Undo the last timeline transaction.",
                        "inputSchema": {
                            "type": "object",
                            "properties": {}
                        }
                    },
                    {
                        "name": "redo",
                        "description": "Redo the last undone timeline transaction.",
                        "inputSchema": {
                            "type": "object",
                            "properties": {}
                        }
                    }
                ]
            });
            JsonRpcResponse {
                jsonrpc: "2.0".to_string(),
                result: Some(tools),
                error: None,
                id,
            }
        }
        "get_timeline" => {
            let timeline = state.timeline.lock().unwrap();
            let compacted = perform_compaction(&timeline);
            JsonRpcResponse {
                jsonrpc: "2.0".to_string(),
                result: Some(compacted),
                error: None,
                id,
            }
        }
        "apply_operations" => {
            if let Err(err_msg) = validate_apply_operations_args(&method_args) {
                return make_error_response(-32602, format!("Validation error: {}", err_msg), None, id);
            }
            
            #[derive(Deserialize)]
            struct ApplyArgs {
                operations: Vec<undo::Operation>,
            }
            
            // Step 2: Strict type decoding
            let args: ApplyArgs = match serde_json::from_value(method_args) {
                Ok(a) => a,
                Err(e) => {
                    return make_error_response(-32602, format!("Type decoding failed: {}", e), None, id);
                }
            };
            
            let mut timeline = state.timeline.lock().unwrap();
            let mut stack = state.undo_stack.lock().unwrap();
            
            stack.begin_transaction("Apply Operations");
            let mut success = true;
            let mut err_msg = String::new();
            for op in &args.operations {
                match undo::apply_operation(&mut timeline, op) {
                    Ok(_) => {
                        stack.record_operation(op.clone());
                    }
                    Err(e) => {
                        success = false;
                        err_msg = e;
                        break;
                    }
                }
            }
            
            if success {
                stack.commit_transaction();
                JsonRpcResponse {
                    jsonrpc: "2.0".to_string(),
                    result: Some(serde_json::json!({ "status": "success" })),
                    error: None,
                    id,
                }
            } else {
                stack.rollback_transaction();
                make_error_response(-32000, format!("Execution failed: {}", err_msg), None, id)
            }
        }
        "undo" => {
            let mut timeline = state.timeline.lock().unwrap();
            let mut stack = state.undo_stack.lock().unwrap();
            match stack.undo(&mut timeline) {
                Ok(_) => JsonRpcResponse {
                    jsonrpc: "2.0".to_string(),
                    result: Some(serde_json::json!({ "status": "success" })),
                    error: None,
                    id,
                },
                Err(e) => make_error_response(-32000, e, None, id),
            }
        }
        "redo" => {
            let mut timeline = state.timeline.lock().unwrap();
            let mut stack = state.undo_stack.lock().unwrap();
            match stack.redo(&mut timeline) {
                Ok(_) => JsonRpcResponse {
                    jsonrpc: "2.0".to_string(),
                    result: Some(serde_json::json!({ "status": "success" })),
                    error: None,
                    id,
                },
                Err(e) => make_error_response(-32000, e, None, id),
            }
        }
        _ => make_error_response(-32601, format!("Method not found: {}", method_name), None, id),
    }
}

async fn handle_post(
    State(state): State<Arc<ServerState>>,
    Json(payload): Json<JsonRpcRequest>,
) -> Json<JsonRpcResponse> {
    let resp = handle_rpc_call(state, payload).await;
    Json(resp)
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();

    let state = Arc::new(ServerState {
        timeline: Mutex::new(Timeline::new(1920, 1080, 30.0)),
        undo_stack: Mutex::new(UndoRedoStack::new()),
    });

    let app = Router::new()
        .route("/", post(handle_post))
        .route("/mcp", post(handle_post))
        .layer(CorsLayer::permissive())
        .with_state(state);

    let addr = SocketAddr::from(([127, 0, 0, 1], 19789));
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    tracing::info!("JSON-RPC 2.0 loopback server running on {}", addr);
    axum::serve(listener, app).await.unwrap();
}

#[cfg(test)]
mod tests {
    use super::*;
    use core_crate::timeline::{ClipType, ClipContent, Clip, Track};

    #[test]
    fn test_numeric_finiteness_validation() {
        let valid_json = serde_json::json!({
            "operations": []
        });
        assert!(validate_apply_operations_args(&valid_json).is_ok());

        let nan_json = serde_json::json!({
            "operations": [
                {
                    "SetProperty": {
                        "target_id": Uuid::new_v4().to_string(),
                        "property_path": "opacity",
                        "old_value": 1.0,
                        "new_value": f64::NAN
                    }
                }
            ]
        });
        assert!(validate_apply_operations_args(&nan_json).is_err());
        
        let inf_json = serde_json::json!({
            "operations": [
                {
                    "SetProperty": {
                        "target_id": Uuid::new_v4().to_string(),
                        "property_path": "opacity",
                        "old_value": 1.0,
                        "new_value": f64::INFINITY
                    }
                }
            ]
        });
        assert!(validate_apply_operations_args(&inf_json).is_err());
    }

    #[test]
    fn test_unauthorized_property_validation() {
        let invalid_json = serde_json::json!({
            "operations": [],
            "unknown_key": "some_value"
        });
        assert!(validate_apply_operations_args(&invalid_json).is_err());

        let invalid_op_json = serde_json::json!({
            "operations": [
                {
                    "InsertTrack": {
                        "index": 0,
                        "track": {
                            "id": Uuid::new_v4().to_string(),
                            "track_type": "video",
                            "muted": false,
                            "hidden": false,
                            "sync_locked": false,
                            "clips": [],
                            "unauthorized_key": true
                        }
                    }
                }
            ]
        });
        let res = validate_apply_operations_args(&invalid_op_json);
        assert!(res.is_err());
        assert!(res.err().unwrap().contains("Unauthorized property"));
    }

    #[test]
    fn test_compaction_logic() {
        let mut timeline = Timeline::new(1920, 1080, 30.0);
        let mut track = Track::new(ClipType::Text);
        
        let clip1 = Clip {
            id: Uuid::new_v4(),
            media_ref: "text1".to_string(),
            start_frame: 0,
            duration_frames: 30,
            trim_start_frame: 0,
            speed: 1.0,
            content: ClipContent::Text { text_content: "Hello".to_string() },
            link_group_id: None,
        };
        let clip2 = Clip {
            id: Uuid::new_v4(),
            media_ref: "text2".to_string(),
            start_frame: 40,
            duration_frames: 30,
            trim_start_frame: 0,
            speed: 1.0,
            content: ClipContent::Text { text_content: "world".to_string() },
            link_group_id: None,
        };
        
        track.clips.push(clip1);
        track.clips.push(clip2);
        timeline.tracks.push(track);

        let compacted_json = perform_compaction(&timeline);
        
        let tracks_arr = compacted_json.get("tracks").unwrap().as_array().unwrap();
        let clips_arr = tracks_arr[0].get("clips").unwrap().as_array().unwrap();
        
        assert_eq!(clips_arr.len(), 1);
        let group_clip = &clips_arr[0];
        assert_eq!(group_clip.get("media_ref").unwrap().as_str().unwrap(), "group");
        assert!(group_clip.get("text_group").is_some());
        
        assert!(group_clip.get("speed").is_none());
        assert!(group_clip.get("link_group_id").is_none());
    }
}
