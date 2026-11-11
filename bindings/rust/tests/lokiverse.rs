mod wire;

use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpListener;
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use lokiverse::{Error, HttpTransport, Logger, Message, SafeLogger, make_http, make_safe};

fn http_logger(host: &str, port: u16) -> Result<SafeLogger<HttpTransport>, Error> {
    make_safe(make_http(host, port)?)
}

struct Request {
    path: String,
    content_type: String,
    body: Vec<u8>,
}

fn receiver(status: u16) -> (u16, Receiver<Request>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let (sender, requests) = mpsc::channel();
    thread::spawn(move || {
        for connection in listener.incoming() {
            let Ok(connection) = connection else { break };
            let sender = sender.clone();
            thread::spawn(move || {
                let mut reader = BufReader::new(connection);
                let mut line = String::new();
                if reader.read_line(&mut line).unwrap_or(0) == 0 {
                    return;
                }
                let path = line.split(' ').nth(1).unwrap_or("").to_owned();
                let mut content_type = String::new();
                let mut length = 0;
                loop {
                    line.clear();
                    reader.read_line(&mut line).unwrap();
                    let header = line.trim_end();
                    if header.is_empty() {
                        break;
                    }
                    let (name, value) = header.split_once(':').unwrap();
                    match name.to_ascii_lowercase().as_str() {
                        "content-type" => content_type = value.trim().to_owned(),
                        "content-length" => length = value.trim().parse().unwrap(),
                        _ => {}
                    }
                }
                let mut body = vec![0; length];
                reader.read_exact(&mut body).unwrap();
                let _ = sender.send(Request {
                    path,
                    content_type,
                    body,
                });
                let response = format!(
                    "HTTP/1.1 {status} Status\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                );
                let _ = reader.get_mut().write_all(response.as_bytes());
            });
        }
    });
    (port, requests)
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos() as u64
}

fn sample() -> Message {
    let mut message = Message::new();
    message
        .stream()
        .label("job", "rust")
        .entry(1, "hello")
        .commit()
        .unwrap();
    message
}

fn decoded(message: &Message) -> Vec<wire::Stream> {
    wire::decode(&message.encode().expect("message has no body"))
}

#[test]
fn encodes_multiple_streams() {
    let mut message = Message::new();
    message
        .stream()
        .label("job", "api")
        .label("env", "test")
        .entry(1700000000123456789, "first")
        .entry(1700000000123456790, "second")
        .commit()
        .unwrap();
    message
        .stream()
        .label("job", "worker")
        .entry(1700000001123456789, "third")
        .commit()
        .unwrap();
    let streams = decoded(&message);
    assert_eq!(streams.len(), 2);
    assert_eq!(streams[0].labels, r#"{env="test",job="api"}"#);
    assert_eq!(
        streams[0].values(),
        [
            (1700000000123456789, b"first".as_slice()),
            (1700000000123456790, b"second".as_slice()),
        ]
    );
    assert_eq!(streams[1].labels, r#"{job="worker"}"#);
    assert_eq!(
        streams[1].values(),
        [(1700000001123456789, b"third".as_slice())]
    );
}

#[test]
fn timestamp_boundaries_and_empty_lines() {
    let timestamps = [
        0,
        1,
        999_999_999,
        1_000_000_000,
        1700000000123456789,
        u64::MAX,
    ];
    let mut message = Message::new();
    let mut stream = message.stream();
    for timestamp in timestamps {
        stream.entry(timestamp, "");
    }
    stream.commit().unwrap();
    drop(stream);
    let streams = decoded(&message);
    assert_eq!(streams[0].labels, "{}");
    assert_eq!(streams[0].entries.len(), timestamps.len());
    for (entry, timestamp) in streams[0].entries.iter().zip(timestamps) {
        assert!(entry.has_timestamp);
        assert!(entry.nanos < 1_000_000_000);
        assert_eq!(entry.timestamp, timestamp);
        assert!(entry.line.is_empty());
    }
}

#[test]
fn unicode_nulls_and_escaped_labels() {
    let line = "say \"hi\"\\\nnext\ttab\0fin: café 🐍";
    let mut message = Message::new();
    message
        .stream()
        .label("clé\0", "quote\"\\\n\0café 🐍")
        .entry(1, line)
        .commit()
        .unwrap();
    let streams = decoded(&message);
    assert_eq!(
        streams[0].labels,
        r#"{"clé\u0000"="quote\"\\\u000a\u0000café 🐍"}"#
    );
    assert_eq!(streams[0].values(), [(1, line.as_bytes())]);
}

#[test]
fn entry_now_uses_wall_clock() {
    let mut message = Message::new();
    let before = now();
    let mut stream = message.stream();
    stream.entry_now("now");
    let after = now();
    thread::sleep(Duration::from_millis(2));
    stream.commit().unwrap();
    drop(stream);
    let streams = decoded(&message);
    let entry = &streams[0].entries[0];
    assert!((before..=after).contains(&entry.timestamp));
    assert_eq!(entry.line, b"now");
}

#[test]
fn commit_requires_entries_and_builder_is_reusable() {
    let mut message = Message::new();
    assert_eq!(
        message.stream().label("job", "rust").commit(),
        Err(Error::NoEntries)
    );
    assert!(message.encode().is_none());
    message
        .stream()
        .label("job", "discarded")
        .entry(1, "discarded");
    assert!(message.encode().is_none());
    let mut stream = message.stream();
    stream.label("job", "rust");
    assert_eq!(stream.commit(), Err(Error::NoEntries));
    stream
        .entry(2, "second")
        .entry(1, "first")
        .commit()
        .unwrap();
    assert_eq!(stream.commit(), Err(Error::NoEntries));
    stream
        .label("job", "rust")
        .entry(1, "separate")
        .commit()
        .unwrap();
    drop(stream);
    let streams = decoded(&message);
    assert_eq!(streams.len(), 2);
    assert_eq!(streams[0].labels, r#"{job="rust"}"#);
    assert_eq!(streams[1].labels, r#"{job="rust"}"#);
    assert_eq!(
        streams[0].values(),
        [(2, b"second".as_slice()), (1, b"first".as_slice())]
    );
    assert_eq!(streams[1].values(), [(1, b"separate".as_slice())]);
}

#[test]
fn push_posts_encoded_body() {
    let (port, requests) = receiver(204);
    let logger = http_logger("127.0.0.1", port).unwrap();
    let message = sample();
    let body = message.encode().unwrap();
    for _ in 0..2 {
        assert!(logger.push(&message));
        let request = requests.recv_timeout(Duration::from_secs(3)).unwrap();
        assert_eq!(request.path, "/loki/api/v1/push");
        assert_eq!(request.content_type, "application/x-protobuf");
        assert_eq!(request.body, body);
    }
}

#[test]
fn result_follows_status() {
    for status in [200, 204, 400, 500] {
        let (port, _requests) = receiver(status);
        let logger = http_logger("127.0.0.1", port).unwrap();
        assert_eq!(logger.push(&sample()), status == 204, "status {status}");
    }
}

#[test]
fn empty_message_does_not_post() {
    let (port, requests) = receiver(204);
    let message = Message::default();
    assert!(message.encode().is_none());
    assert!(!http_logger("127.0.0.1", port).unwrap().push(&message));
    assert!(requests.recv_timeout(Duration::from_millis(100)).is_err());
}

#[test]
fn connection_failure_returns_false() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    drop(listener);
    assert!(!http_logger("127.0.0.1", port).unwrap().push(&sample()));
}

#[test]
fn logger_arguments() {
    for host in ["", "local\0host"] {
        assert_eq!(http_logger(host, 3100).err(), Some(Error::InvalidArgument));
    }
    assert_eq!(
        http_logger("localhost", 0).err(),
        Some(Error::InvalidArgument)
    );
    for port in [1, 65535] {
        assert!(http_logger("localhost", port).is_ok());
    }
    assert_eq!(Error::NoEntries.to_string(), "stream has no entries");
}

#[test]
fn concurrent_pushes() {
    let (port, requests) = receiver(204);
    let logger = http_logger("127.0.0.1", port).unwrap();
    let message = sample();
    let body = message.encode().unwrap();
    let results: Vec<bool> = thread::scope(|scope| {
        let workers: Vec<_> = (0..16)
            .map(|_| scope.spawn(|| logger.push(&message)))
            .collect();
        workers
            .into_iter()
            .map(|worker| worker.join().unwrap())
            .collect()
    });
    assert_eq!(results, [true; 16]);
    for _ in 0..16 {
        let request = requests.recv_timeout(Duration::from_secs(3)).unwrap();
        assert_eq!(request.body, body);
    }
}

#[test]
fn handles_move_across_threads() {
    let (port, requests) = receiver(204);
    let logger = http_logger("127.0.0.1", port).unwrap();
    let mut message = Message::new();
    let worker = thread::spawn(move || {
        message.stream().entry(1, "moved").commit().unwrap();
        logger.push(&message)
    });
    assert!(worker.join().unwrap());
    assert!(requests.recv_timeout(Duration::from_secs(3)).is_ok());
}

#[test]
fn large_payload() {
    let big = "é".repeat(512 * 1024);
    let mut message = Message::new();
    message.stream().entry(1, &big).commit().unwrap();
    let mut stream = message.stream();
    stream.label("job", "many");
    for index in 0..10_000 {
        stream.entry(index, "line");
    }
    stream.commit().unwrap();
    drop(stream);
    let streams = decoded(&message);
    assert_eq!(streams[0].values(), [(1, big.as_bytes())]);
    assert_eq!(streams[1].entries.len(), 10_000);
    assert_eq!(streams[1].entries[9_999].timestamp, 9_999);
}

#[test]
fn reservation_preserves_values_and_reports_overflow() {
    let mut message = Message::new();
    let mut stream = message.stream();
    stream
        .reserve(0, 0)
        .reserve(1, 1)
        .label("job", "kept")
        .entry(1, "first");
    stream.reserve(8, 256).reserve(0, 0);
    for i in 0..300 {
        stream.entry(i, "later");
    }
    stream.commit().unwrap();
    stream.reserve(1, 1).entry(2, "reused").commit().unwrap();
    drop(stream);
    assert_eq!(decoded(&message)[0].entries.len(), 301);
    assert_eq!(decoded(&message)[0].entries[0].line, b"first");
    let mut stream = message.stream();
    stream.entry(1, "kept").reserve(0, usize::MAX);
    assert_eq!(stream.commit(), Err(Error::InvalidArgument));
    drop(stream);
    assert_eq!(decoded(&message).len(), 2);
    let before = now();
    let value = lokiverse::Value::now("a\0b");
    assert!(value.timestamp_ns >= before && value.timestamp_ns <= now());
    assert_eq!(value.line, "a\0b");
}

struct RecordingTransport {
    body: std::sync::Arc<std::sync::Mutex<Vec<u8>>>,
    dropped: std::sync::Arc<std::sync::atomic::AtomicUsize>,
    calls: std::cell::Cell<usize>,
    panic: bool,
}

impl lokiverse::Transport for RecordingTransport {
    fn post(&mut self, path: &str, body: &[u8], content_type: &str) -> i32 {
        assert!(!self.panic, "transport panic");
        assert_eq!(path, "/loki/api/v1/push");
        assert_eq!(content_type, "application/x-protobuf");
        self.calls.set(self.calls.get() + 1);
        thread::sleep(Duration::from_millis(1));
        *self.body.lock().unwrap() = body.to_vec();
        204
    }
}

impl Drop for RecordingTransport {
    fn drop(&mut self) {
        self.dropped
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }
}

#[test]
fn custom_transport_safe_concurrency_and_cleanup() {
    use std::sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    };
    let body = Arc::new(Mutex::new(Vec::new()));
    let dropped = Arc::new(AtomicUsize::new(0));
    let transport = RecordingTransport {
        body: body.clone(),
        dropped: dropped.clone(),
        calls: std::cell::Cell::new(0),
        panic: false,
    };
    let logger = make_safe(transport).unwrap();
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<SafeLogger<RecordingTransport>>();
    assert!(!logger.push(&Message::new()));
    let message = sample();
    thread::scope(|scope| {
        for _ in 0..16 {
            scope.spawn(|| assert!(logger.push(&message)));
        }
    });
    assert_eq!(*body.lock().unwrap(), message.encode().unwrap());
    assert_eq!(dropped.load(Ordering::SeqCst), 0);
    drop(logger);
    assert_eq!(dropped.load(Ordering::SeqCst), 1);
}

#[test]
fn custom_unsafe_transport_and_contained_panic() {
    use std::sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    };
    for panic in [false, true] {
        let body = Arc::new(Mutex::new(Vec::new()));
        let dropped = Arc::new(AtomicUsize::new(0));
        let transport = RecordingTransport {
            body,
            dropped: dropped.clone(),
            calls: std::cell::Cell::new(0),
            panic,
        };
        let logger = lokiverse::make_unsafe(transport).unwrap();
        assert_eq!(logger.push(&sample()), !panic);
        drop(logger);
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
    }
}

#[test]
fn custom_logger_and_direct_transport() {
    struct Custom;
    impl Logger for Custom {
        fn push(&self, message: &Message) -> bool {
            message.encode().is_some()
        }
    }
    assert!(!Custom.push(&Message::new()));
    assert!(Custom.push(&sample()));
    let (port, requests) = receiver(202);
    let mut transport = make_http("127.0.0.1", port).unwrap();
    assert_eq!(
        lokiverse::Transport::post(&mut transport, "/direct", b"a\0b", "custom/type"),
        202
    );
    let request = requests.recv_timeout(Duration::from_secs(3)).unwrap();
    assert_eq!(request.path, "/direct");
    assert_eq!(request.content_type, "custom/type");
    assert_eq!(request.body, b"a\0b");
}
