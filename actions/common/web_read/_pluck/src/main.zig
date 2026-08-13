const std = @import("std");
const cli = @import("cli.zig");
const input = @import("input.zig");
const extract = @import("extract.zig");
const model = @import("model.zig");
const render = @import("render.zig");

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa.allocator());
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const args = try std.process.argsAlloc(arena);
    const parsed = cli.parseArgs(arena, args) catch |err| {
        switch (err) {
            error.InvalidArguments, error.MissingInput, error.UnsupportedRenderInput => {
                std.log.err("{s}", .{cli.usage()});
                return err;
            },
            else => return err,
        }
    };

    var loaded = input.load(arena, parsed.source, parsed.config) catch |err| switch (err) {
        error.InputTooLarge => exitWithUsageError(parsed.config.max_bytes),
        else => return err,
    };
    var result = extract.extract(arena, loaded) catch |err| switch (err) {
        error.ChallengePage => exitWithChallengePage(parsed.source),
        error.NoExtractableContent => exitWithNoExtractableContent(parsed.source),
        else => return err,
    };

    if (parsed.config.render and parsed.source == .url and extract.qualityLooksPoor(result)) {
        loaded = render.renderUrl(arena, parsed.source.url, parsed.config) catch |err| switch (err) {
            error.InputTooLarge => exitWithUsageError(parsed.config.max_bytes),
            else => return err,
        };
        result = extract.extract(arena, loaded) catch |err| switch (err) {
            error.ChallengePage => exitWithChallengePage(parsed.source),
            error.NoExtractableContent => exitWithNoExtractableContent(parsed.source),
            else => return err,
        };
    }

    var stdout_buffer: [4096]u8 = undefined;
    var stdout = std.fs.File.stdout().writer(&stdout_buffer);
    if (parsed.config.emit_json) {
        try writeJson(&stdout.interface, result);
    } else {
        try stdout.interface.writeAll(result.text_markdown);
        try stdout.interface.writeAll("\n");
    }
    try stdout.interface.flush();
}

fn exitWithUsageError(max_bytes: usize) noreturn {
    std.log.err(
        "input exceeded --max-bytes limit ({d} bytes). Retry with `--max-bytes <n>` for larger pages.",
        .{max_bytes},
    );
    std.process.exit(1);
}

fn exitWithChallengePage(source: model.Source) noreturn {
    switch (source) {
        .url => |url| std.log.err(
            "site returned a verification or challenge page instead of readable content: {s}",
            .{url},
        ),
        else => std.log.err("input looks like a verification or challenge page, not readable content", .{}),
    }
    std.process.exit(1);
}

fn exitWithNoExtractableContent(source: model.Source) noreturn {
    switch (source) {
        .url => |url| std.log.err(
            "could not extract readable content from: {s}",
            .{url},
        ),
        else => std.log.err("could not extract readable content from input", .{}),
    }
    std.process.exit(1);
}

fn writeJson(writer: *std.Io.Writer, result: model.Extracted) !void {
    const payload = .{
        .title = result.title,
        .byline = result.byline,
        .site_name = result.site_name,
        .url = result.url,
        .excerpt = result.excerpt,
        .text_markdown = result.text_markdown,
        .text_plain = result.text_plain,
        .length = result.length,
        .rendered = result.rendered,
    };
    try std.json.Stringify.value(payload, .{ .whitespace = .indent_2 }, writer);
    try writer.writeAll("\n");
}

test {
    std.testing.refAllDecls(@This());
}
