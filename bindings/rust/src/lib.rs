use std::any::Any;
use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_int, c_void};
use std::fmt;
use std::marker::PhantomData;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::{self, NonNull};
use std::slice;
use std::time::{SystemTime, UNIX_EPOCH};

mod sys {
    use std::ffi::{c_char, c_int, c_void};

    #[repr(C)]
    pub struct Message {
        _private: [u8; 0],
    }

    #[repr(C)]
    pub struct Stream {
        _private: [u8; 0],
    }

    #[repr(C)]
    pub struct Logger {
        _private: [u8; 0],
    }

    #[repr(C)]
    pub struct Transport {
        _private: [u8; 0],
    }

    #[repr(C)]
    pub struct TransportCallbacks {
        pub post: unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            usize,
            *const c_char,
            usize,
            *const c_char,
            usize,
        ) -> c_int,
        pub destroy: unsafe extern "C" fn(*mut c_void),
    }

    pub const OK: c_int = 0;
    pub const INVALID_ARGUMENT: c_int = 1;
    pub const NO_MEMORY: c_int = 2;

    unsafe extern "C" {
        pub fn lokiverse_message_new() -> *mut Message;
        pub fn lokiverse_message_free(message: *mut Message);
        pub fn lokiverse_message_stream(message: *mut Message) -> *mut Stream;
        pub fn lokiverse_message_encode(
            message: *const Message,
            data: *mut *mut c_char,
            size: *mut usize,
        ) -> c_int;
        pub fn lokiverse_buffer_free(data: *mut c_char);
        pub fn lokiverse_stream_label(
            stream: *mut Stream,
            name: *const c_char,
            name_size: usize,
            value: *const c_char,
            value_size: usize,
        ) -> c_int;
        pub fn lokiverse_stream_entry(
            stream: *mut Stream,
            timestamp_ns: u64,
            line: *const c_char,
            line_size: usize,
        ) -> c_int;
        pub fn lokiverse_stream_entry_now(
            stream: *mut Stream,
            line: *const c_char,
            line_size: usize,
        ) -> c_int;
        pub fn lokiverse_stream_commit(stream: *mut Stream) -> c_int;
        pub fn lokiverse_stream_free(stream: *mut Stream);
        pub fn lokiverse_transport_make_http(
            host: *const c_char,
            host_size: usize,
            port: c_int,
            transport: *mut *mut Transport,
        ) -> c_int;
        pub fn lokiverse_transport_make_custom(
            callbacks: TransportCallbacks,
            context: *mut c_void,
            transport: *mut *mut Transport,
        ) -> c_int;
        pub fn lokiverse_transport_post(
            transport: *mut Transport,
            path: *const c_char,
            path_size: usize,
            body: *const c_char,
            body_size: usize,
            content_type: *const c_char,
            content_type_size: usize,
            status: *mut c_int,
        ) -> c_int;
        pub fn lokiverse_transport_free(transport: *mut Transport);
        pub fn lokiverse_logger_make_safe(
            transport: *mut Transport,
            logger: *mut *mut Logger,
        ) -> c_int;
        pub fn lokiverse_logger_make_unsafe(
            transport: *mut Transport,
            logger: *mut *mut Logger,
        ) -> c_int;
        pub fn lokiverse_stream_reserve(
            stream: *mut Stream,
            label_count: usize,
            value_count: usize,
        ) -> c_int;
        pub fn lokiverse_logger_push(
            logger: *mut Logger,
            message: *const Message,
            success: *mut c_int,
        ) -> c_int;
        pub fn lokiverse_logger_free(logger: *mut Logger);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum Error {
    NoEntries,
    InvalidArgument,
    NoMemory,
    Internal,
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Error::NoEntries => "stream has no entries",
            Error::InvalidArgument => "invalid argument",
            Error::NoMemory => "out of memory",
            Error::Internal => "internal error",
        })
    }
}

impl std::error::Error for Error {}

fn check(status: c_int) -> Result<(), Error> {
    match status {
        sys::OK => Ok(()),
        sys::INVALID_ARGUMENT => Err(Error::InvalidArgument),
        sys::NO_MEMORY => Err(Error::NoMemory),
        _ => Err(Error::Internal),
    }
}

