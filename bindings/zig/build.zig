const std = @import("std");

fn link(b: *std.Build, module: *std.Build.Module, arguments: []const []const u8) void {
    var framework = false;
    for (arguments) |argument| {
        if (framework) {
            framework = false;
            module.linkFramework(argument, .{});
        } else if (std.mem.eql(u8, argument, "-framework")) {
            framework = true;
        } else if (std.mem.cutPrefix(u8, argument, "-Wl,-framework,")) |name| {
            module.linkFramework(name, .{});
        } else if (std.mem.cutPrefix(u8, argument, "-Wl,-rpath,")) |directory| {
            module.addRPath(.{ .cwd_relative = directory });
        } else if (std.mem.cutPrefix(u8, argument, "-L")) |directory| {
            module.addLibraryPath(.{ .cwd_relative = directory });
        } else if (std.mem.cutPrefix(u8, argument, "-F")) |directory| {
            module.addFrameworkPath(.{ .cwd_relative = directory });
        } else if (std.mem.cutPrefix(u8, argument, "-l")) |name| {
            module.linkSystemLibrary(name, .{});
        } else if (!std.mem.startsWith(u8, argument, "-")) {
            module.addObjectFile(.{ .cwd_relative = argument });
        } else if (!std.mem.eql(u8, argument, "-pthread")) {
            std.debug.panic("unsupported lokiverse link argument: {s}", .{argument});
        }
    }
    _ = b;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const arguments = b.option(
        []const []const u8,
        "lokiverse-link",
        "Link argument from the lokiverse_c.link file written by the CMake build; repeat per line",
    ) orelse &.{};

    const module = b.addModule("lokiverse", .{
        .root_source_file = b.path("src/lokiverse.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    link(b, module, arguments);

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/tests.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "lokiverse", .module = module }},
        }),
    });
    const step = b.step("test", "Run the binding tests");
    step.dependOn(&b.addRunArtifact(tests).step);
}
