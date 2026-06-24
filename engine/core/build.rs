fn main() {
    // Search directories for the compiled C++ static libraries
    println!("cargo:rustc-link-search=native=d:/k50i/do  it/build/pipeline/Release");
    println!("cargo:rustc-link-search=native=d:/k50i/do  it/build/pipeline/Debug");
    println!("cargo:rustc-link-search=native=d:/k50i/do  it/build/pipeline");

    // Link the C++ pipeline static library
    println!("cargo:rustc-link-lib=static=pipeline");

    // Link the necessary Windows DirectX 12 system libraries
    println!("cargo:rustc-link-lib=dylib=d3d12");
    println!("cargo:rustc-link-lib=dylib=dxgi");
    println!("cargo:rustc-link-lib=dylib=d3dcompiler");
    println!("cargo:rustc-link-lib=dylib=user32");
    println!("cargo:rustc-link-lib=dylib=ole32");

    // Force the MSVC linker to export the C++ API symbols from the static library
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:renderer_create");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:renderer_destroy");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_create");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_present");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:presenter_destroy");
    println!("cargo:rustc-cdylib-link-arg=/EXPORT:render_frame");
}
