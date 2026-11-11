const std = @import("std");

pub const Entry = struct {
    timestamp: u64 = 0,
    nanos: u64 = 0,
    has_timestamp: bool = false,
    line: []const u8 = "",
};

pub const Stream = struct {
    labels: []const u8 = "",
    entries: []const Entry = &.{},
};

const Field = struct {
    number: u64,
    value: u64 = 0,
    bytes: ?[]const u8 = null,
};

const Fields = struct {
    data: []const u8,

    fn varint(self: *Fields) !u64 {
        var value: u64 = 0;
        var shift: u7 = 0;
        while (true) {
            if (self.data.len == 0 or shift >= 64) return error.Malformed;
            const byte = self.data[0];
            self.data = self.data[1..];
            value |= @as(u64, byte & 0x7f) << @intCast(shift);
            if (byte & 0x80 == 0) return value;
            shift += 7;
        }
    }

    fn next(self: *Fields) !?Field {
        if (self.data.len == 0) return null;
        const key = try self.varint();
        switch (key & 7) {
            0 => return .{ .number = key >> 3, .value = try self.varint() },
            2 => {
                const size: usize = @intCast(try self.varint());
                if (size > self.data.len) return error.Malformed;
                const bytes = self.data[0..size];
                self.data = self.data[size..];
                return .{ .number = key >> 3, .bytes = bytes };
            },
            else => return error.Malformed,
        }
    }
};

fn littleEndian(data: *[]const u8, count: usize) !usize {
    if (data.len < count) return error.Malformed;
    var value: usize = 0;
    var index = count;
    while (index > 0) {
        index -= 1;
        value = value << 8 | data.*[index];
    }
    data.* = data.*[count..];
    return value;
}

pub fn unsnappy(arena: std.mem.Allocator, body: []const u8) ![]u8 {
    var header: Fields = .{ .data = body };
    const total: usize = @intCast(try header.varint());
    var data = header.data;
    const output = try arena.alloc(u8, total);
    var written: usize = 0;
    while (data.len > 0) {
        const tag = data[0];
        data = data[1..];
        const upper: usize = tag >> 2;
        var length: usize = undefined;
        var offset: usize = undefined;
        switch (@as(u2, @truncate(tag))) {
            0 => {
                length = if (upper < 60) upper + 1 else try littleEndian(&data, upper - 59) + 1;
                if (length > data.len or written + length > total) return error.Malformed;
                @memcpy(output[written..][0..length], data[0..length]);
                written += length;
                data = data[length..];
                continue;
            },
            1 => {
                length = 4 + (upper & 7);
                offset = @as(usize, tag >> 5) << 8 | try littleEndian(&data, 1);
            },
            2 => {
                length = upper + 1;
                offset = try littleEndian(&data, 2);
            },
            3 => {
                length = upper + 1;
                offset = try littleEndian(&data, 4);
            },
        }
        if (offset == 0 or offset > written or written + length > total) return error.Malformed;
        for (0..length) |_| {
            output[written] = output[written - offset];
            written += 1;
        }
    }
    if (written != total) return error.Malformed;
    return output;
}

pub fn decode(arena: std.mem.Allocator, body: []const u8) ![]const Stream {
    var streams: std.ArrayList(Stream) = .empty;
    var request: Fields = .{ .data = try unsnappy(arena, body) };
    while (try request.next()) |stream_field| {
        if (stream_field.number != 1) return error.Malformed;
        var stream: Stream = .{};
        var entries: std.ArrayList(Entry) = .empty;
        var stream_fields: Fields = .{ .data = stream_field.bytes orelse return error.Malformed };
        while (try stream_fields.next()) |field| {
            const bytes = field.bytes orelse return error.Malformed;
            switch (field.number) {
                1 => stream.labels = bytes,
                2 => {
                    var entry: Entry = .{};
                    var entry_fields: Fields = .{ .data = bytes };
                    while (try entry_fields.next()) |entry_field| {
                        const entry_bytes = entry_field.bytes orelse return error.Malformed;
                        switch (entry_field.number) {
                            1 => {
                                entry.has_timestamp = true;
                                var seconds: u64 = 0;
                                var timestamp_fields: Fields = .{ .data = entry_bytes };
                                while (try timestamp_fields.next()) |timestamp_field| {
                                    if (timestamp_field.bytes != null) return error.Malformed;
                                    switch (timestamp_field.number) {
                                        1 => seconds = timestamp_field.value,
                                        2 => entry.nanos = timestamp_field.value,
                                        else => return error.Malformed,
                                    }
                                }
                                entry.timestamp = seconds * 1_000_000_000 + entry.nanos;
                            },
                            2 => entry.line = entry_bytes,
                            else => return error.Malformed,
                        }
                    }
                    try entries.append(arena, entry);
                },
                else => return error.Malformed,
            }
        }
        stream.entries = entries.items;
        try streams.append(arena, stream);
    }
    return streams.items;
}