fn chars(bytes: &[u8]) -> (*const c_char, usize) {
    (bytes.as_ptr().cast(), bytes.len())
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Value {
    pub timestamp_ns: u64,
    pub line: String,
}

impl Value {
    pub fn now(line: impl Into<String>) -> Self {
        Self {
            timestamp_ns: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("clock precedes Unix epoch")
                .as_nanos() as u64,
            line: line.into(),
        }
    }
}

pub type Labels = Vec<(String, String)>;

pub struct Message {
    handle: NonNull<sys::Message>,
}

unsafe impl Send for Message {}
unsafe impl Sync for Message {}

impl Message {
    pub fn new() -> Self {
        let handle = unsafe { sys::lokiverse_message_new() };
        Self {
            handle: NonNull::new(handle).expect("lokiverse: out of memory"),
        }
    }

    pub fn stream(&mut self) -> StreamBuilder<'_> {
        let handle = unsafe { sys::lokiverse_message_stream(self.handle.as_ptr()) };
        StreamBuilder {
            handle: NonNull::new(handle).expect("lokiverse: out of memory"),
            error: None,
            message: PhantomData,
        }
    }

    pub fn encode(&self) -> Option<Vec<u8>> {
        let mut data = ptr::null_mut();
        let mut size = 0;
        let status =
            unsafe { sys::lokiverse_message_encode(self.handle.as_ptr(), &mut data, &mut size) };
        if status != sys::OK || data.is_null() {
            return None;
        }
        let body = unsafe { slice::from_raw_parts(data.cast::<u8>(), size) }.to_vec();
        unsafe { sys::lokiverse_buffer_free(data) };
        Some(body)
    }
}

impl Default for Message {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Message {
    fn drop(&mut self) {
        unsafe { sys::lokiverse_message_free(self.handle.as_ptr()) }
    }
}

impl fmt::Debug for Message {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("Message").finish_non_exhaustive()
    }
}

pub struct StreamBuilder<'a> {
    handle: NonNull<sys::Stream>,
    error: Option<Error>,
    message: PhantomData<&'a mut Message>,
}

unsafe impl Send for StreamBuilder<'_> {}

impl StreamBuilder<'_> {
    fn record(&mut self, status: c_int) -> &mut Self {
        if let Err(error) = check(status) {
            self.error.get_or_insert(error);
        }
        self
    }

    pub fn reserve(&mut self, label_count: usize, value_count: usize) -> &mut Self {
        let status = unsafe {
            sys::lokiverse_stream_reserve(self.handle.as_ptr(), label_count, value_count)
        };
        self.record(status)
    }

    pub fn label(&mut self, name: impl AsRef<str>, value: impl AsRef<str>) -> &mut Self {
        let (name, name_size) = chars(name.as_ref().as_bytes());
        let (value, value_size) = chars(value.as_ref().as_bytes());
        let status = unsafe {
            sys::lokiverse_stream_label(self.handle.as_ptr(), name, name_size, value, value_size)
        };
        self.record(status)
    }

    pub fn entry(&mut self, timestamp_ns: u64, line: impl AsRef<str>) -> &mut Self {
        let (line, line_size) = chars(line.as_ref().as_bytes());
        let status = unsafe {
            sys::lokiverse_stream_entry(self.handle.as_ptr(), timestamp_ns, line, line_size)
        };
        self.record(status)
    }

    pub fn entry_now(&mut self, line: impl AsRef<str>) -> &mut Self {
        let (line, line_size) = chars(line.as_ref().as_bytes());
        let status =
            unsafe { sys::lokiverse_stream_entry_now(self.handle.as_ptr(), line, line_size) };
        self.record(status)
    }

    pub fn commit(&mut self) -> Result<(), Error> {
        if let Some(error) = self.error {
            return Err(error);
        }
        match unsafe { sys::lokiverse_stream_commit(self.handle.as_ptr()) } {
            sys::INVALID_ARGUMENT => Err(Error::NoEntries),
            status => check(status),
        }
    }
}

impl Drop for StreamBuilder<'_> {
    fn drop(&mut self) {
        unsafe { sys::lokiverse_stream_free(self.handle.as_ptr()) }
    }
}

impl fmt::Debug for StreamBuilder<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("StreamBuilder")
            .finish_non_exhaustive()
    }
}

pub trait Transport {
    fn post(&mut self, path: &str, body: &[u8], content_type: &str) -> i32;
}

pub trait Logger {
    fn push(&self, message: &Message) -> bool;
}

pub struct HttpTransport {
    handle: NonNull<sys::Transport>,
}

unsafe impl Send for HttpTransport {}

pub fn make_http(host: &str, port: u16) -> Result<HttpTransport, Error> {
    let (host, size) = chars(host.as_bytes());
    let mut handle = ptr::null_mut();
    check(unsafe {
        sys::lokiverse_transport_make_http(host, size, c_int::from(port), &mut handle)
    })?;
    Ok(HttpTransport {
        handle: NonNull::new(handle).ok_or(Error::Internal)?,
    })
}

impl Transport for HttpTransport {
    fn post(&mut self, path: &str, body: &[u8], content_type: &str) -> i32 {
        let (path, pn) = chars(path.as_bytes());
        let (body, bn) = chars(body);
        let (content_type, cn) = chars(content_type.as_bytes());
        let mut status = -1;
        let result = unsafe {
            sys::lokiverse_transport_post(
                self.handle.as_ptr(),
                path,
                pn,
                body,
                bn,
                content_type,
                cn,
                &mut status,
            )
        };
        if result == sys::OK { status } else { -1 }
    }
}

