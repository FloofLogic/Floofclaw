const std = @import("std");

pub const Source = union(enum) {
    url: []const u8,
    file: []const u8,
    stdin: void,
};

pub const Config = struct {
    emit_json: bool = false,
    render: bool = false,
    timeout_ms: u32 = 15_000,
    max_bytes: usize = 8 * 1024 * 1024,
    user_agent: []const u8 = "pluck/0.1",
};

pub const LoadedInput = struct {
    source_url: []const u8,
    final_url: []const u8,
    html: []const u8,
    rendered: bool = false,
    content_type: ?[]const u8 = null,
    charset: ?[]const u8 = null,
};

pub const Extracted = struct {
    title: []const u8,
    byline: ?[]const u8,
    site_name: ?[]const u8,
    url: []const u8,
    excerpt: ?[]const u8,
    text_markdown: []const u8,
    text_plain: []const u8,
    length: usize,
    rendered: bool,
};

pub const CliError = error{
    InvalidArguments,
    MissingInput,
    UnsupportedRenderInput,
    FetchFailed,
    RendererFailed,
    ParseFailed,
    ChallengePage,
    NoExtractableContent,
    InputTooLarge,
};

pub fn fallbackTitle(url: []const u8) []const u8 {
    return if (url.len == 0) "Untitled" else url;
}
