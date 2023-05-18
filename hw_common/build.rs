use std::{
    env,
    path::{Path, PathBuf},
};

use cc::Build;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let externals_dir = manifest_dir.parent().unwrap().join("externals");
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed={}", externals_dir.display());
    bindgen::builder()
        .header("src/common.h")
        .header("src/callback.h")
        .rustified_enum("*")
        .parse_callbacks(Box::new(MyCallbacks))
        .generate()
        .unwrap()
        .write_to_file(Path::new(&env::var_os("OUT_DIR").unwrap()).join("common_ffi.rs"))
        .unwrap();
    let mut builder = Build::new();
    let dxgi_path = externals_dir.join("nvEncDXGIOutputDuplicationSample");
    builder.include(&dxgi_path);
    for f in vec!["Preproc.cpp"] {
        builder.file(format!("{}/{}", dxgi_path.display(), f));
    }
    builder.compile("hw_common");
}

#[derive(Debug)]
struct MyCallbacks;
impl bindgen::callbacks::ParseCallbacks for MyCallbacks {
    fn add_derives(&self, name: &str) -> Vec<String> {
        let names = vec!["DataFormat", "SurfaceFormat", "API"];
        if names.contains(&name) {
            vec!["Serialize", "Deserialize"]
                .drain(..)
                .map(|s| s.to_string())
                .collect()
        } else {
            vec![]
        }
    }
}
