use std::ffi::CString;
use std::path::Path;

extern "C" {
    fn ui_single(test: i32) -> i32;
    fn ui_start(test: i32, width: i32, right: i32, x: i32, y: i32) -> i32;
    fn ui_next(row: *mut i32, side: *mut i32, data: *mut *const u16) -> i32;
    fn ui_key(mods: u32, key: u32) -> i32;
    fn ui_row(
        l: *const u16,
        r: *const u16,
        h: *const u16,
        path: *const u16,
        style: i32,
        depth: i32,
        left_detail: i32,
        right_detail: i32,
    );
    fn ui_finish(theme: i32);
    fn ui_menu(items: *const MenuItem, n: i32) -> i32;
    fn ui_input(title: *const u16, initial: *const u16) -> *const u16;
    pub fn ui_shutdown_timer();
    pub fn ui_queue_open();
    pub fn ui_queue_visible() -> i32;
    fn ui_queue_begin();
    fn ui_queue_item(id: *const u16, title: *const u16, running: i32);
    fn ui_queue_finish(online: i32);
    fn ui_queue_notice(text: *const u16);
    pub fn ui_search_begin();
    fn ui_search_item(name: *const u16, path: *const u16);
    pub fn ui_search_show();
    pub fn ui_search_close();
    fn ui_preview_file(path: *const u16);
    fn ui_preview_matches(path: *const u16) -> i32;
    fn preview_load(path: *const u16) -> *mut std::ffi::c_void;
    fn preview_free(frame: *mut std::ffi::c_void);
    fn ui_preview_apply(frame: *mut std::ffi::c_void);
    fn ui_pick(folder: i32) -> *const u16;
    fn ui_clip(value: *const u16, write: i32) -> *const u16;
    fn ui_explorer(dest: *const u16) -> *const u16;
    fn ui_open(path: *const u16, cwd: *const u16) -> i32;
    fn ui_msg(s: *const u16, ask: i32) -> i32;
    fn ui_status(s: *const u16);
    fn ui_tip(s: *const u16);
    fn ui_monitor(x: *mut i32, y: *mut i32);
    fn ui_date(s: u64) -> *const u16;
    fn ui_icon(file: *const u16) -> *const u16;
    fn gpu_processes() -> *const u16;
    fn http_request(
        url: *const u16,
        data: *const i8,
        out: *mut std::ffi::c_void,
        append: unsafe extern "C" fn(*mut std::ffi::c_void, *const u8, usize) -> i32,
    ) -> i32;
    fn replace_file(from: *const u16, to: *const u16) -> i32;
    pub fn metrics(values: *mut f64) -> i32;
    pub fn process_birth(pid: u32) -> i64;
    pub fn ui_show(yes: i32);
    pub fn ui_timer(ms: i32);
    pub fn ui_edge(right: i32);
    pub fn ui_clear();
    pub fn ui_close();
    pub fn ui_wake();
}
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain([0]).collect()
}
pub(crate) unsafe fn copy(p: *const u16) -> String {
    if p.is_null() {
        return String::new();
    }
    let mut n = 0;
    while *p.add(n) != 0 {
        n += 1;
    }
    String::from_utf16_lossy(std::slice::from_raw_parts(p, n))
}
pub fn start(preview: bool, width: i32, right: bool, monitor: (i32, i32)) -> bool {
    unsafe {
        ui_single(preview as i32) != 0
            && ui_start(preview as i32, width, right as i32, monitor.0, monitor.1) != 0
    }
}
pub fn next() -> (i32, i32, i32, String) {
    let (mut row, mut side, mut p) = (-1, 0, std::ptr::null());
    unsafe {
        let k = ui_next(&mut row, &mut side, &mut p);
        (k, row, side, copy(p))
    }
}
pub fn hotkey(s: &str) -> bool {
    let (mut mods, mut key) = (0, 0);
    for part in s.to_ascii_uppercase().split('+').map(str::trim) {
        match part {
            "WIN" => mods |= 8,
            "SHIFT" => mods |= 4,
            "CTRL" | "CONTROL" => mods |= 2,
            "ALT" => mods |= 1,
            s if s.len() == 1 && s.as_bytes()[0].is_ascii_alphanumeric() => {
                key = s.as_bytes()[0] as u32
            }
            s => {
                let Some(n) = s
                    .strip_prefix('F')
                    .and_then(|n| n.parse::<u32>().ok())
                    .filter(|n| (1..=24).contains(n))
                else {
                    return false;
                };
                key = 0x6f + n;
            }
        }
    }
    unsafe { ui_key(mods, key) != 0 }
}
pub fn row(l: &str, r: &str, h: &str, p: &str, style: i32, depth: i32, details: [i32; 2]) {
    unsafe {
        ui_row(
            wide(l).as_ptr(),
            wide(r).as_ptr(),
            wide(h).as_ptr(),
            wide(p).as_ptr(),
            style,
            depth,
            details[0],
            details[1],
        )
    }
}
pub fn finish(theme: &str) {
    unsafe {
        ui_finish(match theme {
            "dark" => 2,
            "light" => 1,
            _ => 0,
        })
    }
}
#[repr(C)]
struct MenuItem {
    label: *const u16,
    glyph: i32,
    group_start: i32,
    destructive: i32,
}
pub fn menu(names: &[String]) -> Option<usize> {
    let values: Vec<_> = names.iter().map(|s| wide(s)).collect();
    let items: Vec<_> = names
        .iter()
        .zip(&values)
        .map(|(name, label)| {
            let (glyph, group_start, destructive) = match name.as_str() {
                "Открыть в новом окне" | "Открыть" | "Открыть ComfyUI" | "Открыть в Студии" => {
                    (0xe8a7, false, false)
                }
                "Терминал здесь" => (0xe756, false, false),
                "Копировать путь" | "Копировать URL" => {
                    (0xe8c8, false, false)
                }
                "Значок папки" | "Тема" => (0xe790, true, false),
                "Переименовать" => (0xe8ac, false, false),
                "Выше" => (0xe74a, false, false),
                "Ниже" => (0xe74b, false, false),
                "Домашняя папка" => (0xe80f, false, false),
                "Добавить в избранное" => (0xe734, false, false),
                "Убрать из избранного"
                | "Очистить полку"
                | "Убрать с полки"
                | "Очистить ожидающие…" => (0xe74d, true, true),
                "Прервать текущую задачу…" => (0xe71a, true, true),
                "Показать в папке" | "Папка профиля" => {
                    (0xe8b7, false, false)
                }
                "Добавить файлы" | "Добавить папку…" => {
                    (0xe710, false, false)
                }
                "Скрыть" | "Скрыть сервис" => (0xed1a, false, false),
                "Уведомления" => (0xea8f, false, false),
                "Показатели" | "Освободить память" | "Выгрузить модели LM Studio" => {
                    (0xe9d9, false, false)
                }
                "Обновить даты (F5)" => (0xe72c, false, false),
                "Быстрый переход…  Ctrl+K" => (0xe721, false, false),
                "Предпросмотр  Space" => (0xe890, false, false),
                "Сортировка: дата / имя" => (0xe8cb, false, false),
                "Следующий монитор" | "Перенести на другой край" => {
                    (0xe7f4, false, false)
                }
                "Показать скрытые сервисы" => (0xe890, false, false),
                "Открыть config.json" => (0xe713, true, false),
                "Настройки Strata и Qwen Code…" => (0xe713, false, false),
                "Справка (F1)" => (0xe897, false, false),
                "Выход" => (0xe8bb, true, false),
                _ => (0, false, false),
            };
            MenuItem {
                label: label.as_ptr(),
                glyph,
                group_start: group_start.into(),
                destructive: destructive.into(),
            }
        })
        .collect();
    usize::try_from(unsafe { ui_menu(items.as_ptr(), items.len() as i32) }).ok()
}
pub fn input(title: &str, initial: &str) -> String {
    unsafe { copy(ui_input(wide(title).as_ptr(), wide(initial).as_ptr())) }
}
pub fn pick(folder: bool) -> String {
    unsafe { copy(ui_pick(folder as i32)) }
}
pub fn clipboard(s: Option<&str>) -> String {
    unsafe { copy(ui_clip(wide(s.unwrap_or("")).as_ptr(), s.is_some() as i32)) }
}
pub fn explorer(p: &str) -> String {
    unsafe { copy(ui_explorer(wide(p).as_ptr())) }
}
pub fn open(p: &str, cwd: &str) -> Result<(), String> {
    if unsafe { ui_open(wide(p).as_ptr(), wide(cwd).as_ptr()) } != 0 {
        Ok(())
    } else {
        Err(format!("Не удалось открыть {p}"))
    }
}
pub fn message(s: &str, ask: bool) -> bool {
    unsafe { ui_msg(wide(s).as_ptr(), ask as i32) != 0 }
}
pub fn status(s: &str) {
    unsafe { ui_status(wide(s).as_ptr()) }
}
pub fn tip(s: &str) {
    unsafe { ui_tip(wide(s).as_ptr()) }
}
pub fn queue_rows(snapshot: Option<&crate::jobs::QueueSnapshot>) {
    if snapshot.is_some_and(|v| !v.detailed) {
        return;
    }
    unsafe {
        ui_queue_begin();
    }
    if let Some(q) = snapshot {
        for e in &q.entries {
            unsafe {
                ui_queue_item(
                    wide(&e.id).as_ptr(),
                    wide(&e.title).as_ptr(),
                    e.running.into(),
                );
            }
        }
    }
    unsafe {
        ui_queue_finish(snapshot.is_some().into());
    }
}
pub fn queue_notice(s: &str) {
    unsafe { ui_queue_notice(wide(s).as_ptr()) }
}
pub fn search_item(name: &str, path: &str) {
    unsafe { ui_search_item(wide(name).as_ptr(), wide(path).as_ptr()) }
}
pub fn preview_file(path: &str) {
    unsafe { ui_preview_file(wide(path).as_ptr()) }
}
pub fn preview_matches(path: &str) -> bool {
    unsafe { ui_preview_matches(wide(path).as_ptr()) != 0 }
}
pub struct PreviewFrame(*mut std::ffi::c_void);
// The decoder returns exclusively owned pixel bytes and text. COM interfaces stay on the decoder thread.
unsafe impl Send for PreviewFrame {}
impl PreviewFrame {
    pub fn load(path: &str) -> Self {
        Self(unsafe { preview_load(wide(path).as_ptr()) })
    }
    pub fn show(self) {
        unsafe {
            ui_preview_apply(self.0);
        }
    }
}
impl Drop for PreviewFrame {
    fn drop(&mut self) {
        unsafe {
            preview_free(self.0);
        }
    }
}
pub fn monitor() -> (i32, i32) {
    let (mut x, mut y) = (0, 0);
    unsafe { ui_monitor(&mut x, &mut y) };
    (x, y)
}
pub fn date(n: u64) -> String {
    unsafe { copy(ui_date(n)) }
}
pub fn icon(file: &str) -> String {
    unsafe { copy(ui_icon(wide(file).as_ptr())) }
}
pub fn processes() -> String {
    unsafe { copy(gpu_processes()) }
}
pub fn http(url: &str, body: Option<&str>) -> Option<String> {
    let data = body.map(CString::new).transpose().ok()?;
    unsafe extern "C" fn append(out: *mut std::ffi::c_void, chunk: *const u8, len: usize) -> i32 {
        let buf = &mut *out.cast::<Vec<u8>>();
        if buf.len().saturating_add(len) > 64 * 1024 * 1024 {
            return 0;
        }
        buf.extend_from_slice(std::slice::from_raw_parts(chunk, len));
        1
    }
    let mut buf: Vec<u8> = Vec::new();
    let n = unsafe {
        http_request(
            wide(url).as_ptr(),
            data.as_ref().map_or(std::ptr::null(), |v| v.as_ptr()),
            std::ptr::from_mut(&mut buf).cast(),
            append,
        )
    };
    if n <= 0 {
        return None;
    }
    String::from_utf8(buf).ok()
}
pub fn replace(a: &Path, b: &Path) -> std::io::Result<()> {
    if unsafe {
        replace_file(
            wide(&a.to_string_lossy()).as_ptr(),
            wide(&b.to_string_lossy()).as_ptr(),
        )
    } == 0
    {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}
