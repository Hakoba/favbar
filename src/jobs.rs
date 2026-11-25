use crate::{profile::*, win};
use std::os::windows::process::CommandExt;
use std::{
    collections::{HashMap, HashSet},
    fs,
    net::{TcpStream, ToSocketAddrs},
    os::windows::fs::MetadataExt,
    path::{Path, PathBuf},
    process::Command,
    sync::mpsc::Sender,
    thread,
    time::{Duration, Instant, UNIX_EPOCH},
};
pub type Dates = HashMap<String, u64>;
pub enum Update {
    Dates(Dates),
    Status(Vec<(String, bool)>, Option<(usize, usize, HashSet<String>)>),
    Message(String),
    GpuTip(String),
}
pub fn gpu_text(url: &str) -> String {
    let mut msg = format!("Видеопамять по процессам\n\n{}", win::processes());
    if let Some(j) = win::http(&format!("{url}/api/v0/models"), None)
        .and_then(|r| serde_json::from_str::<J>(&r).ok())
    {
        let names: Vec<_> = list(&j, "data")
            .iter()
            .filter(|v| v["state"] == "loaded")
            .filter_map(|v| v["id"].as_str())
            .collect();
        msg.push_str(&format!(
            "\nLM Studio · {}",
            if names.is_empty() {
                "модели не загружены".into()
            } else {
                names.join(", ")
            }
        ));
    }
    msg
}
pub fn send(tx: &Sender<Update>, v: Update) {
    if tx.send(v).is_ok() {
        unsafe { win::ui_wake() }
    }
}
pub fn skipped(n: &str, patterns: &[glob::Pattern], dot: bool) -> bool {
    (dot && n.starts_with('.')) || patterns.iter().any(|p| p.matches(&n.to_lowercase()))
}
pub fn patterns(cfg: &J) -> Vec<glob::Pattern> {
    list(cfg, "skip")
        .iter()
        .filter_map(J::as_str)
        .filter_map(|v| glob::Pattern::new(&v.to_lowercase()).ok())
        .collect()
}
pub fn exclusions(cfg: &J) -> Vec<PathBuf> {
    list(cfg, "index_exclude")
        .iter()
        .filter_map(J::as_str)
        .map(|s| PathBuf::from(key(&expand(s))))
        .collect()
}
fn excluded(p: &Path, exclusions: &[PathBuf]) -> bool {
    if exclusions.is_empty() {
        return false;
    }
    let path = PathBuf::from(key(&p.to_string_lossy()));
    exclusions.iter().any(|d| path.starts_with(d))
}
fn latest(
    p: &Path,
    patterns: &[glob::Pattern],
    dot: bool,
    dates: &mut Dates,
    expanded: &HashSet<String>,
    retain: bool,
    exclusions: &[PathBuf],
) -> u64 {
    if excluded(p, exclusions) {
        return 0;
    }
    let mut newest = 0;
    let path_key = key(&p.to_string_lossy());
    let retain_children = expanded.contains(&path_key);
    let Ok(entries) = fs::read_dir(p) else {
        return 0;
    };
    for e in entries.flatten() {
        if excluded(&e.path(), exclusions) {
            continue;
        }
        let Ok(m) = fs::symlink_metadata(e.path()) else {
            continue;
        };
        if m.file_attributes() & 0x400 != 0 {
            continue;
        }
        if m.is_dir() {
            if !skipped(&e.file_name().to_string_lossy(), patterns, dot) {
                newest = newest.max(latest(
                    &e.path(),
                    patterns,
                    dot,
                    dates,
                    expanded,
                    retain_children,
                    exclusions,
                ));
            }
        } else {
            newest = newest.max(
                m.modified()
                    .ok()
                    .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                    .map_or(0, |t| t.as_secs()),
            );
        }
    }
    if retain || retain_children {
        let own = stamp(p)
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |t| t.as_secs());
        dates.insert(path_key, if newest == 0 { own } else { newest });
    }
    newest
}
pub fn scan(cfg: &J, state: &J, tx: Sender<Update>) {
    let roots: Vec<_> = list(cfg, "favorites")
        .iter()
        .filter_map(|f| f["path"].as_str())
        .map(expand)
        .collect();
    let patterns = patterns(cfg);
    let excluded = exclusions(cfg);
    let mut expanded: HashSet<_> = list(state, "expanded")
        .iter()
        .filter_map(J::as_str)
        .filter_map(|v| v.split_once('|'))
        .map(|(_, p)| key(p))
        .collect();
    expanded.extend(roots.iter().map(|p| key(p)));
    let dot = flag(cfg, "hide_dot_folders", true);
    thread::spawn(move || {
        let mut dates = Dates::new();
        for p in roots {
            if !dates.contains_key(&key(&p)) {
                latest(
                    Path::new(&p),
                    &patterns,
                    dot,
                    &mut dates,
                    &expanded,
                    true,
                    &excluded,
                );
            }
        }
        send(&tx, Update::Dates(dates));
    });
}
pub fn children<'a>(
    path: &str,
    patterns: &'a [glob::Pattern],
    dot: bool,
    exclusions: &'a [PathBuf],
) -> impl Iterator<Item = String> + 'a {
    (!excluded(Path::new(path), exclusions))
        .then(|| fs::read_dir(path).ok())
        .flatten()
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(move |e| {
            let n = e.file_name();
            if excluded(&e.path(), exclusions) {
                return None;
            }
            if skipped(&n.to_string_lossy(), patterns, dot) {
                return None;
            }
            let m = fs::symlink_metadata(e.path()).ok()?;
            if !m.is_dir() || m.file_attributes() & 0x400 != 0 {
                return None;
            }
            Some(e.path().to_string_lossy().into_owned())
        })
}
pub fn probe(services: Vec<String>, comfy: Option<String>, tx: Sender<Update>) {
    thread::spawn(move || {
        let queue = comfy
            .and_then(|base| win::http(&format!("{base}/queue"), None))
            .and_then(|v| serde_json::from_str::<J>(&v).ok())
            .map(|q| {
                let (r, p) = (list(&q, "queue_running"), list(&q, "queue_pending"));
                (
                    r.len(),
                    p.len(),
                    r.iter()
                        .chain(p)
                        .filter_map(|v| v.get(1)?.as_str().map(String::from))
                        .collect(),
                )
            });
        let status = services
            .into_iter()
            .map(|url| {
                let up = url::Url::parse(&url)
                    .ok()
                    .and_then(|u| Some((u.host_str()?.to_owned(), u.port_or_known_default()?)))
                    .and_then(|addr| addr.to_socket_addrs().ok())
                    .is_some_and(|mut ips| {
                        ips.any(|ip| {
                            TcpStream::connect_timeout(&ip, Duration::from_millis(400)).is_ok()
                        })
                    });
                (url, up)
            })
            .collect();
        send(&tx, Update::Status(status, queue));
    });
}
#[derive(Default)]
pub struct Queue {
    start: Option<Instant>,
    empty: Option<Instant>,
    seen: HashSet<String>,
    done: usize,
    pub label: String,
}
impl Queue {
    pub fn update(
        &mut self,
        value: Option<(usize, usize, HashSet<String>)>,
        mode: &str,
        minutes: u64,
    ) {
        let Some((run, wait, ids)) = value else {
            *self = Self {
                label: "выключен".into(),
                ..Self::default()
            };
            return;
        };
        self.done += self.seen.difference(&ids).count();
        self.seen = ids;
        if run + wait > 0 {
            let start = *self.start.get_or_insert_with(Instant::now);
            self.empty = None;
            self.label = format!(
                "{}с · {} в работе · {} ждут",
                start.elapsed().as_secs(),
                run,
                wait
            );
        } else {
            self.label = "свободен".into();
            if let Some(start) = self.start {
                let end = *self.empty.get_or_insert_with(Instant::now);
                if end.elapsed() >= Duration::from_secs(10) {
                    let seconds = end.duration_since(start).as_secs();
                    if mode == "all" || (mode == "long" && seconds >= minutes.saturating_mul(60)) {
                        notify(&format!("Завершено задач: {} за {} с", self.done, seconds));
                    }
                    self.start = None;
                    self.done = 0;
                    self.empty = None;
                }
            }
        }
    }
}
fn notify(text: &str) {
    let script = r#"[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime] > $null
$x=New-Object Windows.Data.Xml.Dom.XmlDocument
$x.LoadXml('<toast><visual><binding template="ToastGeneric"><text>FavBar · ComfyUI</text><text></text></binding></visual></toast>')
$x.SelectNodes('//text').Item(1).InnerText=$env:FAVBAR_MESSAGE
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe').Show([Windows.UI.Notifications.ToastNotification]::new($x))"#;
    let _ = Command::new("powershell.exe")
        .args(["-NoProfile", "-Command", script])
        .env("FAVBAR_MESSAGE", text)
        .creation_flags(0x08000000)
        .spawn();
}
pub fn hermes() -> Option<bool> {
    let home = std::env::var_os("HERMES_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("LOCALAPPDATA").map(|p| PathBuf::from(p).join("hermes")))?;
    for n in ["gateway.pid", "gateway_state.json"] {
        if let Ok(raw) = fs::read(home.join(n)) {
            let rec: J = serde_json::from_slice(&raw).ok()?;
            if rec["kind"] != "hermes-gateway" || rec["gateway_state"] == "stopped" {
                continue;
            }
            if let Some(p) = rec["hermes_home"].as_str() {
                if key(p) != key(&home.to_string_lossy()) {
                    continue;
                }
            }
            let Some(pid) = rec["pid"].as_u64().and_then(|v| u32::try_from(v).ok()) else {
                continue;
            };
            let birth = unsafe { win::process_birth(pid) };
            if birth > 0
                && rec["start_time"]
                    .as_i64()
                    .is_some_and(|t| t.abs_diff(birth) <= 1)
            {
                return Some(true);
            }
        }
    }
    Some(false)
}
pub fn queue_post(base: &str, path: &str, body: J, tx: Sender<Update>) {
    let url = format!("{base}{path}");
    thread::spawn(move || {
        let ok = win::http(&url, Some(&body.to_string())).is_some();
        send(
            &tx,
            Update::Message(
                if ok {
                    "Готово"
                } else {
                    "ComfyUI не ответил"
                }
                .into(),
            ),
        );
    });
}
