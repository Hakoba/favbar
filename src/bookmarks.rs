use crate::win::wide;
use std::{
    ffi::{c_void, CString},
    path::Path,
    ptr,
};
use url::Url;
#[link(name = "winsqlite3")]
extern "C" {
    fn sqlite3_open16(name: *const u16, db: *mut *mut c_void) -> i32;
    fn sqlite3_close(db: *mut c_void) -> i32;
    fn sqlite3_busy_timeout(db: *mut c_void, ms: i32) -> i32;
    fn sqlite3_exec(db: *mut c_void, sql: *const i8, cb: usize, data: usize, err: usize) -> i32;
    fn sqlite3_prepare_v2(
        db: *mut c_void,
        sql: *const i8,
        n: i32,
        stmt: *mut *mut c_void,
        end: usize,
    ) -> i32;
    fn sqlite3_bind_text(st: *mut c_void, i: i32, s: *const i8, n: i32, destructor: usize) -> i32;
    fn sqlite3_step(st: *mut c_void) -> i32;
    fn sqlite3_finalize(st: *mut c_void) -> i32;
    fn sqlite3_changes(db: *mut c_void) -> i32;
}
fn parts(raw: &str) -> Result<[String; 4], String> {
    let raw = raw.trim();
    if raw.len() > 8192 || raw.contains(|c: char| c.is_whitespace() || c.is_control() || c == '\\')
    {
        return Err("Нужна веб-ссылка".into());
    }
    let explicit = raw.contains("://");
    let full = if explicit {
        raw.into()
    } else {
        format!("https://{raw}")
    };
    let url = Url::parse(&full).map_err(|_| "Нужна веб-ссылка")?;
    let host = url.host_str().ok_or("Нужна веб-ссылка")?;
    if !matches!(url.scheme(), "http" | "https")
        || !url.username().is_empty()
        || url.password().is_some()
        || (!explicit && !host.contains('.') && host.parse::<std::net::IpAddr>().is_err())
    {
        return Err("Нужна ссылка HTTP(S) без учётных данных".into());
    }
    let h = host.strip_prefix("www.").unwrap_or(host);
    let (source, alias) = match h {
        "civitai.com" | "civitai.red" => ("Civitai", "civitai.com"),
        "old.reddit.com" | "np.reddit.com" | "new.reddit.com" | "m.reddit.com" | "reddit.com" => {
            ("Reddit", "reddit.com")
        }
        "redd.it" | "i.redd.it" | "v.redd.it" => ("Reddit", h),
        "t.me" | "telegram.me" | "telegram.dog" => ("Telegram", h),
        _ => ("Другие", host),
    };
    let mut key = url.clone();
    if source != "Другие" {
        key.set_host(Some(alias)).map_err(|_| "Некорректный сайт")?;
        let pairs: Vec<_> = url
            .query_pairs()
            .filter(|(k, _)| !k.to_lowercase().starts_with("utm_"))
            .collect();
        key.set_query(None);
        if !pairs.is_empty() {
            key.query_pairs_mut().extend_pairs(pairs);
        }
        if source == "Civitai" {
            let seg: Vec<_> = url.path().trim_matches('/').split('/').collect();
            if (2..=3).contains(&seg.len())
                && seg[0] == "models"
                && !seg[1].is_empty()
                && seg[1].bytes().all(|b| b.is_ascii_digit())
            {
                key.set_path(&format!("/models/{}", seg[1]));
            }
        }
    }
    let slug = url
        .path()
        .trim_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or("");
    let title = if slug.is_empty() {
        host.into()
    } else {
        slug.replace(['-', '_'], " ")
    };
    Ok([key.into(), url.into(), source.into(), title])
}
pub fn save(db_path: &Path, raw: &str) -> Result<String, String> {
    let values = parts(raw)?;
    if let Some(parent) = db_path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?
    }
    let strings: Vec<_> = values
        .iter()
        .map(|s| CString::new(s.as_str()).map_err(|e| e.to_string()))
        .collect::<Result<_, _>>()?;
    let mut db = ptr::null_mut();
    let mut stmt = ptr::null_mut();
    unsafe {
        let open = sqlite3_open16(wide(&db_path.to_string_lossy()).as_ptr(), &mut db);
        let result = (|| {
            if open != 0 {
                return Err("Не удалось открыть базу закладок".into());
            }
            sqlite3_busy_timeout(db, 3000);
            let schema=c"CREATE TABLE IF NOT EXISTS bookmarks(id INTEGER PRIMARY KEY,link_key TEXT UNIQUE NOT NULL,url TEXT NOT NULL,source TEXT NOT NULL,title TEXT NOT NULL,saved TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)";
            if sqlite3_exec(db, schema.as_ptr(), 0, 0, 0) != 0 {
                return Err("Ошибка таблицы закладок".into());
            }
            let insert=c"INSERT INTO bookmarks(link_key,url,source,title) VALUES(?,?,?,?) ON CONFLICT(link_key) DO NOTHING";
            if sqlite3_prepare_v2(db, insert.as_ptr(), -1, &mut stmt, 0) != 0 {
                return Err("Ошибка запроса закладок".into());
            }
            for (i, v) in strings.iter().enumerate() {
                if sqlite3_bind_text(stmt, i as i32 + 1, v.as_ptr(), -1, 0) != 0 {
                    return Err("Ошибка ссылки".into());
                }
            }
            if sqlite3_step(stmt) != 101 {
                return Err("Не удалось сохранить ссылку".into());
            }
            Ok(format!(
                "{} · {}",
                if sqlite3_changes(db) == 0 {
                    "Уже в закладках"
                } else {
                    "Сохранено"
                },
                values[3]
            ))
        })();
        if !stmt.is_null() {
            sqlite3_finalize(stmt);
        }
        if !db.is_null() {
            sqlite3_close(db);
        }
        result
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn keys() {
        assert_eq!(
            parts("https://old.reddit.com/r/rust/?utm_source=x").unwrap()[0],
            "https://reddit.com/r/rust/"
        );
        assert!(parts("file:///C:/Windows").is_err());
        assert!(parts("https://user:pass@example.org").is_err());
        assert_eq!(
            parts("https://example.com/a/?sig=1").unwrap()[0],
            "https://example.com/a/?sig=1"
        );
    }
    #[test]
    fn compatible_sqlite_collection_deduplicates_across_calls() {
        let path =
            std::env::temp_dir().join(format!("favbar-bookmarks-{}.sqlite", std::process::id()));
        assert!(save(
            &path,
            "https://civitai.red/models/41/example?utm_medium=test"
        )
        .unwrap()
        .starts_with("Сохранено"));
        assert!(save(&path, "https://civitai.com/models/41")
            .unwrap()
            .starts_with("Уже в закладках"));
        std::fs::remove_file(path).unwrap();
    }
}
