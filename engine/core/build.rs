use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let root = PathBuf::from(&manifest)
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();

    let build_dir = root.join("render/build");
    let vcpkg_root = std::env::var("VCPKG_ROOT").unwrap_or_else(|_| "D:/k50i/vcpkg".to_string());
    
    if std::env::var("SKIP_CMAKE").is_ok() {
        println!("cargo:warning=Skipping CMake build due to SKIP_CMAKE environment variable.");
    } else {
        // Configure CMake (ARM64 Release)
        let configure_status = Command::new("cmake")
            .args([
                "-B",
                build_dir.to_str().unwrap(),
                "-S",
                root.join("render").to_str().unwrap(),
                "-A",
                "ARM64",
                "-DCMAKE_BUILD_TYPE=Release",
                &format!(
                    "-DCMAKE_TOOLCHAIN_FILE={}/vcpkg/scripts/buildsystems/vcpkg.cmake",
                    vcpkg_root
                ),
                "-DVCPKG_TARGET_TRIPLET=arm64-windows",
            ])
            .status()
            .expect("Failed to start cmake configure");

        if !configure_status.success() {
            panic!("cmake configure failed with status: {:?}", configure_status);
        }

        let build_status = Command::new("cmake")
            .args([
                "--build",
                build_dir.to_str().unwrap(),
                "--config",
                "Release",
            ])
            .status()
            .expect("Failed to start cmake build");

        if !build_status.success() {
            panic!("cmake build failed with status: {:?}", build_status);
        }
    }

    // Tell cargo where to find the .lib files
    let render_build = root.join("render").join("build");
  
    println!("cargo:rustc-link-search=native={}",
        render_build.join("pipeline").join("Release").display());
    println!("cargo:rustc-link-search=native={}",
        render_build.join("effects").join("Release").display());
    println!("cargo:rustc-link-search=native={}",
        render_build.join("Release").display());
    println!("cargo:rustc-link-search=native={}",
        render_build.display());

    // Search path for vcpkg static libraries (FFmpeg)
    let vcpkg_lib_dir = format!("{}/installed/arm64-windows-static-md/lib", vcpkg_root);
    println!("cargo:rustc-link-search=native={}", vcpkg_lib_dir);

    // Link C++ static libraries
    println!("cargo:rustc-link-lib=static=pipeline");
    println!("cargo:rustc-link-lib=static=decoder");

    // Link FFmpeg static libraries
    println!("cargo:rustc-link-lib=static=avcodec");
    println!("cargo:rustc-link-lib=static=avformat");
    println!("cargo:rustc-link-lib=static=avutil");
    println!("cargo:rustc-link-lib=static=swscale");

    // Windows system libs needed by the C++ code
    for lib in &["d3d12", "dxgi", "d3d11", "d3dcompiler", "dxguid", "bcrypt", "ws2_32", "secur32", "ncrypt", "crypt32", "mfplat", "mfuuid", "strmiids", "ole32", "oleaut32"] {
        println!("cargo:rustc-link-lib={lib}");
    }

    // Force the MSVC linker to export the C++ API symbols from the static library
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:renderer_create");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:renderer_destroy");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_create");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_present");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_resize");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_destroy");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:render_frame");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:audio_engine_create");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:audio_engine_play");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:audio_engine_pause");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:audio_engine_stop");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:audio_engine_current_frame");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:audio_engine_destroy");
    // Undo / redo stack – defined in core/src/ffi.rs.
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:timeline_undo_stack_init");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:timeline_undo_stack_free");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:timeline_checkpoint");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:timeline_undo");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:timeline_redo");
    // Decoder – C++ ABI, needed by dsp mixer for audio frames.
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:decoder_open");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:decoder_get_audio_info");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:decoder_decode_audio_frame");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:decoder_close");

    // Copy DirectML.dll and FFmpeg DLLs to the target output directory
    let target_dir = out_dir
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();

    // Helper function to dynamically search for DLLs in a directory recursively
    fn copy_dlls(dir: &PathBuf, target: &PathBuf, search_patterns: &[&str]) {
        if let Ok(entries) = fs::read_dir(dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    copy_dlls(&path, target, search_patterns);
                } else if path.extension().and_then(|s| s.to_str()) == Some("dll") {
                    if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                        let lower_name = name.to_lowercase();
                        for pattern in search_patterns {
                            if lower_name.contains(pattern) {
                                fs::copy(&path, target.join(name)).ok();
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Search within the render directory (and build/vcpkg_installed) for DirectML and FFmpeg dlls
    copy_dlls(
        &root.join("render"),
        &target_dir,
        &["directml", "avcodec", "avformat", "avutil", "swscale"],
    );

    // Re-run if any C++ source changes
    println!(
        "cargo:rerun-if-changed={}",
        root.join("render/pipeline/src").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        root.join("render/effects/src").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        root.join("render/pipeline/shaders").display()
    );
}
