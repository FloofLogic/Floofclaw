const std = @import("std");
const model = @import("model.zig");

pub fn load(allocator: std.mem.Allocator, source: model.Source, config: model.Config) !model.LoadedInput {
    return switch (source) {
        .file => |path| loadFile(allocator, path, config.max_bytes),
        .stdin => loadStdin(allocator, config.max_bytes),
        .url => |url| fetchUrl(allocator, url, config),
    };
}

fn loadFile(allocator: std.mem.Allocator, path: []const u8, max_bytes: usize) !model.LoadedInput {
    const file = try std.fs.cwd().openFile(path, .{});
    defer file.close();
    const html = try readBounded(file, allocator, max_bytes);
    return .{
        .source_url = path,
        .final_url = path,
        .html = html,
    };
}

fn loadStdin(allocator: std.mem.Allocator, max_bytes: usize) !model.LoadedInput {
    const html = try readBounded(std.fs.File.stdin(), allocator, max_bytes);
    return .{
        .source_url = "stdin",
        .final_url = "stdin",
        .html = html,
    };
}

pub fn fetchUrl(allocator: std.mem.Allocator, url: []const u8, config: model.Config) !model.LoadedInput {
    const nonce = std.crypto.random.int(u64);
    const body_path = try std.fmt.allocPrint(allocator, "/tmp/pluck-body-{x}.html", .{nonce});
    const header_path = try std.fmt.allocPrint(allocator, "/tmp/pluck-headers-{x}.txt", .{nonce});
    defer std.fs.deleteFileAbsolute(body_path) catch {};
    defer std.fs.deleteFileAbsolute(header_path) catch {};

    const timeout_s = @as(f64, @floatFromInt(config.timeout_ms)) / 1000.0;
    const timeout_arg = try std.fmt.allocPrint(allocator, "{d:.3}", .{timeout_s});
    const write_out = "%{url_effective}\n%{content_type}";

    const argv = [_][]const u8{
        "curl",
        "--location",
        "--compressed",
        "--silent",
        "--show-error",
        "--fail",
        "--max-time",
        timeout_arg,
        "--user-agent",
        config.user_agent,
        "--dump-header",
        header_path,
        "--output",
        body_path,
        "--write-out",
        write_out,
        url,
    };

    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &argv,
        .max_output_bytes = 32 * 1024,
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    switch (result.term) {
        .Exited => |code| {
            if (code != 0) {
                std.log.err("curl failed: {s}", .{result.stderr});
                return error.FetchFailed;
            }
        },
        else => {
            std.log.err("curl terminated unexpectedly", .{});
            return error.FetchFailed;
        },
    }

    const body_file = try std.fs.openFileAbsolute(body_path, .{});
    defer body_file.close();
    const html = try readBounded(body_file, allocator, config.max_bytes);

    const header_file = try std.fs.openFileAbsolute(header_path, .{});
    defer header_file.close();
    const headers = try header_file.readToEndAlloc(allocator, 64 * 1024);

    var lines = std.mem.splitScalar(u8, std.mem.trim(u8, result.stdout, "\r\n "), '\n');
    const final_url = allocator.dupe(u8, lines.next() orelse url) catch return error.OutOfMemory;
    const content_type_line = lines.next() orelse "";

    return .{
        .source_url = try allocator.dupe(u8, url),
        .final_url = final_url,
        .html = html,
        .content_type = if (content_type_line.len == 0) null else try allocator.dupe(u8, content_type_line),
        .charset = parseCharset(allocator, headers, content_type_line),
    };
}

fn readBounded(file: std.fs.File, allocator: std.mem.Allocator, max_bytes: usize) ![]u8 {
    return file.readToEndAlloc(allocator, max_bytes) catch |err| switch (err) {
        error.FileTooBig => error.InputTooLarge,
        else => err,
    };
}

fn parseCharset(allocator: std.mem.Allocator, headers: []const u8, content_type: []const u8) ?[]const u8 {
    if (extractCharset(allocator, content_type)) |value| return value;
    var it = std.mem.splitScalar(u8, headers, '\n');
    while (it.next()) |line| {
        if (std.ascii.indexOfIgnoreCase(line, "content-type:") != null) {
            if (extractCharset(allocator, line)) |value| return value;
        }
    }
    return null;
}

fn extractCharset(allocator: std.mem.Allocator, line: []const u8) ?[]const u8 {
    const marker = "charset=";
    const start = std.ascii.indexOfIgnoreCase(line, marker) orelse return null;
    var rest = line[start + marker.len ..];
    rest = std.mem.trim(u8, rest, "\"' \r\n\t");
    const end = std.mem.indexOfAny(u8, rest, "; \r\n\t") orelse rest.len;
    if (end == 0) return null;
    return allocator.dupe(u8, rest[0..end]) catch null;
}
