const std = @import("std");
const lokiverse = @import("lokiverse");
const wire = @import("wire.zig");

const testing = std.testing;
const c = std.c;

fn httpLogger(host: []const u8, port: u16) !lokiverse.Logger {
    var transport = try lokiverse.Transport.makeHttp(host, port);
    defer transport.deinit();
    return lokiverse.Logger.makeSafe(&transport);
}

const Request = struct {
    path: []const u8 = "",
    content_type: []const u8 = "",
    body: []const u8 = "",
};

const Receiver = struct {
    socket: c.fd_t,
    port: u16,
    status: u16,
    requests: []Request,
    arena: std.mem.Allocator,
    thread: ?std.Thread = null,

    fn start(arena: std.mem.Allocator, status: u16, count: usize) !*Receiver {
        const socket = c.socket(c.AF.INET, c.SOCK.STREAM, 0);
        if (socket < 0) return error.SocketFailed;
        errdefer _ = c.close(socket);
        var address: c.sockaddr.in = .{
            .port = 0,
            .addr = std.mem.nativeToBig(u32, 0x7f000001),
        };
        var size: c.socklen_t = @sizeOf(c.sockaddr.in);
        if (c.bind(socket, @ptrCast(&address), size) != 0) return error.BindFailed;
        if (c.listen(socket, 64) != 0) return error.ListenFailed;
        if (c.getsockname(socket, @ptrCast(&address), &size) != 0) return error.NameFailed;
        const self = try arena.create(Receiver);
        self.* = .{
            .socket = socket,
            .port = std.mem.bigToNative(u16, address.port),
            .status = status,
            .requests = try arena.alloc(Request, count),
            .arena = arena,
        };
        @memset(self.requests, .{});
        if (count > 0) self.thread = try std.Thread.spawn(.{}, serve, .{self});
        return self;
    }

    fn finish(self: *Receiver) []const Request {
        if (self.thread) |thread| thread.join();
        self.thread = null;
        _ = c.close(self.socket);
        return self.requests;
    }

    fn serve(self: *Receiver) void {
        for (self.requests) |*request| {
            const connection = c.accept(self.socket, null, null);
            if (connection < 0) return;
            defer _ = c.close(connection);
            request.* = self.read(connection) catch .{};
            var buffer: [128]u8 = undefined;
            const response = std.fmt.bufPrint(
                &buffer,
                "HTTP/1.1 {d} Status\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                .{self.status},
            ) catch return;
            _ = c.send(connection, response.ptr, response.len, 0);
        }
    }

    fn read(self: *Receiver, connection: c.fd_t) !Request {
        var data: std.ArrayList(u8) = .empty;
        var head_end: ?usize = null;
        var length: usize = 0;
        var chunk: [16384]u8 = undefined;
        while (head_end == null or data.items.len < head_end.? + length) {
            const count = c.recv(connection, &chunk, chunk.len, 0);
            if (count <= 0) return error.Closed;
            try data.appendSlice(self.arena, chunk[0..@intCast(count)]);
            if (head_end == null) {
                if (std.mem.find(u8, data.items, "\r\n\r\n")) |position| {
                    head_end = position + 4;
                    length = try contentLength(data.items[0..position]);
                }
            }
        }
        const head = data.items[0 .. head_end.? - 4];
        var request: Request = .{ .body = data.items[head_end.?..][0..length] };
        var lines = std.mem.splitSequence(u8, head, "\r\n");
        var parts = std.mem.splitScalar(u8, lines.first(), ' ');
        _ = parts.first();
        request.path = parts.next() orelse "";
        while (lines.next()) |line| {
            if (header(line, "content-type")) |value| request.content_type = value;
        }
        return request;
    }

    fn header(line: []const u8, name: []const u8) ?[]const u8 {
        const colon = std.mem.findScalar(u8, line, ':') orelse return null;
        if (!std.ascii.eqlIgnoreCase(line[0..colon], name)) return null;
        return std.mem.trim(u8, line[colon + 1 ..], " ");
    }

    fn contentLength(head: []const u8) !usize {
        var lines = std.mem.splitSequence(u8, head, "\r\n");
        while (lines.next()) |line| {
            if (header(line, "content-length")) |value| return std.fmt.parseInt(usize, value, 10);
        }
        return 0;
    }
};

fn now() u64 {
    var time: c.timespec = undefined;
    _ = c.clock_gettime(.REALTIME, &time);
    return @as(u64, @intCast(time.sec)) * 1_000_000_000 + @as(u64, @intCast(time.nsec));
}

fn sample() !lokiverse.Message {
    var message = try lokiverse.Message.init();
    errdefer message.deinit();
    var stream = try message.stream();
    defer stream.deinit();
    try stream.label("job", "zig");
    try stream.entry(1, "hello");
    try stream.commit();
    return message;
}

