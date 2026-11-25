fn main() {
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=assets/favbar.ico");
    let target = std::env::var("TARGET").unwrap();
    let compiler =
        cc::windows_registry::find_tool(&target, "cl.exe").expect("Microsoft C++ Build Tools");
    let path = compiler
        .get_envs()
        .rev()
        .find(|(k, _)| k.to_string_lossy().eq_ignore_ascii_case("PATH"))
        .map(|(_, p)| p.to_os_string())
        .unwrap_or_else(|| std::env::var_os("PATH").unwrap_or_default());
    let rc = std::env::split_paths(&path)
        .map(|p| p.join("rc.exe"))
        .find(|p| p.is_file())
        .expect("Windows SDK resource compiler (rc.exe)");
    let resource =
        std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("favbar.res");
    let status = std::process::Command::new(rc)
        .args(["/nologo", "/fo"])
        .arg(&resource)
        .arg("native/favbar.rc")
        .status()
        .expect("start rc.exe");
    assert!(status.success(), "Failed to compile FavBar resources");
    println!("cargo:rustc-link-arg={}", resource.display());
    cc::Build::new()
        .cpp(true)
        .file("native/windows.cpp")
        .flag("/std:c++17")
        .flag("/utf-8")
        .flag("/EHsc")
        .define("UNICODE", None)
        .define("_UNICODE", None)
        .compile("favbar_windows");
    for lib in [
        "user32",
        "gdi32",
        "shell32",
        "ole32",
        "oleaut32",
        "uuid",
        "comdlg32",
        "shlwapi",
        "advapi32",
        "comctl32",
        "windowscodecs",
        "pdh",
        "dwmapi",
    ] {
        println!("cargo:rustc-link-lib={lib}");
    }
}
