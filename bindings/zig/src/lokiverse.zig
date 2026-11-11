const std = @import("std");

const c = struct {
    const MessageHandle = opaque {};
    const StreamHandle = opaque {};
    const LoggerHandle = opaque {};
    const TransportHandle = opaque {};
    const TransportCallbacks = extern struct {
        post: *const fn (?*anyopaque, [*]const u8, usize, [*]const u8, usize, [*]const u8, usize) callconv(.c) c_int,
        destroy: ?*const fn (?*anyopaque) callconv(.c) void,
    };
    const LoggerCallbacks = extern struct {
        push: *const fn (?*anyopaque, *const MessageHandle, *c_int) callconv(.c) c_int,
        destroy: ?*const fn (?*anyopaque) callconv(.c) void,
    };

    const ok: c_int = 0;
    const invalid_argument: c_int = 1;
    const no_memory: c_int = 2;

    extern fn lokiverse_message_new() ?*MessageHandle;
    extern fn lokiverse_message_free(message: ?*MessageHandle) void;
    extern fn lokiverse_message_stream(message: ?*MessageHandle) ?*StreamHandle;
    extern fn lokiverse_message_encode(message: ?*const MessageHandle, data: *?[*]u8, size: *usize) c_int;
    extern fn lokiverse_buffer_free(data: ?[*]u8) void;
    extern fn lokiverse_stream_label(stream: ?*StreamHandle, name: [*]const u8, name_size: usize, value: [*]const u8, value_size: usize) c_int;
    extern fn lokiverse_stream_entry(stream: ?*StreamHandle, timestamp_ns: u64, line: [*]const u8, line_size: usize) c_int;
    extern fn lokiverse_stream_entry_now(stream: ?*StreamHandle, line: [*]const u8, line_size: usize) c_int;
    extern fn lokiverse_stream_commit(stream: ?*StreamHandle) c_int;
    extern fn lokiverse_stream_free(stream: ?*StreamHandle) void;
    extern fn lokiverse_transport_make_http(host: [*]const u8, host_size: usize, port: c_int, transport: *?*TransportHandle) c_int;
    extern fn lokiverse_transport_make_custom(callbacks: TransportCallbacks, context: ?*anyopaque, transport: *?*TransportHandle) c_int;
    extern fn lokiverse_transport_post(transport: *TransportHandle, path: [*]const u8, path_size: usize, body: [*]const u8, body_size: usize, content_type: [*]const u8, content_type_size: usize, status: *c_int) c_int;
    extern fn lokiverse_transport_free(transport: ?*TransportHandle) void;
    extern fn lokiverse_logger_make_safe(transport: *TransportHandle, logger: *?*LoggerHandle) c_int;
    extern fn lokiverse_logger_make_unsafe(transport: *TransportHandle, logger: *?*LoggerHandle) c_int;
    extern fn lokiverse_logger_make_custom(callbacks: LoggerCallbacks, context: ?*anyopaque, logger: *?*LoggerHandle) c_int;
    extern fn lokiverse_stream_reserve(stream: *StreamHandle, label_count: usize, value_count: usize) c_int;
    extern fn lokiverse_logger_push(logger: ?*LoggerHandle, message: ?*const MessageHandle, success: *c_int) c_int;
    extern fn lokiverse_logger_free(logger: ?*LoggerHandle) void;
};

pub const Error = error{
    NoEntries,
    InvalidUtf8,
    InvalidArgument,
    OutOfMemory,
    Internal,
};

fn check(status: c_int) Error!void {
    return switch (status) {
        c.ok => {},
        c.invalid_argument => error.InvalidArgument,
        c.no_memory => error.OutOfMemory,
        else => error.Internal,
    };
}

fn text(bytes: []const u8) Error!void {
    if (!std.unicode.utf8ValidateSlice(bytes)) return error.InvalidUtf8;
}