fn decoded(arena: std.mem.Allocator, message: *const lokiverse.Message) ![]const wire.Stream {
    const body = (try message.encode(arena)) orelse return error.NoBody;
    return wire.decode(arena, body);
}

fn expectEntry(entry: wire.Entry, timestamp: u64, line: []const u8) !void {
    try testing.expectEqual(timestamp, entry.timestamp);
    try testing.expectEqualStrings(line, entry.line);
}

test "encodes multiple streams" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    var message = try lokiverse.Message.init();
    defer message.deinit();
    {
        var stream = try message.stream();
        defer stream.deinit();
        try stream.label("job", "api");
        try stream.label("env", "test");
        try stream.entry(1700000000123456789, "first");
        try stream.entry(1700000000123456790, "second");
        try stream.commit();
        try stream.label("job", "worker");
        try stream.entry(1700000001123456789, "third");
        try stream.commit();
    }
    const streams = try decoded(arena, &message);
    try testing.expectEqual(2, streams.len);
    try testing.expectEqualStrings("{env=\"test\",job=\"api\"}", streams[0].labels);
    try testing.expectEqual(2, streams[0].entries.len);
    try expectEntry(streams[0].entries[0], 1700000000123456789, "first");
    try expectEntry(streams[0].entries[1], 1700000000123456790, "second");
    try testing.expectEqualStrings("{job=\"worker\"}", streams[1].labels);
    try testing.expectEqual(1, streams[1].entries.len);
    try expectEntry(streams[1].entries[0], 1700000001123456789, "third");
}

test "timestamp boundaries and empty lines" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    const timestamps = [_]u64{ 0, 1, 999_999_999, 1_000_000_000, 1700000000123456789, std.math.maxInt(u64) };
    var message = try lokiverse.Message.init();
    defer message.deinit();
    {
        var stream = try message.stream();
        defer stream.deinit();
        for (timestamps) |timestamp| try stream.entry(timestamp, "");
        try stream.commit();
    }
    const streams = try decoded(arena, &message);
    try testing.expectEqualStrings("{}", streams[0].labels);
    try testing.expectEqual(timestamps.len, streams[0].entries.len);
    for (streams[0].entries, timestamps) |entry, timestamp| {
        try testing.expect(entry.has_timestamp);
        try testing.expect(entry.nanos < 1_000_000_000);
        try expectEntry(entry, timestamp, "");
    }
}

test "unicode nulls and escaped labels" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    const line = "say \"hi\"\\\nnext\ttab\x00fin: café 🐍";
    var message = try lokiverse.Message.init();
    defer message.deinit();
    {
        var stream = try message.stream();
        defer stream.deinit();
        try stream.label("clé\x00", "quote\"\\\n\x00café 🐍");
        try stream.entry(1, line);
        try stream.commit();
    }
    const streams = try decoded(arena, &message);
    try testing.expectEqualStrings("{\"clé\\u0000\"=\"quote\\\"\\\\\\u000a\\u0000café 🐍\"}", streams[0].labels);
    try expectEntry(streams[0].entries[0], 1, line);
}

test "invalid UTF-8 is rejected" {
    var message = try lokiverse.Message.init();
    defer message.deinit();
    var stream = try message.stream();
    defer stream.deinit();
    try testing.expectError(error.InvalidUtf8, stream.label("\xff", "value"));
    try testing.expectError(error.InvalidUtf8, stream.label("name", "\xff"));
    try testing.expectError(error.InvalidUtf8, stream.entry(1, "\xff"));
    try testing.expectError(error.InvalidUtf8, stream.entryNow("\xff"));
    try testing.expectError(error.NoEntries, stream.commit());
    try testing.expectEqual(null, try message.encode(testing.allocator));
}

test "entryNow uses the wall clock" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    var message = try lokiverse.Message.init();
    defer message.deinit();
    var stream = try message.stream();
    defer stream.deinit();
    const before = now();
    try stream.entryNow("now");
    const after = now();
    try stream.commit();
    const streams = try decoded(arena, &message);
    const entry = streams[0].entries[0];
    try testing.expect(entry.timestamp >= before and entry.timestamp <= after);
    try testing.expectEqualStrings("now", entry.line);
}