impl Drop for HttpTransport {
    fn drop(&mut self) {
        unsafe { sys::lokiverse_transport_free(self.handle.as_ptr()) }
    }
}

struct Context<T> {
    transport: RefCell<T>,
}

unsafe extern "C" fn post<T: Transport>(
    context: *mut c_void,
    path: *const c_char,
    pn: usize,
    body: *const c_char,
    bn: usize,
    content_type: *const c_char,
    cn: usize,
) -> c_int {
    catch_unwind(AssertUnwindSafe(|| {
        let context = unsafe { &*context.cast::<Context<T>>() };
        let Ok(mut transport) = context.transport.try_borrow_mut() else {
            return -1;
        };
        let bytes = |data: *const c_char, size: usize| {
            if size == 0 {
                &[][..]
            } else {
                unsafe { slice::from_raw_parts(data.cast::<u8>(), size) }
            }
        };
        let Ok(path) = std::str::from_utf8(bytes(path, pn)) else {
            return -1;
        };
        let Ok(content_type) = std::str::from_utf8(bytes(content_type, cn)) else {
            return -1;
        };
        transport.post(path, bytes(body, bn), content_type)
    }))
    .unwrap_or(-1)
}

unsafe extern "C" fn destroy<T>(context: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(context.cast::<Context<T>>()) });
    }));
}

fn create_logger<T: Transport + 'static>(
    transport: T,
    safe: bool,
) -> Result<NonNull<sys::Logger>, Error> {
    let mut logger = ptr::null_mut();
    if let Some(http) = (&transport as &dyn Any).downcast_ref::<HttpTransport>() {
        let status = unsafe {
            if safe {
                sys::lokiverse_logger_make_safe(http.handle.as_ptr(), &mut logger)
            } else {
                sys::lokiverse_logger_make_unsafe(http.handle.as_ptr(), &mut logger)
            }
        };
        check(status)?;
    } else {
        let context = Box::into_raw(Box::new(Context {
            transport: RefCell::new(transport),
        }))
        .cast::<c_void>();
        let callbacks = sys::TransportCallbacks {
            post: post::<T>,
            destroy: destroy::<T>,
        };
        let mut handle = ptr::null_mut();
        let result =
            unsafe { sys::lokiverse_transport_make_custom(callbacks, context, &mut handle) };
        if result != sys::OK {
            unsafe { destroy::<T>(context) };
            check(result)?;
        }
        let status = unsafe {
            if safe {
                sys::lokiverse_logger_make_safe(handle, &mut logger)
            } else {
                sys::lokiverse_logger_make_unsafe(handle, &mut logger)
            }
        };
        unsafe { sys::lokiverse_transport_free(handle) };
        check(status)?;
    }
    NonNull::new(logger).ok_or(Error::Internal)
}

pub struct SafeLogger<T: Transport + Send> {
    handle: NonNull<sys::Logger>,
    transport: PhantomData<T>,
}

pub struct UnsafeLogger<T: Transport> {
    handle: NonNull<sys::Logger>,
    transport: PhantomData<T>,
    unsynchronized: PhantomData<Cell<()>>,
}

unsafe impl<T: Transport + Send> Send for SafeLogger<T> {}
unsafe impl<T: Transport + Send> Sync for SafeLogger<T> {}
unsafe impl<T: Transport + Send> Send for UnsafeLogger<T> {}

pub fn make_safe<T: Transport + Send + 'static>(transport: T) -> Result<SafeLogger<T>, Error> {
    Ok(SafeLogger {
        handle: create_logger(transport, true)?,
        transport: PhantomData,
    })
}

pub fn make_unsafe<T: Transport + 'static>(transport: T) -> Result<UnsafeLogger<T>, Error> {
    Ok(UnsafeLogger {
        handle: create_logger(transport, false)?,
        transport: PhantomData,
        unsynchronized: PhantomData,
    })
}

fn push(handle: NonNull<sys::Logger>, message: &Message) -> bool {
    let mut success = 0;
    let status = unsafe {
        sys::lokiverse_logger_push(handle.as_ptr(), message.handle.as_ptr(), &mut success)
    };
    status == sys::OK && success != 0
}

impl<T: Transport + Send> Logger for SafeLogger<T> {
    fn push(&self, message: &Message) -> bool {
        push(self.handle, message)
    }
}

impl<T: Transport> Logger for UnsafeLogger<T> {
    fn push(&self, message: &Message) -> bool {
        push(self.handle, message)
    }
}

impl<T: Transport + Send> Drop for SafeLogger<T> {
    fn drop(&mut self) {
        unsafe { sys::lokiverse_logger_free(self.handle.as_ptr()) }
    }
}

impl<T: Transport> Drop for UnsafeLogger<T> {
    fn drop(&mut self) {
        unsafe { sys::lokiverse_logger_free(self.handle.as_ptr()) }
    }
}