pub const Value = struct {
    timestamp_ns: u64,
    line: []u8,

    pub fn now(allocator: std.mem.Allocator, line: []const u8) !Value {
        var timestamp: std.c.timespec = undefined;
        if (std.c.clock_gettime(std.c.CLOCK.REALTIME, &timestamp) != 0) return error.Internal;
        return .{ .timestamp_ns = @as(u64, @intCast(timestamp.sec)) * 1_000_000_000 + @as(u64, @intCast(timestamp.nsec)), .line = try allocator.dupe(u8, line) };
    }

    pub fn deinit(self: *Value, allocator: std.mem.Allocator) void {
        allocator.free(self.line);
        self.* = undefined;
    }
};

pub const Label = struct { name: []const u8, value: []const u8 };
pub const Labels = []const Label;

pub const Message = struct {
    handle: *c.MessageHandle,

    pub fn init() Error!Message {
        return .{ .handle = c.lokiverse_message_new() orelse return error.OutOfMemory };
    }

    pub fn deinit(self: *Message) void {
        c.lokiverse_message_free(self.handle);
        self.* = undefined;
    }

    pub fn stream(self: *Message) Error!StreamBuilder {
        return .{ .handle = c.lokiverse_message_stream(self.handle) orelse return error.OutOfMemory };
    }

    pub fn encode(self: *const Message, allocator: std.mem.Allocator) error{OutOfMemory}!?[]u8 {
        var data: ?[*]u8 = null;
        var size: usize = 0;
        const status = c.lokiverse_message_encode(self.handle, &data, &size);
        if (status == c.no_memory) return error.OutOfMemory;
        const bytes = data orelse return null;
        defer c.lokiverse_buffer_free(bytes);
        if (status != c.ok) return null;
        return try allocator.dupe(u8, bytes[0..size]);
    }
};

pub const StreamBuilder = struct {
    handle: *c.StreamHandle,

    pub fn deinit(self: *StreamBuilder) void {
        c.lokiverse_stream_free(self.handle);
        self.* = undefined;
    }

    pub fn reserve(self: *StreamBuilder, label_count: usize, value_count: usize) Error!void {
        try check(c.lokiverse_stream_reserve(self.handle, label_count, value_count));
    }

    pub fn label(self: *StreamBuilder, name: []const u8, value: []const u8) Error!void {
        try text(name);
        try text(value);
        try check(c.lokiverse_stream_label(self.handle, name.ptr, name.len, value.ptr, value.len));
    }

    pub fn entry(self: *StreamBuilder, timestamp_ns: u64, line: []const u8) Error!void {
        try text(line);
        try check(c.lokiverse_stream_entry(self.handle, timestamp_ns, line.ptr, line.len));
    }

    pub fn entryNow(self: *StreamBuilder, line: []const u8) Error!void {
        try text(line);
        try check(c.lokiverse_stream_entry_now(self.handle, line.ptr, line.len));
    }

    pub fn commit(self: *StreamBuilder) Error!void {
        return switch (c.lokiverse_stream_commit(self.handle)) {
            c.invalid_argument => error.NoEntries,
            else => |status| check(status),
        };
    }
};

pub const Transport = struct {
    handle: *c.TransportHandle,

    pub const VTable = struct {
        post: *const fn (*anyopaque, []const u8, []const u8, []const u8) i32,
        deinit: ?*const fn (*anyopaque) void = null,
    };

    const Context = struct {
        allocator: std.mem.Allocator,
        pointer: *anyopaque,
        vtable: VTable,

        fn post(context: ?*anyopaque, path: [*]const u8, pn: usize, body: [*]const u8, bn: usize, content_type: [*]const u8, cn: usize) callconv(.c) c_int {
            const self: *Context = @ptrCast(@alignCast(context.?));
            return self.vtable.post(self.pointer, path[0..pn], body[0..bn], content_type[0..cn]);
        }

        fn destroy(context: ?*anyopaque) callconv(.c) void {
            const self: *Context = @ptrCast(@alignCast(context.?));
            if (self.vtable.deinit) |deinit_fn| deinit_fn(self.pointer);
            self.allocator.destroy(self);
        }
    };

    pub fn makeHttp(host: []const u8, port: u16) Error!Transport {
        var handle: ?*c.TransportHandle = null;
        try text(host);
        try check(c.lokiverse_transport_make_http(host.ptr, host.len, port, &handle));
        return .{ .handle = handle orelse return error.Internal };
    }

    pub fn makeCustom(allocator: std.mem.Allocator, pointer: *anyopaque, vtable: VTable) Error!Transport {
        const context = try allocator.create(Context);
        errdefer allocator.destroy(context);
        context.* = .{ .allocator = allocator, .pointer = pointer, .vtable = vtable };
        var handle: ?*c.TransportHandle = null;
        try check(c.lokiverse_transport_make_custom(.{ .post = Context.post, .destroy = Context.destroy }, context, &handle));
        return .{ .handle = handle orelse return error.Internal };
    }

    pub fn from(allocator: std.mem.Allocator, pointer: anytype) Error!Transport {
        const T = @TypeOf(pointer);
        const Adapter = struct {
            fn post(context: *anyopaque, path: []const u8, body: []const u8, content_type: []const u8) i32 {
                const typed: T = @ptrCast(@alignCast(context));
                return typed.post(path, body, content_type);
            }
        };
        return makeCustom(allocator, pointer, .{ .post = Adapter.post });
    }

    pub fn deinit(self: *Transport) void {
        c.lokiverse_transport_free(self.handle);
        self.* = undefined;
    }

    pub fn post(self: *const Transport, path: []const u8, body: []const u8, content_type: []const u8) i32 {
        var status: c_int = -1;
        if (c.lokiverse_transport_post(self.handle, path.ptr, path.len, body.ptr, body.len, content_type.ptr, content_type.len, &status) != c.ok) return -1;
        return status;
    }
};

