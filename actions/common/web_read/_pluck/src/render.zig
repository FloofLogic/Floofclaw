const std = @import("std");
const model = @import("model.zig");

pub fn renderUrl(allocator: std.mem.Allocator, url: []const u8, config: model.Config) !model.LoadedInput {
    const timeout_arg = try std.fmt.allocPrint(allocator, "{d}", .{config.timeout_ms});
    const argv = [_][]const u8{
        "node",
        "scripts/render_page.mjs",
        url,
        timeout_arg,
        config.user_agent,
    };

    const result = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &argv,
        .max_output_bytes = config.max_bytes * 4,
    }) catch |err| switch (err) {
        error.StdoutStreamTooLong, error.StderrStreamTooLong => return error.InputTooLarge,
        else => return err,
    };
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    switch (result.term) {
        .Exited => |code| {
            if (code != 0) {
                std.log.err("{s}", .{std.mem.trim(u8, result.stderr, "\r\n")});
                return error.RendererFailed;
            }
        },
        else => return error.RendererFailed,
    }

    const parsed = try std.json.parseFromSlice(std.json.Value, allocator, result.stdout, .{});
    defer parsed.deinit();

    const root = parsed.value.object;
    const html = root.get("html") orelse return error.RendererFailed;
    const final_url = root.get("finalUrl") orelse return error.RendererFailed;

    return .{
        .source_url = try allocator.dupe(u8, url),
        .final_url = try allocator.dupe(u8, final_url.string),
        .html = try allocator.dupe(u8, html.string),
        .rendered = true,
        .content_type = "text/html",
    };
}
