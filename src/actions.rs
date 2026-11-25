use crate::{profile::*, win};
use std::{
    fs,
    os::windows::process::CommandExt,
    path::{Path, PathBuf},
    process::{Child, Command},
};
pub type Result<T = ()> = std::result::Result<T, String>;
pub fn run(mut command: Command) -> Result<Child> {
    command.spawn().map_err(|e| e.to_string())
}
pub fn studio(p: &Profile, selected: &str) -> Result {
    if p.studio().is_empty() {
        return Err("Укажите studio_url в config.json".into());
    }
    let dir = expand(s(&p.cfg, "studio_projects", ""));
    let root = Path::new(&dir);
    let project = if dir.is_empty() {
        let path = Path::new(selected);
        path.ancestors()
            .find(|p| {
                p.parent()
                    .is_some_and(|d| d.ends_with("video-studio/projects"))
            })
            .and_then(Path::file_name)
    } else {
        Path::new(selected)
            .strip_prefix(root)
            .ok()
            .and_then(|p| p.components().next())
            .map(|c| c.as_os_str())
    };
    let mut target = format!("{}/#/studio", p.studio());
    if let Some(n) = project
        .and_then(|v| v.to_str())
        .filter(|v| !v.starts_with(['.', '_']))
    {
        let enc: String = url::form_urlencoded::byte_serialize(n.as_bytes()).collect();
        target.push('/');
        target.push_str(&enc.replace('+', "%20"));
    }
    for base in ["ProgramFiles", "LOCALAPPDATA"] {
        if let Some(v) = std::env::var_os(base) {
            let chrome = PathBuf::from(v).join("Google/Chrome/Application/chrome.exe");
            if chrome.is_file() {
                let mut c = Command::new(chrome);
                c.arg(format!("--app={target}"))
                    .args(["--window-size=1700,1150", "--window-position=300,80"]);
                return run(c).map(drop);
            }
        }
    }
    win::open(&target, "")
}
pub fn command(p: &Profile, b: &J, selected: &str) -> Result<Option<Child>> {
    if flag(b, "studio", false) {
        studio(p, selected)?;
        return Ok(None);
    }
    if flag(b, "confirm", false)
        && !win::message(&format!("Запустить «{}»?", s(b, "name", "")), true)
    {
        return Ok(None);
    }
    let template = s(b, "cmd", "");
    if template.trim().is_empty() {
        return Err("Пустая команда".into());
    }
    let explorer = if template.contains("{explorer}") || s(b, "cwd", "").contains("{explorer}") {
        win::explorer("")
    } else {
        String::new()
    };
    let home = expand("%USERPROFILE%");
    let exp = if explorer.is_empty() {
        if selected.is_empty() {
            &home
        } else {
            selected
        }
    } else {
        &explorer
    };
    let vars = [
        ("{explorer}", exp),
        ("{selected_name}", name(selected)),
        ("{selected}", selected),
    ];
    let substitute = |text: &str| vars.iter().fold(expand(text), |s, (k, v)| s.replace(k, v));
    let raw = substitute(template);
    let target = PathBuf::from(raw.trim().trim_matches('"'));
    let cwd = substitute(s(b, "cwd", ""));
    let console = flag(b, "console", true);
    let flags = if console { 0x10 } else { 0x08000000 };
    let mut cmd;
    if target.is_file() {
        let ext = target
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        match ext.as_str() {
            "ps1" => {
                cmd = Command::new("powershell.exe");
                cmd.args(["-NoProfile", "-ExecutionPolicy", "Bypass"]);
                if console {
                    cmd.arg("-NoExit");
                }
                cmd.arg("-File").arg(&target);
            }
            "cmd" | "bat" => {
                cmd = Command::new("cmd.exe");
                cmd.args(["/d", "/s", if flag(b, "keep", false) { "/k" } else { "/c" }]);
                cmd.env("FAVBAR_SCRIPT", &target)
                    .raw_arg("\"\"%FAVBAR_SCRIPT%\"\"");
            }
            "exe" => cmd = Command::new(&target),
            _ => {
                win::open(&target.to_string_lossy(), &cwd)?;
                return Ok(None);
            }
        }
        if cwd.is_empty() {
            if let Some(parent) = target.parent() {
                cmd.current_dir(parent);
            }
        }
    } else {
        // Delayed expansion occurs after cmd has recognized its control operators.
        // Selected folder names therefore cannot introduce a second shell command.
        let mut safe = template.to_string();
        cmd = Command::new("cmd.exe");
        for ((k, v), variable) in
            vars.iter()
                .zip(["FAVBAR_EXPLORER", "FAVBAR_NAME", "FAVBAR_SELECTED"])
        {
            cmd.env(variable, v);
            safe = safe.replace(k, &format!("!{variable}!"));
        }
        cmd.args([
            "/d",
            if vars.iter().any(|(k, _)| template.contains(k)) {
                "/v:on"
            } else {
                "/v:off"
            },
            "/s",
            if flag(b, "keep", false) { "/k" } else { "/c" },
        ])
        .raw_arg(format!("\"{safe}\""));
    }
    if !cwd.is_empty() {
        cmd.current_dir(cwd);
    }
    cmd.creation_flags(flags);
    run(cmd).map(Some)
}
pub fn terminal(path: &str) -> Result {
    let mut c = Command::new("wt.exe");
    c.arg("-d").arg(path);
    if run(c).is_err() {
        let mut c = Command::new("powershell.exe");
        c.current_dir(path).creation_flags(0x10);
        run(c)?;
    }
    Ok(())
}
pub fn reveal(path: &str) -> Result {
    let mut c = Command::new("explorer.exe");
    c.arg(format!("/select,{path}"));
    run(c).map(drop)
}
pub fn strata(p: &mut Profile) -> Result {
    let legacy = p
        .legacy
        .as_ref()
        .ok_or("Для Strata укажите --legacy-dir с модулем strata_mode.py")?;
    let module = legacy.join("strata_mode.py");
    if !module.is_file() {
        return Err("strata_mode.py не найден".into());
    }
    let launcher = list(&p.cfg, "buttons")
        .iter()
        .find(|v| v["id"] == "strata_qwen")
        .ok_or("Нет кнопки strata_qwen")?;
    let file = s(launcher, "cmd", "").to_string();
    let mut settings = p.state.clone();
    let read = r#"import importlib.util,json,sys
spec=importlib.util.spec_from_file_location("strata_mode",sys.argv[1])
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
ink=sys.argv[2]=="Inkling"
cfg=m.INKLING_CONFIG if ink else None
v=m.selection(cfg) if len(sys.argv)==3 else m.set_selection(sys.argv[3]=="true" and not ink,config=cfg,context=int(sys.argv[4]))
print(json.dumps(v))"#;
    let access = |model: &str, change: Option<(bool, u64)>| -> Result<J> {
        let mut c = Command::new("python.exe");
        c.args(["-I", "-c", read])
            .arg(&module)
            .arg(model)
            .creation_flags(0x08000000);
        if let Some((enabled, context)) = change {
            c.arg(enabled.to_string()).arg(context.to_string());
        }
        let out = c.output().map_err(|e| e.to_string())?;
        if !out.status.success() {
            return Err(String::from_utf8_lossy(&out.stderr).into_owned());
        }
        serde_json::from_slice(&out.stdout).map_err(|e| e.to_string())
    };
    let mut model = s(&settings, "strata_model", "Qwen").to_owned();
    let mut state = access(&model, None)?;
    let mut context = state["context"].as_u64().unwrap_or(32768);
    let mut enabled = flag(&state, "enabled", false);
    let mut folder = s(&settings, "strata_project", s(launcher, "cwd", "")).to_owned();
    loop {
        let labels = vec![
            format!("Модель · {model}"),
            format!("Вектор · {enabled}"),
            format!("Контекст · {context}"),
            format!("Папка · {folder}"),
            format!(
                "Продолжить чат · {}",
                flag(&settings, "strata_continue", false)
            ),
            format!("Веб-инструменты · {}", flag(&settings, "strata_web", true)),
            format!("Навыки · {}", flag(&settings, "strata_skills", true)),
            "Сохранить".into(),
            "Запустить Strata".into(),
            "Открыть Qwen Code".into(),
        ];
        let Some(choice) = win::menu(&labels) else {
            return Ok(());
        };
        match choice {
            0 => {
                model = if model == "Qwen" { "Inkling" } else { "Qwen" }.into();
                state = access(&model, None)?;
                context = state["context"].as_u64().unwrap_or(context);
                enabled = flag(&state, "enabled", false);
            }
            1 if model != "Inkling" => enabled = !enabled,
            1 => {}
            2 => {
                let choices = ["32K", "64K", "96K", "128K", "192K", "256K", "Своё значение"]
                    .map(String::from);
                if let Some(i) = win::menu(&choices) {
                    if i == 6 {
                        let v = win::input("Контекст в токенах", &context.to_string());
                        if !v.is_empty() {
                            context = v.parse().map_err(|_| "Неверное число токенов")?
                        }
                    } else {
                        context = [32, 64, 96, 128, 192, 256][i] * 1024;
                    }
                }
            }
            3 => {
                let choice = win::pick(true);
                if !choice.is_empty() {
                    folder = choice;
                }
            }
            4..=6 => {
                let k = ["strata_continue", "strata_web", "strata_skills"][choice - 4];
                settings[k] = (!flag(&settings, k, choice != 4)).into();
            }
            _ => {
                if !Path::new(&folder).is_dir() {
                    return Err("Выберите рабочую папку".into());
                }
                access(&model, Some((enabled, context)))?;
                settings["strata_model"] = model.clone().into();
                settings["strata_project"] = folder.clone().into();
                p.state = settings;
                p.save().map_err(|e| e.to_string())?;
                if choice >= 8 {
                    let mut c = Command::new("powershell.exe");
                    c.args([
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        &file,
                        "-ProjectPath",
                        &folder,
                    ]);
                    if model == "Inkling" {
                        c.args(["-Model", "Inkling"]);
                    }
                    if choice == 8 {
                        c.arg("-ServerOnly");
                    } else {
                        for (k, default, target, arg) in [
                            ("strata_continue", false, true, "-ContinueSession"),
                            ("strata_web", true, false, "-NoWebTools"),
                            ("strata_skills", true, false, "-NoSkills"),
                        ] {
                            if flag(&p.state, k, default) == target {
                                c.arg(arg);
                            }
                        }
                    }
                    c.current_dir(&folder).creation_flags(0x10);
                    run(c)?;
                }
                return Ok(());
            }
        }
    }
}
pub fn temperature(p: &Profile) -> Result<String> {
    let helper = p
        .legacy
        .as_ref()
        .map(|l| l.join("cpu_temperature.ps1"))
        .filter(|p| p.is_file())
        .ok_or("Укажите путь к установленному провайдеру через --legacy-dir")?;
    let out = Command::new("powershell.exe")
        .args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-File"])
        .arg(helper)
        .arg("-Once")
        .creation_flags(0x08000000)
        .output()
        .map_err(|e| e.to_string())?;
    let j: J = serde_json::from_str(
        String::from_utf8_lossy(&out.stdout)
            .trim_start_matches('\u{feff}')
            .trim(),
    )
    .map_err(|e| e.to_string())?;
    j["temperature"]
        .as_f64()
        .map(|t| format!("CPU Package · {t:.0} °C"))
        .ok_or_else(|| s(&j, "error", "Датчик недоступен").into())
}
pub fn bookmark_db(p: &Profile) -> PathBuf {
    if let Some(s) = p.cfg["pocket_db"].as_str() {
        return PathBuf::from(expand(s));
    }
    p.dir.join("bookmarks.sqlite")
}
pub fn import(src: &Path, dst: &Path, pocket: Option<&Path>) -> Result {
    if src == dst || dst.join("config.json").exists() || dst.join("state.json").exists() {
        return Err("Для импорта выберите новую папку профиля".into());
    }
    let mut docs = Vec::new();
    for n in ["config.json", "state.json"] {
        let f = src.join(n);
        if n == "state.json" && !f.exists() {
            continue;
        }
        let raw = fs::read_to_string(&f).map_err(|e| e.to_string())?;
        let mut j: J =
            serde_json::from_str(raw.trim_start_matches('\u{feff}')).map_err(|e| e.to_string())?;
        let obj = j.as_object_mut().ok_or("Ожидался JSON-объект")?;
        if n == "state.json" {
            obj.remove("hwnd");
        } else {
            if src.join("strata_mode.py").is_file() || src.join("cpu_temperature.ps1").is_file() {
                obj.insert(
                    "legacy_dir".into(),
                    src.canonicalize()
                        .map_err(|e| e.to_string())?
                        .to_string_lossy()
                        .trim_start_matches(r"\\?\")
                        .into(),
                );
            }
            if let Some(path) = pocket {
                obj.insert(
                    "pocket_db".into(),
                    path.to_string_lossy().into_owned().into(),
                );
            }
        }
        docs.push((n, j));
    }
    fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    for (n, j) in docs {
        write(&dst.join(n), &j).map_err(|e| e.to_string())?;
    }
    Ok(())
}
