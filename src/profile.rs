use crate::win;
use serde_json::{json, Value};
use std::{
    env, fs,
    io::{self, Write},
    path::{Path, PathBuf},
    time::SystemTime,
};
pub type J = Value;
pub fn s<'a>(v: &'a J, k: &str, default: &'a str) -> &'a str {
    v[k].as_str().unwrap_or(default)
}
pub fn flag(v: &J, k: &str, default: bool) -> bool {
    v[k].as_bool().unwrap_or(default)
}
pub fn list<'a>(v: &'a J, k: &str) -> &'a [J] {
    v[k].as_array().map(Vec::as_slice).unwrap_or(&[])
}
pub fn array<'a>(v: &'a mut J, k: &str) -> &'a mut Vec<J> {
    if !v[k].is_array() {
        v[k] = json!([])
    }
    v[k].as_array_mut().unwrap()
}
pub fn expand(input: &str) -> String {
    let mut value = input.to_string();
    if value == "~" || value.starts_with("~/") || value.starts_with("~\\") {
        value.replace_range(..1, &env::var("USERPROFILE").unwrap_or_default());
    }
    let mut out = String::new();
    let mut rest = value.as_str();
    while let Some(start) = rest.find('%') {
        out.push_str(&rest[..start]);
        rest = &rest[start..];
        let Some(end) = rest[1..].find('%').map(|n| n + 1) else {
            break;
        };
        if let Ok(val) = env::var(&rest[1..end]) {
            out.push_str(&val)
        } else {
            out.push_str(&rest[..=end])
        }
        rest = &rest[end + 1..];
    }
    out.push_str(rest);
    out
}
pub fn key(p: &str) -> String {
    p.replace('/', "\\").to_lowercase()
}
pub fn name(p: &str) -> &str {
    Path::new(p)
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or(p)
}
pub fn stamp(p: &Path) -> Option<SystemTime> {
    fs::metadata(p).and_then(|m| m.modified()).ok()
}
fn read(path: &Path) -> io::Result<J> {
    let text = fs::read_to_string(path)?;
    let v: J = serde_json::from_str(text.trim_start_matches('\u{feff}'))?;
    if !v.is_object() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Expected a JSON object",
        ));
    }
    Ok(v)
}
pub fn write(path: &Path, v: &J) -> io::Result<()> {
    let tmp = path.with_extension(format!("{}.tmp", std::process::id()));
    let mut file = fs::File::create(&tmp)?;
    serde_json::to_writer_pretty(&mut file, v)?;
    file.write_all(b"\n")?;
    file.sync_all()?;
    drop(file);
    win::replace(&tmp, path)
}
pub struct Profile {
    pub dir: PathBuf,
    pub cfg: J,
    pub state: J,
    pub legacy: Option<PathBuf>,
    pub config_path: PathBuf,
    overrides: J,
    modified: Option<SystemTime>,
}
impl Profile {
    pub fn load(dir: PathBuf, legacy: Option<PathBuf>) -> io::Result<Self> {
        fs::create_dir_all(&dir)?;
        let opts = dir.join("native.json");
        let overrides = if opts.exists() {
            read(&opts)?
        } else {
            json!({})
        };
        let path = overrides["config_file"]
            .as_str()
            .map(expand)
            .map(PathBuf::from)
            .unwrap_or_else(|| dir.join("config.json"));
        if overrides["config_file"].is_string() && !path.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                "Linked config.json is missing",
            ));
        }
        if !path.exists() {
            write(
                &path,
                &serde_json::from_str(include_str!("../config.example.json"))?,
            )?;
        }
        let cfg = Self::effective(read(&path)?, &overrides);
        let state = if dir.join("state.json").exists() {
            read(&dir.join("state.json"))?
        } else {
            json!({})
        };
        Ok(Self {
            dir,
            cfg,
            state,
            legacy,
            modified: stamp(&path),
            config_path: path,
            overrides,
        })
    }
    fn effective(mut cfg: J, overrides: &J) -> J {
        for k in ["legacy_dir", "pocket_db", "index_exclude", "lm_url"] {
            if let Some(v) = overrides.get(k) {
                cfg[k] = v.clone();
            }
        }
        cfg
    }
    pub fn reload(&mut self) -> io::Result<bool> {
        let path = self.config_path.clone();
        if self.modified == stamp(&path) {
            return Ok(false);
        }
        self.cfg = Self::effective(read(&path)?, &self.overrides);
        self.modified = stamp(&path);
        Ok(true)
    }
    pub fn config(&mut self) -> io::Result<()> {
        let path = self.config_path.clone();
        if self.reload()? {
            return Err(io::Error::other(
                "Конфиг изменился извне. Повторите действие.",
            ));
        }
        let mut cfg = read(&path)?;
        for key in ["favorites", "services"] {
            if let Some(value) = self.cfg.get(key) {
                cfg[key] = value.clone();
            }
        }
        write(&path, &cfg)?;
        self.modified = stamp(&path);
        Ok(())
    }
    pub fn save(&self) -> io::Result<()> {
        write(&self.dir.join("state.json"), &self.state)
    }
    pub fn folded(&self, k: &str) -> bool {
        list(&self.state, "collapsed").iter().any(|v| v == k)
    }
    pub fn toggle(&mut self, k: &str, value: &str) {
        let a = array(&mut self.state, k);
        if a.iter().any(|v| v == value) {
            a.retain(|v| v != value)
        } else {
            a.push(json!(value))
        }
    }
    pub fn studio(&self) -> &str {
        s(&self.cfg, "studio_url", "").trim_end_matches('/')
    }
    pub fn comfy(&self) -> Option<String> {
        self.cfg["comfy_url"]
            .as_str()
            .or_else(|| {
                list(&self.cfg, "services")
                    .iter()
                    .any(|v| s(v, "url", "").contains(":8188"))
                    .then_some("http://127.0.0.1:8188")
            })
            .map(|v| v.trim_end_matches('/').to_owned())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn linked_legacy_config_keeps_native_settings_private() {
        let dir = env::temp_dir().join(format!("favbar-linked-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let source = dir.join("legacy.json");
        write(&source, &json!({"buttons":[],"future":42})).unwrap();
        write(
            &dir.join("native.json"),
            &json!({"config_file":source,"index_exclude":["example"]}),
        )
        .unwrap();
        let mut p = Profile::load(dir.clone(), None).unwrap();
        assert_eq!(p.cfg["index_exclude"][0], "example");
        p.cfg["favorites"] = json!([{"name":"Example"}]);
        p.config().unwrap();
        let saved = read(&source).unwrap();
        assert_eq!(saved["future"], 42);
        assert!(saved.get("index_exclude").is_none());
        fs::remove_file(source).unwrap();
        fs::remove_file(dir.join("native.json")).unwrap();
        fs::remove_dir(dir).unwrap();
    }
    #[test]
    fn preserve_unknown() {
        let dir = env::temp_dir().join(format!("favbar-test-{}", std::process::id()));
        let mut profile = Profile::load(dir.clone(), None).unwrap();
        profile.state = json!({"unknown":{"x":2}});
        profile.toggle("collapsed", "system");
        profile.save().unwrap();
        assert_eq!(
            Profile::load(dir.clone(), None).unwrap().state["unknown"]["x"],
            2
        );
        for n in ["config.json", "state.json"] {
            fs::remove_file(dir.join(n)).unwrap()
        }
        fs::remove_dir(dir).unwrap();
    }
}