test "commit requires entries and the builder is reusable" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    var message = try lokiverse.Message.init();
    defer message.deinit();
    {
        var discarded = try message.stream();
        defer discarded.deinit();
        try discarded.label("job", "discarded");
        try discarded.entry(1, "discarded");
    }
    try testing.expectEqual(null, try message.encode(arena));
    var stream = try message.stream();
    defer stream.deinit();
    try stream.label("job", "zig");
    try testing.expectError(error.NoEntries, stream.commit());
    try testing.expectEqual(null, try message.encode(arena));
    try stream.entry(2, "second");
    try stream.entry(1, "first");
    try stream.commit();
    try testing.expectError(error.NoEntries, stream.commit());
    try stream.label("job", "zig");
    try stream.entry(1, "separate");
    try stream.commit();
    const streams = try decoded(arena, &message);
    try testing.expectEqual(2, streams.len);
    try testing.expectEqualStrings("{job=\"zig\"}", streams[0].labels);
    try testing.expectEqualStrings("{job=\"zig\"}", streams[1].labels);
    try expectEntry(streams[0].entries[0], 2, "second");
    try expectEntry(streams[0].entries[1], 1, "first");
    try expectEntry(streams[1].entries[0], 1, "separate");
}

test "push posts the encoded body" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    const receiver = try Receiver.start(arena, 204, 2);
    var logger = try httpLogger("127.0.0.1", receiver.port);
    defer logger.deinit();
    var message = try sample();
    defer message.deinit();
    const body = (try message.encode(arena)).?;
    const first = logger.push(&message);
    const second = logger.push(&message);
    const requests = receiver.finish();
    try testing.expect(first and second);
    for (requests) |request| {
        try testing.expectEqualStrings("/loki/api/v1/push", request.path);
        try testing.expectEqualStrings("application/x-protobuf", request.content_type);
        try testing.expectEqualSlices(u8, body, request.body);
    }
}

test "result follows status" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    var message = try sample();
    defer message.deinit();
    for ([_]u16{ 200, 204, 400, 500 }) |status| {
        const receiver = try Receiver.start(arena, status, 1);
        var logger = try httpLogger("127.0.0.1", receiver.port);
        defer logger.deinit();
        const result = logger.push(&message);
        _ = receiver.finish();
        try testing.expectEqual(status == 204, result);
    }
}

test "empty message and refused connection return false" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    const receiver = try Receiver.start(arena, 204, 0);
    var logger = try httpLogger("127.0.0.1", receiver.port);
    defer logger.deinit();
    var empty = try lokiverse.Message.init();
    defer empty.deinit();
    try testing.expectEqual(null, try empty.encode(arena));
    try testing.expect(!logger.push(&empty));
    _ = receiver.finish();
    var message = try sample();
    defer message.deinit();
    try testing.expect(!logger.push(&message));
}

test "logger arguments" {
    try testing.expectError(error.InvalidArgument, httpLogger("", 3100));
    try testing.expectError(error.InvalidArgument, httpLogger("local\x00host", 3100));
    try testing.expectError(error.InvalidArgument, httpLogger("localhost", 0));
    for ([_]u16{ 1, 65535 }) |port| {
        var logger = try httpLogger("localhost", port);
        logger.deinit();
    }
}

const Pusher = struct {
    logger: *const lokiverse.Logger,
    message: *const lokiverse.Message,
    result: bool = false,

    fn run(self: *Pusher) void {
        self.result = self.logger.push(self.message);
    }
};

test "concurrent pushes" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    const receiver = try Receiver.start(arena, 204, 16);
    var logger = try httpLogger("127.0.0.1", receiver.port);
    defer logger.deinit();
    var message = try sample();
    defer message.deinit();
    const body = (try message.encode(arena)).?;
    var pushers: [16]Pusher = undefined;
    var threads: [16]std.Thread = undefined;
    for (&pushers, &threads) |*pusher, *thread| {
        pusher.* = .{ .logger = &logger, .message = &message };
        thread.* = try std.Thread.spawn(.{}, Pusher.run, .{pusher});
    }
    for (threads) |thread| thread.join();
    const requests = receiver.finish();
    for (pushers) |pusher| try testing.expect(pusher.result);
    for (requests) |request| try testing.expectEqualSlices(u8, body, request.body);
}

test "large payload" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    const big = try arena.alloc(u8, 1024 * 1024);
    for (big, 0..) |*byte, index| byte.* = if (index % 2 == 0) 0xc3 else 0xa9;
    var message = try lokiverse.Message.init();
    defer message.deinit();
    var stream = try message.stream();
    defer stream.deinit();
    try stream.entry(1, big);
    try stream.commit();
    try stream.label("job", "many");
    for (0..10_000) |index| try stream.entry(index, "line");
    try stream.commit();
    const streams = try decoded(arena, &message);
    try expectEntry(streams[0].entries[0], 1, big);
    try testing.expectEqual(10_000, streams[1].entries.len);
    try testing.expectEqual(9_999, streams[1].entries[9_999].timestamp);
}

