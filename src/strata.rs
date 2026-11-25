use crate::{
    actions::{self, Result},
    profile::*,
    win,
};
use std::{os::windows::process::CommandExt, path::Path, process::Command};

#[repr(C)]
#[derive(Default)]
struct Fields {
    inkling: i32,
    vector: i32,
    context: u32,
    resume: i32,
    web: i32,
    skills: i32,
}
extern "C" {
    fn ui_strata_activate() -> i32;
    fn ui_strata_open(fields: &Fields, folder: *const u16, x: i32, y: i32) -> i32;
    fn ui_strata_model(ink: i32, vector: i32, context: u32, status: *const u16, status_only: i32);
    fn ui_strata_fields(fields: &mut Fields) -> *const u16;
    fn ui_strata_message(message: *const u16, error: i32);
}
fn launcher(p: &Profile) -> Result<&J> {
    list(&p.cfg, "buttons")
        .iter()
        .find(|v| v["id"] == "strata_qwen")
        .ok_or_else(|| "Нет кнопки strata_qwen".into())
}
pub fn configured(p: &Profile) -> bool {
    p.legacy.is_some() && launcher(p).is_ok()
}
fn provider(p: &Profile, ink: bool, change: Option<&Fields>) -> Result<J> {
    let module = p
        .legacy
        .as_ref()
        .ok_or("Укажите папку провайдера Strata")?
        .join("strata_mode.py");
    if !module.is_file() {
        return Err("Не найден strata_mode.py".into());
    }
    let script = r#"import importlib.util,json,sys
spec=importlib.util.spec_from_file_location("strata_mode",sys.argv[1])
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
cfg=m.INKLING_CONFIG if sys.argv[2]=="Inkling" else None
v=m.selection(cfg) if len(sys.argv)==3 else m.set_selection(sys.argv[3]=="true" and cfg is None,config=cfg,context=int(sys.argv[4]))
print(json.dumps(v))"#;
    let mut c = Command::new("python.exe");
    c.args(["-I", "-X", "utf8", "-c", script])
        .arg(module)
        .arg(if ink { "Inkling" } else { "Qwen" })
        .creation_flags(0x08000000);
    if let Some(f) = change {
        c.arg((f.vector != 0).to_string())
            .arg(f.context.to_string());
    }
    let output = c.output().map_err(|e| e.to_string())?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr)
            .trim()
            .lines()
            .last()
            .unwrap_or("Ошибка провайдера Strata")
            .into());
    }
    serde_json::from_slice(&output.stdout).map_err(|e| e.to_string())
}
fn show_model(state: &J, ink: bool, status_only: bool) {
    let status = if flag(state, "pending", false) {
        "Изменения ждут запуска"
    } else if state["server"].is_object() || state["server"] == true {
        "Сервер запущен"
    } else {
        "Сервер выключен"
    };
    unsafe {
        ui_strata_model(
            ink.into(),
            flag(state, "enabled", false).into(),
            state["context"]
                .as_u64()
                .and_then(|n| n.try_into().ok())
                .unwrap_or_default(),
            win::wide(status).as_ptr(),
            status_only.into(),
        );
    }
}
fn message(text: &str, error: bool) {
    unsafe {
        ui_strata_message(win::wide(text).as_ptr(), error.into());
    }
}
pub fn load_model(p: &Profile, ink: bool, status_only: bool) {
    match provider(p, ink, None) {
        Ok(state) => show_model(&state, ink, status_only),
        Err(e) => message(&e, true),
    }
}
pub fn open(p: &Profile) -> Result {
    if unsafe { ui_strata_activate() != 0 } {
        return Ok(());
    }
    let launcher = launcher(p)?;
    let ink = s(&p.state, "strata_model", "Qwen") == "Inkling";
    let fields = Fields {
        inkling: ink.into(),
        resume: flag(&p.state, "strata_continue", false).into(),
        web: flag(&p.state, "strata_web", true).into(),
        skills: flag(&p.state, "strata_skills", true).into(),
        ..Fields::default()
    };
    let folder = expand(s(&p.state, "strata_project", s(launcher, "cwd", "")));
    let coords = &p.state["strata_window_position"];
    let coord = |i: usize| {
        coords
            .get(i)
            .and_then(J::as_i64)
            .and_then(|n| i32::try_from(n).ok())
            .unwrap_or(i32::MIN)
    };
    if unsafe { ui_strata_open(&fields, win::wide(&folder).as_ptr(), coord(0), coord(1)) } == 0 {
        return Err("Не удалось открыть окно Strata".into());
    }
    load_model(p, ink, false);
    Ok(())
}
fn apply(p: &mut Profile, action: i32) -> Result {
    let mut f = Fields::default();
    let folder = unsafe { win::copy(ui_strata_fields(&mut f)) };
    let folder = folder.trim();
    if f.context == 0 {
        return Err("Введите положительное число токенов.".into());
    }
    if !Path::new(folder).is_absolute() || !Path::new(folder).is_dir() {
        return Err("Выберите существующую рабочую папку.".into());
    }
    let file = s(launcher(p)?, "cmd", "").to_owned();
    let ink = f.inkling != 0;
    let saved = provider(p, ink, Some(&f))?;
    p.state["strata_model"] = if ink { "Inkling" } else { "Qwen" }.into();
    p.state["strata_project"] = folder.into();
    p.state["strata_continue"] = (f.resume != 0).into();
    p.state["strata_web"] = (f.web != 0).into();
    p.state["strata_skills"] = (f.skills != 0).into();
    p.save().map_err(|e| e.to_string())?;
    show_model(&saved, ink, true);
    if action == 0 {
        message(
            if flag(&saved, "pending", false) {
                "Сохранено. Изменения применятся при запуске."
            } else {
                "Настройки сохранены."
            },
            false,
        );
        return Ok(());
    }
    let mut c = Command::new("powershell.exe");
    c.args([
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        &file,
        "-ProjectPath",
        folder,
    ]);
    if ink {
        c.args(["-Model", "Inkling"]);
    }
    if action == 1 {
        c.arg("-ServerOnly");
    } else {
        if f.resume != 0 {
            c.arg("-ContinueSession");
        }
        if f.web == 0 {
            c.arg("-NoWebTools");
        }
        if f.skills == 0 {
            c.arg("-NoSkills");
        }
    }
    c.current_dir(folder).creation_flags(0x10);
    actions::run(c)?;
    message(
        if action == 1 {
            "Запускается Strata…"
        } else {
            "Открывается Qwen Code…"
        },
        false,
    );
    Ok(())
}
pub fn submit(p: &mut Profile, action: i32) {
    if let Err(e) = apply(p, action) {
        message(&e, true);
    }
}
