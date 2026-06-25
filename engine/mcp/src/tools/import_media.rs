use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;
use uuid::Uuid;
use serde_json::Value;

#[derive(Debug, Deserialize)]
pub struct ImportMediaArgs {
    pub source_path: String,
}

pub fn execute(args: Value) -> Result<Value, String> {
    let parsed: ImportMediaArgs = serde_json::from_value(args)
        .map_err(|e| format!("Invalid arguments: {}", e))?;

    let source_path = Path::new(&parsed.source_path);
    if !source_path.exists() {
        return Err(format!("Source path does not exist: {:?}", source_path));
    }

    let ext = source_path.extension()
        .and_then(|s| s.to_str())
        .unwrap_or("mp4");

    let media_dir = Path::new("project/media");
    fs::create_dir_all(&media_dir).map_err(|e| e.to_string())?;

    let uuid = Uuid::new_v4().to_string();
    let file_name = format!("{}.{}", uuid, ext);
    let dest_path = media_dir.join(&file_name);

    fs::copy(source_path, &dest_path).map_err(|e| e.to_string())?;

    let manifest_path = Path::new("media.json");
    let mut manifest: Value = if manifest_path.exists() {
        let content = fs::read_to_string(manifest_path).map_err(|e| e.to_string())?;
        serde_json::from_str(&content).unwrap_or_else(|_| serde_json::json!({}))
    } else {
        serde_json::json!({})
    };

    if let Some(obj) = manifest.as_object_mut() {
        obj.insert(uuid.clone(), Value::String(file_name));
    }

    fs::write(manifest_path, serde_json::to_string_pretty(&manifest).unwrap())
        .map_err(|e| e.to_string())?;

    Ok(serde_json::json!({ "mediaRef": uuid }))
}