test "reserve preserves contents and builder remains usable after overflow" {
    var message = try lokiverse.Message.init();
    defer message.deinit();
    var stream = try message.stream();
    defer stream.deinit();
    try stream.reserve(0, 0);
    try stream.reserve(1, 1);
    try stream.label("job", "kept");
    try stream.entry(1, "first");
    try stream.reserve(8, 256);
    try stream.reserve(0, 0);
    for (0..300) |i| try stream.entry(i, "later");
    try testing.expectError(error.InvalidArgument, stream.reserve(0, std.math.maxInt(usize)));
    try stream.commit();
    try stream.reserve(1, 1);
    try stream.entry(2, "reused");
    try stream.commit();
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const streams = try decoded(arena_state.allocator(), &message);
    try testing.expectEqual(@as(usize, 2), streams.len);
    try testing.expectEqual(@as(usize, 301), streams[0].entries.len);
    try expectEntry(streams[0].entries[0], 1, "first");
    const before = now();
    var value = try lokiverse.Value.now(testing.allocator, "a\x00b");
    defer value.deinit(testing.allocator);
    try testing.expect(value.timestamp_ns >= before and value.timestamp_ns <= now());
    try testing.expectEqualStrings("a\x00b", value.line);
}

const CustomTransport = struct {
    calls: usize = 0,
    destroyed: usize = 0,
    allocator: std.mem.Allocator,
    expected: []const u8,

    pub fn post(self: *CustomTransport, path: []const u8, body: []const u8, content_type: []const u8) i32 {
        if (!std.mem.eql(u8, path, "/loki/api/v1/push") or
            !std.mem.eql(u8, content_type, "application/x-protobuf") or
            !std.mem.eql(u8, body, self.expected)) return -1;
        self.calls += 1;
        return 204;
    }

    fn postOpaque(context: *anyopaque, path: []const u8, body: []const u8, content_type: []const u8) i32 {
        const self: *CustomTransport = @ptrCast(@alignCast(context));
        return self.post(path, body, content_type);
    }

    fn destroy(context: *anyopaque) void {
        const self: *CustomTransport = @ptrCast(@alignCast(context));
        self.destroyed += 1;
    }
};

test "custom transport lifetime and safe unsafe factories" {
    var message = try lokiverse.Message.init();
    defer message.deinit();
    var stream = try message.stream();
    defer stream.deinit();
    try stream.entry(1, "custom");
    try stream.commit();
    const body = (try message.encode(testing.allocator)).?;
    defer testing.allocator.free(body);
    for ([_]bool{ false, true }) |safe| {
        var context: CustomTransport = .{ .allocator = testing.allocator, .expected = body };
        var transport = try lokiverse.Transport.makeCustom(testing.allocator, &context, .{ .post = CustomTransport.postOpaque, .deinit = CustomTransport.destroy });
        var logger = if (safe) try lokiverse.Logger.makeSafe(&transport) else try lokiverse.Logger.makeUnsafe(&transport);
        transport.deinit();
        try testing.expectEqual(@as(usize, 0), context.destroyed);
        try testing.expect(logger.push(&message));
        try testing.expectEqual(@as(usize, 1), context.calls);
        logger.deinit();
        try testing.expectEqual(@as(usize, 1), context.destroyed);
    }
    var context: CustomTransport = .{ .allocator = testing.allocator, .expected = body };
    var transport = try lokiverse.Transport.from(testing.allocator, &context);
    defer transport.deinit();
    var logger = try lokiverse.Logger.makeSafe(&transport);
    defer logger.deinit();
    try testing.expect(logger.push(&message));
}

test "custom logger and direct HTTP transport" {
    const CustomLogger = struct {
        pub fn push(_: *@This(), message: *const lokiverse.Message) bool {
            const body = message.encode(testing.allocator) catch return false;
            if (body) |bytes| {
                testing.allocator.free(bytes);
                return true;
            }
            return false;
        }
    };
    var context: CustomLogger = .{};
    var logger = try lokiverse.Logger.from(testing.allocator, &context);
    defer logger.deinit();
    var message = try lokiverse.Message.init();
    defer message.deinit();
    try testing.expect(!logger.push(&message));
    var stream = try message.stream();
    defer stream.deinit();
    try stream.entry(1, "custom");
    try stream.commit();
    try testing.expect(logger.push(&message));
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const receiver = try Receiver.start(arena_state.allocator(), 202, 1);
    var transport = try lokiverse.Transport.makeHttp("127.0.0.1", receiver.port);
    defer transport.deinit();
    try testing.expectEqual(@as(i32, 202), transport.post("/direct", "a\x00b", "custom/type"));
    const requests = receiver.finish();
    try testing.expectEqualStrings("/direct", requests[0].path);
    try testing.expectEqualStrings("custom/type", requests[0].content_type);
    try testing.expectEqualStrings("a\x00b", requests[0].body);
}
