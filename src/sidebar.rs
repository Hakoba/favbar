use crate::{
    actions, bookmarks,
    jobs::{self, Dates, Queue, Update},
    profile::*,
    win,
};
use serde_json::json;
use std::{
    collections::HashMap,
    path::Path,
    process::{Child, Command},
    sync::mpsc::{self, Receiver, Sender},
    time::{Duration, Instant},
};
#[derive(Clone, Default)]
enum Action {
    #[default]
    None,
    Menu,
    Home,
    Add,
    Hide,
    Section(&'static str),
    Vm,
    Folder {
        root: usize,
        path: String,
    },
    Expand {
        root: usize,
        path: String,
    },
    Button(usize),
    Service(usize),
    Shelf,
    ShelfAdd,
    ShelfClear,
    File(String),
    Reveal(String),
    Link,
    Links,
    Strata,
    Metric(&'static str),
}
struct Row {
    left: String,
    right: String,
    hint: String,
    path: String,
    style: i32,
    depth: i32,
    actions: [Action; 2],
}
impl Row {
    fn new(text: impl Into<String>, act: Action) -> Self {
        Self {
            left: text.into(),
            right: String::new(),
            hint: String::new(),
            path: String::new(),
            style: 0,
            depth: 0,
            actions: [act, Action::None],
        }
    }
    fn right(mut self, text: impl Into<String>, act: Action) -> Self {
        self.right = text.into();
        self.actions[1] = act;
        self
    }
    fn style(mut self, s: i32) -> Self {
        self.style = s;
        self
    }
    fn hint(mut self, s: impl Into<String>) -> Self {
        self.hint = s.into();
        self
    }
}
enum Directory {
    Branch,
    Listed(Vec<String>),
}
pub struct App {
    pub profile: Profile,
    rows: Vec<Row>,
    selected: String,
    dates: Dates,
    dirs: HashMap<String, Directory>,
    tx: Sender<Update>,
    rx: Receiver<Update>,
    scanning: bool,
    probing: bool,
    tip_busy: bool,
    last_scan: Option<Instant>,
    last_probe: Option<Instant>,
    visible: bool,
    preview: bool,
    services: HashMap<String, bool>,
    queue: Queue,
    metrics: [f64; 7],
    gpu: bool,
    hermes: Option<Child>,
    hermes_on: Option<bool>,
    last_status: Option<Instant>,
    pub closing: bool,
}
impl App {
    pub fn new(profile: Profile, preview: bool) -> Self {
        let (tx, rx) = mpsc::channel();
        Self {
            profile,
            rows: vec![],
            selected: String::new(),
            dates: Dates::new(),
            dirs: HashMap::new(),
            tx,
            rx,
            scanning: false,
            probing: false,
            tip_busy: false,
            last_scan: None,
            last_probe: None,
            visible: true,
            preview,
            services: HashMap::new(),
            queue: Queue::default(),
            metrics: [0.; 7],
            gpu: false,
            hermes: None,
            hermes_on: None,
            last_status: None,
            closing: false,
        }
    }
    fn say(&mut self, s: &str) {
        win::status(s);
        self.last_status = Some(Instant::now())
    }
    fn save(&mut self) {
        if let Err(e) = self.profile.save() {
            self.say(&e.to_string())
        }
    }
    fn edited(&mut self) {
        if let Err(e) = self.profile.config() {
            self.say(&e.to_string())
        }
        self.dirs.clear();
    }
    fn scan(&mut self) {
        if !self.scanning {
            self.scanning = true;
            self.last_scan = Some(Instant::now());
            jobs::scan(&self.profile.cfg, &self.profile.state, self.tx.clone());
        }
    }
    fn folder(&mut self, root: usize, path: String, label: String, depth: i32) {
        let cfg = &self.profile.cfg;
        let rootpath = expand(s(&cfg["favorites"][root], "path", ""));
        let id = format!("{}|{}", key(&rootpath), key(&path));
        let open = list(&self.profile.state, "expanded")
            .iter()
            .any(|v| v == &id);
        let home = depth == 0 && flag(&cfg["favorites"][root], "home", false);
        let date = self
            .dates
            .get(&key(&path))
            .copied()
            .filter(|&v| v != 0)
            .map(win::date)
            .unwrap_or_default();
        // A closed branch only needs its first visible child. Keep that answer across redraws.
        let read = |all: bool| {
            let patterns = jobs::patterns(cfg);
            let excluded = jobs::exclusions(cfg);
            let mut entries = jobs::children(
                &path,
                &patterns,
                flag(cfg, "hide_dot_folders", true),
                &excluded,
            );
            if all {
                Directory::Listed(entries.collect())
            } else if entries.next().is_some() {
                Directory::Branch
            } else {
                Directory::Listed(vec![])
            }
        };
        let directory = self
            .dirs
            .entry(key(&path))
            .or_insert_with(|| read(open && depth < 64));
        if open && depth < 64 && matches!(directory, Directory::Branch) {
            *directory = read(true);
        }
        let branch = depth < 64
            && match directory {
                Directory::Branch => true,
                Directory::Listed(children) => !children.is_empty(),
            };
        let navigate = Action::Folder {
            root,
            path: path.clone(),
        };
        let mut r = Row::new(
            format!("{}{}", if home { "⌂ " } else { "" }, label),
            navigate.clone(),
        )
        .right(
            format!(
                "{}  {date}",
                if !branch {
                    ""
                } else if open {
                    "▾"
                } else {
                    "▸"
                }
            ),
            if branch {
                Action::Expand {
                    root,
                    path: path.clone(),
                }
            } else {
                navigate
            },
        )
        .hint(&path)
        .style(2);
        r.depth = depth;
        r.path = s(&self.profile.state["folder_icons"], &key(&path), "").into();
        self.rows.push(r);
        if !open || depth >= 64 {
            return;
        }
        let Directory::Listed(children) = directory else {
            return;
        };
        let by_date = s(&self.profile.state, "sort", "date") == "date";
        children.sort_by_cached_key(|v| {
            (
                std::cmp::Reverse(if by_date {
                    self.dates.get(&key(v)).copied().unwrap_or(0)
                } else {
                    0
                }),
                name(v).to_lowercase(),
            )
        });
        for child in children.clone() {
            let n = name(&child).to_string();
            self.folder(root, child, n, depth + 1);
        }
    }
    fn section(&mut self, k: &'static str, title: &str) -> bool {
        let open = !self.profile.folded(k);
        self.rows.push(
            Row::new(
                format!("{}  {title}", if open { "▾" } else { "▸" }),
                Action::Section(k),
            )
            .style(1),
        );
        open
    }
    fn pair(&mut self, items: Vec<(String, Action, String)>) {
        for line in items.chunks(2) {
            let (l, a, h) = &line[0];
            let mut row = Row::new(l, a.clone()).hint(h);
            if let Some((r, b, hh)) = line.get(1) {
                row = row.right(r, b.clone());
                row.hint = format!("{h}   ·   {hh}");
            }
            self.rows.push(row);
        }
    }
    pub fn render(&mut self) {
        self.rows.clear();
        self.rows.push(
            Row::new("FavBar", Action::Menu)
                .right("Скрыть", Action::Hide)
                .hint(s(&self.profile.cfg, "hotkey", "Win+Shift+F"))
                .style(6),
        );
        self.rows.push(
            Row::new("Избранное", Action::Home)
                .right("Добавить", Action::Add)
                .hint("Домой · добавить текущую папку в избранное")
                .style(7),
        );
        let mut roots: Vec<_> = list(&self.profile.cfg, "favorites")
            .iter()
            .enumerate()
            .filter_map(|(i, v)| {
                let p = expand(v["path"].as_str()?);
                Some((
                    i,
                    flag(v, "home", false),
                    s(v, "name", name(&p)).to_owned(),
                    p,
                ))
            })
            .collect();
        roots.sort_by_key(|v| !v.1);
        for (i, _, n, p) in roots {
            self.folder(i, p, n, 0);
        }
        if !self.profile.studio().is_empty() || self.profile.cfg["pocket_db"].is_string() {
            self.rows.push(
                Row::new("Сохранить ссылку", Action::Link)
                    .right("↗ Закладки", Action::Links)
                    .style(5),
            );
        }
        let paths = list(&self.profile.state, "shelf_paths");
        let shelf_open = flag(&self.profile.state, "shelf_open", false);
        self.rows.push(
            Row::new(
                format!(
                    "{} Полка ({})",
                    if shelf_open { "▾" } else { "▸" },
                    paths.len()
                ),
                Action::Shelf,
            )
            .right("＋ Файлы", Action::ShelfAdd)
            .style(3)
            .hint("Перетащите файлы сюда. Правый клик — очистить полку."),
        );
        if shelf_open {
            for p in paths.iter().filter_map(J::as_str) {
                let mut r = Row::new(name(p), Action::File(p.into()))
                    .right("↗ В папку", Action::Reveal(p.into()))
                    .style(4)
                    .hint(p);
                r.path = p.into();
                self.rows.push(r);
            }
        }
        if self.section("buttons", "Скрипты") {
            let buttons = list(&self.profile.cfg, "buttons").to_vec();
            let mut groups: Vec<String> = vec![];
            for g in [
                "Запуск",
                "Ассистенты",
                "Инструменты",
                "Экран и питание",
                "Виртуалки",
            ] {
                if buttons.iter().any(|b| b["group"] == g) {
                    groups.push(g.into())
                }
            }
            for b in &buttons {
                let g = s(b, "group", "Прочее");
                if !groups.iter().any(|v| v == g) {
                    groups.push(g.into())
                }
            }
            for group in groups {
                let vm = buttons
                    .iter()
                    .filter(|v| s(v, "group", "Прочее") == group)
                    .all(|v| v["agentbox_action"].is_string());
                self.rows.push(
                    Row::new(
                        if vm {
                            format!(
                                "{} {group}",
                                if flag(&self.profile.state, "vm_group_open", false) {
                                    "▾"
                                } else {
                                    "▸"
                                }
                            )
                        } else {
                            group.clone()
                        },
                        if vm { Action::Vm } else { Action::None },
                    )
                    .style(1),
                );
                if vm && !flag(&self.profile.state, "vm_group_open", false) {
                    continue;
                }
                let mut items = vec![];
                let mut strata = false;
                for (i, b) in buttons
                    .iter()
                    .enumerate()
                    .filter(|(_, v)| s(v, "group", "Прочее") == group)
                {
                    if self.profile.legacy.is_some()
                        && matches!(s(b, "id", ""), "strata_only" | "strata_qwen")
                    {
                        strata = true;
                        continue;
                    }
                    let mut n = s(b, "label", s(b, "name", "?")).to_owned();
                    if vm {
                        if let Some((name, action)) = s(b, "agentbox_action", "").split_once(':') {
                            n = format!("{} {name}", if action == "start" { "▶" } else { "■" });
                        }
                    }
                    if s(b, "cmd", "").to_lowercase().ends_with("hermes_tg.bat") {
                        n = format!(
                            "Hermes TG · {}",
                            if self.hermes_on == Some(true) {
                                "вкл"
                            } else if self.hermes.is_some() {
                                "старт…"
                            } else {
                                "выкл"
                            }
                        );
                    }
                    items.push((n, Action::Button(i), s(b, "name", "").into()));
                }
                self.pair(items);
                if strata {
                    self.rows
                        .push(Row::new("Настройки Strata и Qwen Code", Action::Strata));
                }
            }
        }
        if self.section("services", "Сервисы") {
            let items = list(&self.profile.cfg, "services")
                .iter()
                .enumerate()
                .filter(|(_, v)| !flag(v, "hidden", false))
                .map(|(i, v)| {
                    let u = s(v, "url", "");
                    (
                        format!(
                            "{} {}",
                            match self.services.get(u) {
                                Some(true) => "●",
                                Some(false) => "○",
                                _ => "·",
                            },
                            s(v, "label", s(v, "name", u))
                        ),
                        Action::Service(i),
                        u.into(),
                    )
                })
                .collect();
            self.pair(items);
        }
        if self.section("system", "Система") {
            let v = &self.metrics;
            let gib = 1073741824.;
            self.rows.push(
                Row::new(format!("CPU  {:.0}%", v[2]), Action::Metric("cpu"))
                    .right(
                        format!("RAM · ГБ  {:.1} / {:.1}", v[0] / gib, v[1] / gib),
                        Action::Metric("ram"),
                    )
                    .hint("Температура CPU по нажатию"),
            );
            let gpu = if self.gpu {
                if v[6] > 0. {
                    format!("GPU  {:.0}% · {:.0}°", v[5], v[6])
                } else {
                    format!("GPU  {:.0}%", v[5])
                }
            } else {
                "GPU  —".into()
            };
            let mem = if self.gpu {
                format!("VRAM · ГБ  {:.1} / {:.1}", v[3] / gib, v[4] / gib)
            } else {
                "VRAM  —".into()
            };
            self.rows.push(
                Row::new(gpu, Action::Metric("gpu"))
                    .right(mem, Action::Metric("vram"))
                    .hint("Видеопамять по процессам"),
            );
            if self.profile.comfy().is_some() {
                self.rows.push(
                    Row::new(
                        format!("ComfyUI · {}", self.queue.label),
                        Action::Metric("comfy"),
                    )
                    .hint("Правый клик — управление очередью и уведомления"),
                );
            }
        }
        unsafe { win::ui_clear() }
        for r in &self.rows {
            let style = match &r.actions[0] {
                Action::None | Action::Vm => 8,
                Action::Service(_) => 9,
                Action::Metric("comfy") => 11,
                Action::Metric(_) => 10,
                Action::Section("system") => 12,
                _ => r.style,
            };
            let details = r.actions.each_ref().map(|a| match a {
                Action::Button(i) => button_glyph(&self.profile.cfg["buttons"][i]),
                Action::Strata => 0xe713,
                Action::Metric("cpu") => self.metrics[2] as i32,
                Action::Metric("gpu") => self.metrics[5] as i32,
                Action::Metric("ram") => (100. * self.metrics[0] / self.metrics[1]) as i32,
                Action::Metric("vram") => (100. * self.metrics[3] / self.metrics[4]) as i32,
                _ => 0,
            });
            win::row(&r.left, &r.right, &r.hint, &r.path, style, r.depth, details)
        }
        win::finish(s(&self.profile.state, "theme", "auto"));
    }
    pub fn tick(&mut self) {
        if self.last_status.is_some_and(|t| t.elapsed().as_secs() >= 5) {
            win::status("");
            self.last_status = None;
        }
        if self.visible && !self.profile.folded("system") {
            self.gpu = unsafe { win::metrics(self.metrics.as_mut_ptr()) != 0 };
        }
        if let Some(c) = self.hermes.as_mut() {
            if c.try_wait().ok().flatten().is_some() {
                self.hermes = None;
            }
        }
        if self.visible
            && list(&self.profile.cfg, "buttons").iter().any(|b| {
                s(b, "cmd", "")
                    .to_ascii_lowercase()
                    .ends_with("hermes_tg.bat")
            })
        {
            self.hermes_on = jobs::hermes();
        }
        let queue = self
            .profile
            .comfy()
            .filter(|_| self.visible || s(&self.profile.state, "notify", "long") != "off");
        let services = if self.visible
            && !self.profile.folded("services")
            && self
                .last_probe
                .is_none_or(|t| t.elapsed() >= Duration::from_secs(5))
        {
            self.last_probe = Some(Instant::now());
            list(&self.profile.cfg, "services")
                .iter()
                .filter(|v| !flag(v, "hidden", false))
                .filter_map(|v| v["url"].as_str().map(str::to_owned))
                .collect()
        } else {
            vec![]
        };
        if !self.probing && (queue.is_some() || !services.is_empty()) {
            self.probing = true;
            jobs::probe(services, queue, self.tx.clone());
        }
    }
    fn timer(&self) {
        let needed = self.visible
            || (self.profile.comfy().is_some()
                && s(&self.profile.state, "notify", "long") != "off");
        unsafe { win::ui_timer(if needed { 2000 } else { 0 }) }
    }
    pub fn begin(&mut self) {
        self.scan();
        self.tick();
        self.render();
        unsafe { win::ui_show(1) }
        self.timer();
        if !self.preview && !win::hotkey(s(&self.profile.cfg, "hotkey", "Win+Shift+F")) {
            self.say("Хоткей занят или указан неверно");
        }
    }
    fn updates(&mut self) {
        while let Ok(v) = self.rx.try_recv() {
            match v {
                Update::Dates(d) => {
                    self.dates = d;
                    self.dirs.clear();
                    self.scanning = false;
                }
                Update::Message(s) => self.say(&s),
                Update::GpuTip(s) => {
                    self.tip_busy = false;
                    win::tip(&s);
                }
                Update::Status(services, q) => {
                    self.probing = false;
                    self.services.extend(services);
                    self.queue.update(
                        q,
                        s(&self.profile.state, "notify", "long"),
                        self.profile.cfg["notify_min"].as_u64().unwrap_or(2),
                    );
                }
            }
        }
    }
    fn add(&mut self, path: &str) {
        if !path.is_empty()
            && !list(&self.profile.cfg, "favorites")
                .iter()
                .any(|v| key(&expand(s(v, "path", ""))) == key(path))
        {
            array(&mut self.profile.cfg, "favorites").push(json!({"name":name(path),"path":path}));
            self.edited();
            self.scan();
        }
    }
    fn add_file(&mut self, path: &str) {
        if Path::new(path).is_file()
            && !list(&self.profile.state, "shelf_paths")
                .iter()
                .filter_map(J::as_str)
                .any(|v| key(v) == key(path))
        {
            array(&mut self.profile.state, "shelf_paths").push(json!(path));
            self.save();
        }
    }
    fn action(&mut self, a: Action) -> actions::Result {
        match a {
            Action::None => {}
            Action::Menu => self.menu()?,
            Action::Home => {
                if let Some(v) = list(&self.profile.cfg, "favorites")
                    .iter()
                    .find(|v| flag(v, "home", false))
                {
                    win::explorer(&expand(s(v, "path", "")));
                } else {
                    self.say("Домашняя папка не задана")
                }
            }
            Action::Add => {
                let mut path = win::explorer("");
                if path.is_empty() {
                    path = win::pick(true);
                }
                self.add(&path);
            }
            Action::Hide => {
                unsafe { win::ui_show(0) }
                self.visible = false;
                self.timer();
            }
            Action::Section(k) => {
                self.profile.toggle("collapsed", k);
                self.save();
            }
            Action::Vm => {
                self.profile.state["vm_group_open"] =
                    (!flag(&self.profile.state, "vm_group_open", false)).into();
                self.save();
            }
            Action::Folder { path, .. } => {
                self.selected = path.clone();
                if win::explorer(&path).is_empty() {
                    return Err(format!("Не удалось открыть {path}"));
                }
            }
            Action::Expand { root, path } => {
                self.selected = path.clone();
                let root = expand(s(&self.profile.cfg["favorites"][root], "path", ""));
                self.profile
                    .toggle("expanded", &format!("{}|{}", key(&root), key(&path)));
                self.save();
            }
            Action::Button(i) => {
                let b = &self.profile.cfg["buttons"][i];
                let hermes = s(b, "cmd", "")
                    .to_ascii_lowercase()
                    .ends_with("hermes_tg.bat");
                if hermes && (jobs::hermes() != Some(false) || self.hermes.is_some()) {
                    self.say("Hermes уже работает или запускается");
                    return Ok(());
                }
                let child = actions::command(&self.profile, b, &self.selected)?;
                if hermes {
                    self.hermes = child;
                }
            }
            Action::Service(i) => win::open(s(&self.profile.cfg["services"][i], "url", ""), "")?,
            Action::Shelf => {
                self.profile.state["shelf_open"] =
                    (!flag(&self.profile.state, "shelf_open", false)).into();
                self.save();
            }
            Action::ShelfClear => {
                self.profile.state["shelf_paths"] = json!([]);
                self.save();
            }
            Action::ShelfAdd => {
                for p in win::pick(false).lines() {
                    self.add_file(p);
                }
            }
            Action::File(p) => win::open(&p, "")?,
            Action::Reveal(p) => actions::reveal(&p)?,
            Action::Link => {
                let msg =
                    bookmarks::save(&actions::bookmark_db(&self.profile), &win::clipboard(None))?;
                self.say(&msg);
            }
            Action::Links => {
                if self.profile.studio().is_empty() {
                    return Err("Укажите studio_url".into());
                }
                win::open(&format!("{}/#/links", self.profile.studio()), "")?;
            }
            Action::Strata => crate::strata::open(&self.profile)?,
            Action::Metric("cpu") => {
                let info = actions::temperature(&self.profile)?;
                self.say(&info);
            }
            Action::Metric("comfy") => {
                if let Some(url) = self.profile.comfy() {
                    win::open(&url, "")?
                }
            }
            Action::Metric("ram") => self.say(&format!(
                "Свободно {:.1} ГБ",
                (self.metrics[1] - self.metrics[0]) / 1073741824.
            )),
            Action::Metric(_) => {
                let url = s(&self.profile.cfg, "lm_url", "http://127.0.0.1:1234");
                win::message(&jobs::gpu_text(url), false);
            }
        }
        Ok(())
    }
    fn menu(&mut self) -> actions::Result {
        let mut options = vec![
            "Обновить даты (F5)",
            "Сортировка: дата / имя",
            "Тема",
            "Перенести на другой край",
            "Следующий монитор",
            "Показать скрытые сервисы",
            "Добавить папку…",
            "Открыть config.json",
            "Папка профиля",
            "Справка (F1)",
            "Скрыть",
            "Выход",
        ];
        if crate::strata::configured(&self.profile) {
            options.insert(9, "Настройки Strata и Qwen Code…");
        }
        match choose(&options).map(|index| options[index]) {
            Some("Настройки Strata и Qwen Code…") => {
                crate::strata::open(&self.profile)?
            }
            Some("Обновить даты (F5)") => self.scan(),
            Some("Сортировка: дата / имя") => {
                self.profile.state["sort"] = (if self.profile.state["sort"] == "date"
                    || self.profile.state["sort"].is_null()
                {
                    "name"
                } else {
                    "date"
                })
                .into();
                self.save();
            }
            Some("Тема") => {
                if let Some(i) = choose(&["Как в Windows", "Светлая", "Тёмная"]) {
                    self.profile.state["theme"] = ["auto", "light", "dark"][i].into();
                    self.save();
                }
            }
            Some("Перенести на другой край") => {
                let right = self.profile.state["edge"] != "right";
                self.profile.state["edge"] = (if right { "right" } else { "left" }).into();
                unsafe { win::ui_edge(right as i32) }
                self.save();
            }
            Some("Следующий монитор") => {
                let (x, y) = win::monitor();
                self.profile.state["monitor"] = json!([x, y]);
                self.save();
            }
            Some("Показать скрытые сервисы") => {
                for v in array(&mut self.profile.cfg, "services") {
                    v["hidden"] = false.into();
                }
                self.edited();
            }
            Some("Добавить папку…") => self.add(&win::pick(true)),
            Some("Открыть config.json") => {
                Command::new("notepad.exe")
                    .arg(&self.profile.config_path)
                    .spawn()
                    .map_err(|e| e.to_string())?;
            }
            Some("Папка профиля") => {
                win::open(&self.profile.dir.to_string_lossy(), "")?
            }
            Some("Справка (F1)") => help(),
            Some("Скрыть") => self.action(Action::Hide)?,
            Some("Выход") => self.closing = true,
            _ => {}
        }
        Ok(())
    }
    fn context(&mut self, a: Action) -> actions::Result {
        match a {
            Action::Folder { root, path } | Action::Expand { root, path } => {
                self.selected = path.clone();
                let is_root =
                    key(&expand(s(&self.profile.cfg["favorites"][root], "path", ""))) == key(&path);
                let mut opts = vec![
                    "Открыть в новом окне",
                    "Терминал здесь",
                    "Копировать путь",
                    "Значок папки",
                ];
                if is_root {
                    opts.extend([
                        "Переименовать",
                        "Выше",
                        "Ниже",
                        "Домашняя папка",
                        "Убрать из избранного",
                    ]);
                } else {
                    opts.push("Добавить в избранное");
                }
                if !self.profile.studio().is_empty() {
                    opts.push("Открыть в Студии");
                }
                let Some(i) = choose(&opts) else {
                    return Ok(());
                };
                match opts[i] {
                    "Открыть в новом окне" => {
                        let mut c = Command::new("explorer.exe");
                        c.arg(&path);
                        actions::run(c)?;
                    }
                    "Терминал здесь" => actions::terminal(&path)?,
                    "Копировать путь" => {
                        win::clipboard(Some(&path));
                    }
                    "Значок папки" => {
                        if let Some(i) = choose(&[
                            "Розовая",
                            "Оранжевая",
                            "Зелёная",
                            "Синяя",
                            "Фиолетовая",
                            "Из файла…",
                            "Стандартный",
                        ]) {
                            let icons = &mut self.profile.state["folder_icons"];
                            if !icons.is_object() {
                                *icons = json!({});
                            }
                            if i == 6 {
                                icons.as_object_mut().unwrap().remove(&key(&path));
                            } else if i == 5 {
                                let file = win::pick(false);
                                if let Some(p) = file.lines().next() {
                                    let spec = win::icon(p);
                                    if spec.is_empty() {
                                        return Err("Не удалось прочитать картинку".into());
                                    }
                                    icons[key(&path)] = spec.into();
                                }
                            } else {
                                icons[key(&path)] =
                                    ["pink", "orange", "green", "blue", "violet"][i].into();
                            }
                            self.save();
                        }
                    }
                    "Переименовать" => {
                        let n = win::input(
                            "Название папки",
                            s(&self.profile.cfg["favorites"][root], "name", name(&path)),
                        );
                        if !n.is_empty() {
                            self.profile.cfg["favorites"][root]["name"] = n.into();
                            self.edited();
                        }
                    }
                    "Выше" | "Ниже" => {
                        let a = array(&mut self.profile.cfg, "favorites");
                        let to = if opts[i] == "Выше" {
                            root.saturating_sub(1)
                        } else {
                            (root + 1).min(a.len() - 1)
                        };
                        a.swap(root, to);
                        self.edited();
                    }
                    "Домашняя папка" => {
                        let on = !flag(&self.profile.cfg["favorites"][root], "home", false);
                        for v in array(&mut self.profile.cfg, "favorites") {
                            v["home"] = false.into();
                        }
                        self.profile.cfg["favorites"][root]["home"] = on.into();
                        self.edited();
                    }
                    "Убрать из избранного" => {
                        array(&mut self.profile.cfg, "favorites").remove(root);
                        self.edited();
                    }
                    "Добавить в избранное" => self.add(&path),
                    _ => actions::studio(&self.profile, &path)?,
                }
            }
            Action::File(path) | Action::Reveal(path) => {
                match choose(&["Показать в папке", "Копировать путь", "Убрать с полки"])
                {
                    Some(0) => actions::reveal(&path)?,
                    Some(1) => {
                        win::clipboard(Some(&path));
                    }
                    Some(2) => {
                        array(&mut self.profile.state, "shelf_paths").retain(|v| v != &path);
                        self.save();
                    }
                    _ => {}
                }
            }
            Action::Shelf | Action::ShelfAdd => {
                match choose(&["Добавить файлы", "Очистить полку"]) {
                    Some(0) => self.action(Action::ShelfAdd)?,
                    Some(1) => {
                        self.profile.state["shelf_paths"] = json!([]);
                        self.save();
                    }
                    _ => {}
                }
            }
            Action::Service(i) => {
                let u = s(&self.profile.cfg["services"][i], "url", "").to_string();
                match choose(&["Открыть", "Копировать URL", "Скрыть сервис"])
                {
                    Some(0) => win::open(&u, "")?,
                    Some(1) => {
                        win::clipboard(Some(&u));
                    }
                    Some(2) => {
                        self.profile.cfg["services"][i]["hidden"] = true.into();
                        self.edited();
                    }
                    _ => {}
                }
            }
            Action::Metric("comfy") => {
                let Some(base) = self.profile.comfy() else {
                    return Ok(());
                };
                match choose(&[
                    "Открыть ComfyUI",
                    "Уведомления",
                    "Прервать текущую задачу…",
                    "Очистить ожидающие…",
                ]) {
                    Some(0) => win::open(&base, "")?,
                    Some(1) => {
                        if let Some(i) = choose(&["Выключены", "Длинная очередь", "Все очереди"])
                        {
                            self.profile.state["notify"] = ["off", "long", "all"][i].into();
                            self.save();
                            self.timer();
                        }
                    }
                    Some(2) if win::message("Прервать текущую задачу?", true) => {
                        jobs::queue_post(&base, "/interrupt", json!({}), self.tx.clone())
                    }
                    Some(3) if win::message("Очистить ожидающие задачи?", true) => {
                        jobs::queue_post(&base, "/queue", json!({"clear":true}), self.tx.clone())
                    }
                    _ => {}
                }
            }
            Action::Metric(m) => {
                let gpu = m == "gpu" || m == "vram";
                let free = if gpu { "free-vram" } else { "free-ram" };
                let mut opts = vec!["Показатели"];
                let button = list(&self.profile.cfg, "buttons")
                    .iter()
                    .position(|v| s(v, "cmd", "").contains(free));
                if button.is_some() {
                    opts.push("Освободить память");
                }
                if gpu {
                    opts.push("Выгрузить модели LM Studio");
                }
                if let Some(i) = choose(&opts) {
                    match opts[i] {
                        "Показатели" => self.action(Action::Metric(m))?,
                        "Освободить память" => {
                            self.action(Action::Button(button.unwrap()))?
                        }
                        _ => {
                            let mut c =
                                Command::new(expand(r"%USERPROFILE%\.lmstudio\bin\lms.exe"));
                            c.args(["unload", "--all"]);
                            actions::run(c)?;
                        }
                    }
                }
            }
            _ => self.menu()?,
        }
        Ok(())
    }
    pub fn event(&mut self, k: i32, row: i32, side: i32, data: String) -> actions::Result {
        let a = self
            .rows
            .get(row as usize)
            .map(|r| {
                if k == 1 && side == 2 && matches!(r.actions[0], Action::Shelf) {
                    Action::ShelfClear
                } else {
                    r.actions[usize::from(side == 1)].clone()
                }
            })
            .unwrap_or_default();
        match k {
            0 => self.closing = true,
            1 => self.action(a)?,
            2 => self.context(a)?,
            3 => {
                if let Action::Folder { path, .. } | Action::Expand { path, .. } = a {
                    let mut c = Command::new("explorer.exe");
                    c.arg(path);
                    actions::run(c)?;
                }
            }
            4 => match side {
                112 => help(),
                116 => self.scan(),
                27 => self.action(Action::Hide)?,
                46 => {
                    if let Action::File(p) = a {
                        array(&mut self.profile.state, "shelf_paths").retain(|v| v != &p);
                        self.save();
                    }
                }
                _ => {}
            },
            5 => self.tick(),
            14 => crate::strata::load_model(&self.profile, side != 0, row != 0),
            15 => crate::strata::submit(&mut self.profile, side),
            16 => {
                self.profile.state["strata_window_position"] = json!([row, side]);
                self.save();
            }
            13 if !self.tip_busy => {
                self.tip_busy = true;
                let tx = self.tx.clone();
                let url = s(&self.profile.cfg, "lm_url", "http://127.0.0.1:1234").to_owned();
                std::thread::spawn(move || jobs::send(&tx, Update::GpuTip(jobs::gpu_text(&url))));
            }
            6 => {
                self.visible = side != 0;
                self.timer();
                if self.visible {
                    self.tick();
                }
            }
            7 => self.add_file(&data),
            8 => {
                let m = bookmarks::save(&actions::bookmark_db(&self.profile), &data)?;
                self.say(&m);
            }
            9 => self.updates(),
            11 => {
                if self.profile.reload().map_err(|e| e.to_string())? {
                    self.dirs.clear();
                    if !self.preview {
                        win::hotkey(s(&self.profile.cfg, "hotkey", "Win+Shift+F"));
                    }
                    self.scan();
                } else if self
                    .last_scan
                    .is_none_or(|t| t.elapsed() > Duration::from_secs(15))
                {
                    self.scan();
                } else {
                    return Ok(());
                }
            }
            12 => {
                self.profile.state["width"] = side.into();
                self.save();
            }
            _ => {}
        }
        if self.visible && !self.closing {
            self.render()
        }
        Ok(())
    }
}
fn button_glyph(b: &J) -> i32 {
    match s(b, "icon", "play") {
        "terminal" => 0xe756,
        "code" => 0xe943,
        "moon" => 0xe708,
        "power" => 0xe7e8,
        "delete" => 0xe74d,
        "bolt" => 0xe945,
        "monitor" => 0xe7f4,
        "folder" => 0xe8b7,
        "link" => 0xe71b,
        "settings" => 0xe713,
        "sync" => 0xe895,
        "video" => 0xe714,
        _ => 0xe768,
    }
}
fn choose(labels: &[&str]) -> Option<usize> {
    win::menu(&labels.iter().map(|s| s.to_string()).collect::<Vec<_>>())
}

fn help() {
    win::message("FavBar\n\nНажмите значок или название папки, чтобы открыть её в Проводнике. Клик по остальной строке раскрывает или сворачивает ветку. Папку без видимых подпапок можно открыть по всей строке. Средняя кнопка открывает новое окно. Правый клик показывает действия.\n\nПеретащите файлы на полку или с полки в другое окно. Ссылку можно перетащить на строку закладок или вставить из буфера.\n\nСтрелки клавиатуры выбирают строку. Enter нажимает слева, Shift+Enter справа. F5 обновляет даты. Потяните внутренний край панели, чтобы изменить ширину.",false);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn folder_actions_follow_visible_children() {
        let temp =
            std::env::temp_dir().join(format!("favbar-folder-actions-{}", std::process::id()));
        let root = temp.join("root");
        let leaf = root.join("leaf");
        fs::create_dir_all(leaf.join(".hidden")).unwrap();
        fs::create_dir(leaf.join("ignored")).unwrap();
        fs::write(leaf.join("file.txt"), b"").unwrap();
        let mut profile = Profile::load(temp.clone(), None).unwrap();
        profile.cfg =
            json!({"favorites":[{"path":root}], "skip":["ignored"], "hide_dot_folders":true});
        let mut app = App::new(profile, true);
        let root = root.to_str().unwrap();
        app.folder(0, root.into(), "root".into(), 0);
        assert!(matches!(
            app.rows[0].actions,
            [Action::Folder { .. }, Action::Expand { .. }]
        ));
        app.profile
            .toggle("expanded", &format!("{}|{}", key(root), key(root)));
        app.rows.clear();
        app.folder(0, root.into(), "root".into(), 0);
        assert_eq!(app.rows.len(), 2);
        assert!(
            matches!(&app.rows[1].actions, [Action::Folder {path:a,..}, Action::Folder {path:b,..}] if a == b && Path::new(a) == leaf)
        );
        assert!(!app.rows[1].right.contains('▸'));
        fs::remove_dir_all(temp).unwrap();
    }
}
