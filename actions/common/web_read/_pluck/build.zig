const std = @import("std");

fn macosSdkInclude(b: *std.Build) []const u8 {
    if (std.process.getEnvVarOwned(b.allocator, "MACOS_SDK_PATH")) |env| {
        const trimmed = std.mem.trimRight(u8, env, "/\n ");
        return b.fmt("{s}/usr/include", .{trimmed});
    } else |_| {}

    const result = std.process.Child.run(.{
        .allocator = b.allocator,
        .argv = &.{ "xcrun", "--show-sdk-path" },
    }) catch {
        return "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include";
    };
    if (result.term != .Exited or result.term.Exited != 0) {
        return "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include";
    }
    const trimmed = std.mem.trimRight(u8, result.stdout, "\n");
    return b.fmt("{s}/usr/include", .{trimmed});
}

fn linkPluckDeps(b: *std.Build, compile: *std.Build.Step.Compile, target: std.Build.ResolvedTarget) void {
    if (target.result.os.tag == .macos) {
        compile.root_module.addSystemIncludePath(.{ .cwd_relative = macosSdkInclude(b) });
        compile.linkSystemLibrary("icucore");
    }
    compile.linkSystemLibrary("xml2");
    compile.linkSystemLibrary("z");
    compile.linkSystemLibrary("pthread");
    compile.linkSystemLibrary("m");
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe = b.addExecutable(.{
        .name = "pluck",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    linkPluckDeps(b, exe, target);
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    if (b.args) |args| run_cmd.addArgs(args);
    const run_step = b.step("run", "Run pluck");
    run_step.dependOn(&run_cmd.step);

    const exe_unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    linkPluckDeps(b, exe_unit_tests, target);

    const run_unit_tests = b.addRunArtifact(exe_unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_unit_tests.step);
}
