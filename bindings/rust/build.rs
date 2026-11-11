use std::env;
use std::fs;
use std::path::Path;

fn library(path: &Path) -> Option<(String, String)> {
    let directory = path.parent()?.to_str()?.to_owned();
    let file = path.file_name()?.to_str()?;
    let stem = file.strip_prefix("lib").unwrap_or(file);
    if let Some(name) = stem.strip_suffix(".a") {
        return Some((directory, format!("static={name}")));
    }
    for suffix in [".dylib", ".tbd", ".so"] {
        if let Some(name) = stem.strip_suffix(suffix) {
            return Some((directory, format!("dylib={name}")));
        }
    }
    if file.contains(".so.") {
        return Some((directory, format!("dylib:+verbatim={file}")));
    }
    None
}

fn main() {
    println!("cargo::rerun-if-env-changed=LOKIVERSE_LINK_ARGS");
    let file = env::var("LOKIVERSE_LINK_ARGS")
        .expect("set LOKIVERSE_LINK_ARGS to the lokiverse_c.link file written by the CMake build");
    println!("cargo::rerun-if-changed={file}");
    let content = fs::read_to_string(&file)
        .unwrap_or_else(|error| panic!("cannot read LOKIVERSE_LINK_ARGS file {file}: {error}"));
    let mut framework = false;
    for argument in content
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
    {
        if framework {
            framework = false;
            println!("cargo::rustc-link-lib=framework={argument}");
        } else if argument == "-framework" {
            framework = true;
        } else if let Some(name) = argument.strip_prefix("-Wl,-framework,") {
            println!("cargo::rustc-link-lib=framework={name}");
        } else if let Some(directory) = argument.strip_prefix("-L") {
            println!("cargo::rustc-link-search=native={directory}");
        } else if let Some(directory) = argument.strip_prefix("-F") {
            println!("cargo::rustc-link-search=framework={directory}");
        } else if let Some(name) = argument.strip_prefix("-l") {
            println!("cargo::rustc-link-lib=dylib={name}");
        } else if argument.starts_with('-') {
            println!("cargo::rustc-link-arg={argument}");
        } else if let Some((directory, name)) = library(Path::new(argument)) {
            println!("cargo::rustc-link-search=native={directory}");
            println!("cargo::rustc-link-lib={name}");
        } else {
            println!("cargo::rustc-link-arg={argument}");
        }
    }
}
