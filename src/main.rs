#![windows_subsystem = "windows"]
mod actions;
mod bookmarks;
mod jobs;
mod profile;
mod sidebar;
mod strata;
mod win;
use profile::{s, Profile};
use std::{env, path::PathBuf};

fn launch() -> Result<(), String> {
    let mut dir = env::var_os("FAVBAR_PROFILE")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(env::var_os("LOCALAPPDATA").unwrap_or_else(|| ".".into())).join("FavBar")
        });
    let mut legacy = None;
    let mut import = None;
    let mut pocket = None;
    let mut preview = false;
    let mut args = env::args_os().skip(1);
    while let Some(a) = args.next() {
        match a.to_str() {
            Some("--profile") => dir = PathBuf::from(args.next().ok_or("--profile DIRECTORY")?),
            Some("--import") => {
                import = Some(PathBuf::from(args.next().ok_or("--import DIRECTORY")?))
            }
            Some("--legacy-dir") => {
                legacy = Some(PathBuf::from(args.next().ok_or("--legacy-dir DIRECTORY")?))
            }
            Some("--preview") => preview = true,
            Some("--pocket-db") => {
                pocket = Some(PathBuf::from(args.next().ok_or("--pocket-db FILE")?))
            }
            Some("--help") | Some("-h") => {
                win::message("favbar.exe [--profile DIRECTORY] [--legacy-dir DIRECTORY] [--preview]\n\n--import DIRECTORY copies favorites and settings into an unused profile and exits.",false);
                return Ok(());
            }
            _ => return Err(format!("Unknown option: {}", a.to_string_lossy())),
        }
    }
    if let Some(src) = import {
        return actions::import(&src, &dir, pocket.as_deref());
    }
    let mut p = Profile::load(dir, legacy).map_err(|e| e.to_string())?;
    if let Some(path) = pocket {
        p.cfg["pocket_db"] = path.to_string_lossy().into_owned().into();
    }
    if p.legacy.is_none() {
        p.legacy = p.cfg["legacy_dir"]
            .as_str()
            .map(profile::expand)
            .map(PathBuf::from);
    }
    let monitor = &p.state["monitor"];
    let point = (
        monitor.get(0).and_then(|v| v.as_i64()).unwrap_or(0) as i32,
        monitor.get(1).and_then(|v| v.as_i64()).unwrap_or(0) as i32,
    );
    if !win::start(
        preview,
        p.state["width"].as_i64().unwrap_or(260) as i32,
        s(&p.state, "edge", "left") == "right",
        point,
    ) {
        return Ok(());
    }
    let mut app = sidebar::App::new(p, preview);
    app.begin();
    while !app.closing {
        let (k, r, c, d) = win::next();
        if let Err(e) = app.event(k, r, c, d) {
            win::status(&e);
        }
    }
    unsafe { win::ui_close() }
    Ok(())
}
fn main() {
    if let Err(e) = launch() {
        win::message(&e, false);
        std::process::exit(1);
    }
}