pub const Logger = struct {
    handle: *c.LoggerHandle,

    pub const VTable = struct {
        push: *const fn (*anyopaque, *const Message) bool,
        deinit: ?*const fn (*anyopaque) void = null,
    };

    const Context = struct {
        allocator: std.mem.Allocator,
        pointer: *anyopaque,
        vtable: VTable,

        fn push(context: ?*anyopaque, message: *const c.MessageHandle, success: *c_int) callconv(.c) c_int {
            const self: *Context = @ptrCast(@alignCast(context.?));
            const borrowed: Message = .{ .handle = @constCast(message) };
            success.* = @intFromBool(self.vtable.push(self.pointer, &borrowed));
            return c.ok;
        }

        fn destroy(context: ?*anyopaque) callconv(.c) void {
            const self: *Context = @ptrCast(@alignCast(context.?));
            if (self.vtable.deinit) |deinit_fn| deinit_fn(self.pointer);
            self.allocator.destroy(self);
        }
    };

    pub fn makeSafe(transport: *const Transport) Error!Logger {
        var handle: ?*c.LoggerHandle = null;
        try check(c.lokiverse_logger_make_safe(transport.handle, &handle));
        return .{ .handle = handle orelse return error.Internal };
    }

    pub fn makeUnsafe(transport: *const Transport) Error!Logger {
        var handle: ?*c.LoggerHandle = null;
        try check(c.lokiverse_logger_make_unsafe(transport.handle, &handle));
        return .{ .handle = handle orelse return error.Internal };
    }

    pub fn makeCustom(allocator: std.mem.Allocator, pointer: *anyopaque, vtable: VTable) Error!Logger {
        const context = try allocator.create(Context);
        errdefer allocator.destroy(context);
        context.* = .{ .allocator = allocator, .pointer = pointer, .vtable = vtable };
        var handle: ?*c.LoggerHandle = null;
        try check(c.lokiverse_logger_make_custom(.{ .push = Context.push, .destroy = Context.destroy }, context, &handle));
        return .{ .handle = handle orelse return error.Internal };
    }

    pub fn from(allocator: std.mem.Allocator, pointer: anytype) Error!Logger {
        const T = @TypeOf(pointer);
        const Adapter = struct {
            fn push(context: *anyopaque, message: *const Message) bool {
                const typed: T = @ptrCast(@alignCast(context));
                return typed.push(message);
            }
        };
        return makeCustom(allocator, pointer, .{ .push = Adapter.push });
    }

    pub fn deinit(self: *Logger) void {
        c.lokiverse_logger_free(self.handle);
        self.* = undefined;
    }

    pub fn push(self: *const Logger, message: *const Message) bool {
        var success: c_int = 0;
        return c.lokiverse_logger_push(self.handle, message.handle, &success) == c.ok and success != 0;
    }
};
