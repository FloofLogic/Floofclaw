const std = @import("std");
const model = @import("model.zig");

pub const Parsed = struct {
    source: model.Source,
    config: model.Config,
};

pub fn parseArgs(allocator: std.mem.Allocator, args: []const []const u8) !Parsed {
    _ = allocator;

    var config = model.Config{};
    var source: ?model.Source = null;

    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        const arg = args[i];
        if (std.mem.eql(u8, arg, "--json")) {
            config.emit_json = true;
            continue;
        }
        if (std.mem.eql(u8, arg, "--render")) {
            config.render = true;
            continue;
        }
        if (std.mem.eql(u8, arg, "--stdin")) {
            source = .{ .stdin = {} };
            continue;
        }
        if (std.mem.eql(u8, arg, "--file")) {
            i += 1;
            if (i >= args.len) return error.InvalidArguments;
            source = .{ .file = args[i] };
            continue;
        }
        if (std.mem.eql(u8, arg, "--timeout")) {
            i += 1;
            if (i >= args.len) return error.InvalidArguments;
            config.timeout_ms = try std.fmt.parseInt(u32, args[i], 10);
            continue;
        }
        if (std.mem.eql(u8, arg, "--max-bytes")) {
            i += 1;
            if (i >= args.len) return error.InvalidArguments;
            config.max_bytes = try std.fmt.parseInt(usize, args[i], 10);
            continue;
        }
        if (std.mem.eql(u8, arg, "--user-agent")) {
            i += 1;
            if (i >= args.len) return error.InvalidArguments;
            config.user_agent = args[i];
            continue;
        }
        if (std.mem.eql(u8, arg, "--help") or std.mem.eql(u8, arg, "-h")) {
            return error.InvalidArguments;
        }
        if (std.mem.startsWith(u8, arg, "--")) {
            return error.InvalidArguments;
        }

        source = .{ .url = arg };
    }

    const resolved_source = source orelse return error.MissingInput;
    if (config.render and resolved_source != .url) return error.UnsupportedRenderInput;

    return .{
        .source = resolved_source,
        .config = config,
    };
}

pub fn usage() []const u8 {
    return
        \\Usage:
        \\  pluck <url>
        \\  pluck --file <path>
        \\  pluck --stdin
        \\
        \\Flags:
        \\  --json
        \\  --render
        \\  --timeout <ms>
        \\  --max-bytes <n>
        \\  --user-agent <value>
        \\
    ;
}

test "parse URL args" {
    const args = [_][]const u8{ "pluck", "--json", "https://example.com" };
    const parsed = try parseArgs(std.testing.allocator, &args);
    try std.testing.expect(parsed.config.emit_json);
    try std.testing.expect(parsed.source == .url);
}

test "render only allowed for urls" {
    const args = [_][]const u8{ "pluck", "--render", "--file", "x.html" };
    try std.testing.expectError(error.UnsupportedRenderInput, parseArgs(std.testing.allocator, &args));
}
