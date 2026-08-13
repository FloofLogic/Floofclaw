const std = @import("std");
const model = @import("model.zig");
const parser = @import("parser.zig");

const Candidate = struct {
    node: parser.Node,
    raw_score: f64,
    text_len: usize,
};

const LinkEntry = struct {
    text: []const u8,
    href: []const u8,
    score: i32,
};

pub fn extract(allocator: std.mem.Allocator, loaded: model.LoadedInput) !model.Extracted {
    const doc = try parser.parse(loaded.html, loaded.final_url);
    defer doc.deinit();

    const metadata = try extractMetadata(allocator, doc.root);

    var candidates = std.AutoHashMap(usize, Candidate).init(allocator);
    defer candidates.deinit();

    scoreDocument(allocator, doc.root, &candidates) catch return error.ParseFailed;
    const raw_best = chooseBest(&candidates) orelse (findFallback(doc.root) orelse return error.NoExtractableContent);
    const best = refineBestCandidate(allocator, raw_best);

    var markdown = std.ArrayList(u8).empty;
    defer markdown.deinit(allocator);
    var plain = std.ArrayList(u8).empty;
    defer plain.deinit(allocator);

    try renderSelection(allocator, best, &candidates, .markdown, &markdown);
    try renderSelection(allocator, best, &candidates, .plain, &plain);

    const lead = if (isHomepageLike(loaded.final_url))
        try findHomepageLead(allocator, doc.root, best)
    else
        null;

    const markdown_text = try materializeOutput(allocator, trimDocument(markdown.items), lead);
    const plain_text = try materializeOutput(allocator, trimDocument(plain.items), lead);
    const should_try_homepage_fallback = isHomepageLike(loaded.final_url) and
        (plain_text.len == 0 or homepageOutputLooksLowSignal(plain_text));
    if (plain_text.len == 0 and looksLikeChallengePage(loaded.html, metadata.title)) {
        return error.ChallengePage;
    }
    if (should_try_homepage_fallback) {
        if (try buildLinkIndexFallback(allocator, loaded, doc.root, metadata)) |fallback| return fallback;
    }
    if (plain_text.len == 0) {
        return error.NoExtractableContent;
    }

    const title = metadata.title orelse findHeadingTitle(allocator, best) orelse try allocator.dupe(u8, model.fallbackTitle(loaded.final_url));
    const excerpt = metadata.excerpt orelse buildExcerpt(allocator, plain_text);

    return .{
        .title = title,
        .byline = metadata.byline,
        .site_name = metadata.site_name,
        .url = loaded.final_url,
        .excerpt = excerpt,
        .text_markdown = markdown_text,
        .text_plain = plain_text,
        .length = plain_text.len,
        .rendered = loaded.rendered,
    };
}

pub fn qualityLooksPoor(extracted: model.Extracted) bool {
    return extracted.length < 240 or extracted.text_plain.len < 240;
}

fn homepageOutputLooksLowSignal(text: []const u8) bool {
    var non_empty_lines: usize = 0;
    var short_lines: usize = 0;
    var label_lines: usize = 0;
    var sentence_lines: usize = 0;
    var substantial_lines: usize = 0;

    var iterator = std.mem.splitScalar(u8, text, '\n');
    while (iterator.next()) |raw_line| {
        const line = trimMarkdownLine(raw_line);
        if (line.len == 0) continue;
        non_empty_lines += 1;
        if (line.len <= 22) short_lines += 1;
        if (looksLikeSectionLabel(line)) label_lines += 1;
        if (looksLikeSentenceLine(line)) sentence_lines += 1;
        if (line.len >= 45) substantial_lines += 1;
    }

    if (non_empty_lines < 6) return false;
    if (sentence_lines >= 2 or substantial_lines >= 3) return false;

    return short_lines * 100 >= non_empty_lines * 65 or
        label_lines * 100 >= non_empty_lines * 55;
}

fn trimMarkdownLine(line: []const u8) []const u8 {
    return std.mem.trimLeft(u8, std.mem.trim(u8, line, " \t\r"), "#-0123456789. ");
}

fn looksLikeSectionLabel(line: []const u8) bool {
    if (line.len == 0 or line.len > 28) return false;
    if (containsSentencePunctuation(line)) return false;
    return countWords(line) <= 3;
}

fn looksLikeSentenceLine(line: []const u8) bool {
    if (line.len >= 55 and countWords(line) >= 6) return true;
    if (!containsSentencePunctuation(line)) return false;
    return countWords(line) >= 4;
}

fn containsSentencePunctuation(text: []const u8) bool {
    return std.mem.indexOfAny(u8, text, ".?!:;") != null;
}

fn countWords(text: []const u8) usize {
    var count: usize = 0;
    var in_word = false;
    for (text) |byte| {
        const is_space = std.ascii.isWhitespace(byte);
        if (is_space) {
            in_word = false;
            continue;
        }
        if (!in_word) {
            count += 1;
            in_word = true;
        }
    }
    return count;
}

const RenderMode = enum { markdown, plain };

const Metadata = struct {
    title: ?[]const u8 = null,
    byline: ?[]const u8 = null,
    site_name: ?[]const u8 = null,
    excerpt: ?[]const u8 = null,
};

fn scoreDocument(allocator: std.mem.Allocator, root: parser.Node, candidates: *std.AutoHashMap(usize, Candidate)) !void {
    var stack = std.ArrayList(parser.Node).empty;
    defer stack.deinit(allocator);
    try stack.append(allocator, root);

    while (stack.pop()) |node| {
        var child = parser.firstElementChild(node);
        while (child) |current| : (child = parser.nextElementSibling(current)) {
            try stack.append(allocator, current);
        }

        if (!parser.isElement(node)) continue;
        if (isUnlikelyNode(allocator, node)) continue;
        if (!isScoredTag(parser.name(node))) continue;

        const scratch = std.heap.c_allocator;
        const text = try parser.nodeText(scratch, node);
        defer scratch.free(text);
        const normalized = try normalizeWhitespace(scratch, text);
        defer scratch.free(normalized);
        if (normalized.len < 25) continue;

        const parent = parser.parentElement(node) orelse continue;
        const grand = parser.parentElement(parent);

        const content_score = 1.0 + countRune(normalized, ',') + @min(@as(f64, @floatFromInt(normalized.len / 100)), 3.0);
        const parent_candidate = try ensureCandidate(allocator, candidates, parent);
        parent_candidate.raw_score += content_score;
        parent_candidate.text_len = @max(parent_candidate.text_len, normalized.len);

        if (grand) |grand_node| {
            const grand_candidate = try ensureCandidate(allocator, candidates, grand_node);
            grand_candidate.raw_score += content_score / 2.0;
            grand_candidate.text_len = @max(grand_candidate.text_len, normalized.len);
        }
    }
}

fn chooseBest(candidates: *std.AutoHashMap(usize, Candidate)) ?parser.Node {
    var iterator = candidates.iterator();
    var best_node: ?parser.Node = null;
    var best_score: f64 = -1_000_000.0;

    while (iterator.next()) |entry| {
        const adjusted = entry.value_ptr.raw_score * (1.0 - linkDensity(entry.value_ptr.node));
        if (adjusted > best_score) {
            best_score = adjusted;
            best_node = entry.value_ptr.node;
        }
    }

    return best_node;
}

fn renderSelection(
    allocator: std.mem.Allocator,
    best: parser.Node,
    candidates: *std.AutoHashMap(usize, Candidate),
    mode: RenderMode,
    output: *std.ArrayList(u8),
) !void {
    if (parser.parentElement(best)) |parent| {
        const best_key = @intFromPtr(best);
        const best_class = classForComparison(allocator, best);
        var child = parser.firstElementChild(parent);
        while (child) |current| : (child = parser.nextElementSibling(current)) {
            if (!parser.isElement(current)) continue;
            if (nodeLooksLikeBoilerplate(allocator, current)) continue;
            const include = if (@intFromPtr(current) == best_key)
                true
            else
                siblingShouldBeIncluded(allocator, current, best_class, candidates);

            if (include) try renderNode(allocator, current, mode, output, 0);
        }
        if (trimDocument(output.items).len != 0) return;
    }

    try renderNode(allocator, best, mode, output, 0);
}

fn siblingShouldBeIncluded(
    allocator: std.mem.Allocator,
    node: parser.Node,
    best_class: ?[]const u8,
    candidates: *std.AutoHashMap(usize, Candidate),
) bool {
    if (nodeLooksLikeBoilerplate(allocator, node)) return false;

    if (candidates.getPtr(@intFromPtr(node))) |candidate| {
        const adjusted = candidate.raw_score * (1.0 - linkDensity(node));
        if (adjusted >= 12.0) return true;
    }

    const scratch = std.heap.c_allocator;
    const text = parser.nodeText(scratch, node) catch return false;
    defer scratch.free(text);
    const normalized = normalizeWhitespace(scratch, text) catch return false;
    defer scratch.free(normalized);
    const density = linkDensity(node);
    if (normalized.len > 220 and density < 0.2) return true;

    if (best_class) |class_name| {
        if (classForComparison(allocator, node)) |node_class| {
            return std.mem.eql(u8, class_name, node_class);
        }
    }

    return false;
}

fn ensureCandidate(
    allocator: std.mem.Allocator,
    candidates: *std.AutoHashMap(usize, Candidate),
    node: parser.Node,
) !*Candidate {
    const key = @intFromPtr(node);
    const entry = try candidates.getOrPut(key);
    if (!entry.found_existing) {
        entry.value_ptr.* = .{
            .node = node,
            .raw_score = baseContentScore(allocator, node),
            .text_len = 0,
        };
    }
    return entry.value_ptr;
}

fn baseContentScore(allocator: std.mem.Allocator, node: parser.Node) f64 {
    const tag = parser.name(node);
    var score: f64 = switch (tag[0]) {
        'd' => if (std.mem.eql(u8, tag, "div")) 5 else 0,
        'p' => if (std.mem.eql(u8, tag, "pre")) 3 else 0,
        't' => if (std.mem.eql(u8, tag, "td")) 3 else if (std.mem.eql(u8, tag, "th")) -5 else 0,
        'b' => if (std.mem.eql(u8, tag, "blockquote")) 3 else 0,
        'a' => if (std.mem.eql(u8, tag, "address")) -3 else 0,
        'o' => if (std.mem.eql(u8, tag, "ol")) -3 else 0,
        'u' => if (std.mem.eql(u8, tag, "ul")) -3 else 0,
        'l' => if (std.mem.eql(u8, tag, "li")) -3 else 0,
        'f' => if (std.mem.eql(u8, tag, "form") or std.mem.eql(u8, tag, "footer")) -3 else 0,
        'h' => if (tag.len == 2 and tag[1] >= '1' and tag[1] <= '6') -5 else 0,
        else => 0,
    };

    score += classWeight(allocator, node);
    return score;
}

fn classWeight(allocator: std.mem.Allocator, node: parser.Node) f64 {
    var score: f64 = 0;
    if (parser.attr(allocator, node, "class")) |class_name| {
        score += keywordWeight(class_name);
    }
    if (parser.attr(allocator, node, "id")) |id_name| {
        score += keywordWeight(id_name);
    }
    return score;
}

fn keywordWeight(value: []const u8) f64 {
    const positive = [_][]const u8{ "article", "body", "content", "entry", "main", "page", "post", "story", "text", "blog" };
    const negative = [_][]const u8{
        "author",
        "comment",
        "combx",
        "contact",
        "footer",
        "foot",
        "latest",
        "masthead",
        "media",
        "meta",
        "modal",
        "nav",
        "newsletter",
        "promo",
        "recommended",
        "related",
        "scroll",
        "share",
        "sidebar",
        "social",
        "sponsor",
        "subscription",
        "tag",
        "trending",
        "utilitybar",
        "widget",
    };

    var score: f64 = 0;
    for (positive) |needle| {
        if (std.ascii.indexOfIgnoreCase(value, needle) != null) score += 5;
    }
    for (negative) |needle| {
        if (std.ascii.indexOfIgnoreCase(value, needle) != null) score -= 5;
    }
    return score;
}

fn isUnlikelyNode(allocator: std.mem.Allocator, node: parser.Node) bool {
    const positive = [_][]const u8{ "article", "body", "content", "entry", "main", "page", "post", "story" };
    const negative = [_][]const u8{
        "author",
        "banner",
        "comment",
        "community",
        "footer",
        "foot",
        "latest",
        "masthead",
        "menu",
        "meta",
        "nav",
        "newsletter",
        "promo",
        "recommended",
        "related",
        "remark",
        "rss",
        "share",
        "shoutbox",
        "sidebar",
        "social",
        "sponsor",
        "subscription",
        "toolbar",
        "widget",
    };

    var joined = std.ArrayList(u8).empty;
    defer joined.deinit(allocator);

    if (parser.attr(allocator, node, "class")) |class_name| {
        joined.appendSlice(allocator, class_name) catch return false;
    }
    if (parser.attr(allocator, node, "id")) |id_name| {
        if (joined.items.len != 0) joined.append(allocator, ' ') catch return false;
        joined.appendSlice(allocator, id_name) catch return false;
    }
    if (joined.items.len == 0) return false;

    for (positive) |needle| {
        if (std.ascii.indexOfIgnoreCase(joined.items, needle) != null) return false;
    }
    for (negative) |needle| {
        if (std.ascii.indexOfIgnoreCase(joined.items, needle) != null) return true;
    }
    return false;
}

fn isScoredTag(tag: []const u8) bool {
    return std.mem.eql(u8, tag, "p") or
        std.mem.eql(u8, tag, "pre") or
        std.mem.eql(u8, tag, "td") or
        std.mem.eql(u8, tag, "blockquote") or
        std.mem.eql(u8, tag, "div") or
        std.mem.eql(u8, tag, "article") or
        std.mem.eql(u8, tag, "section");
}

fn linkDensity(node: parser.Node) f64 {
    const total = textLength(node);
    if (total == 0) return 0;
    const linked = linkTextLength(node);
    return @as(f64, @floatFromInt(linked)) / @as(f64, @floatFromInt(total));
}

fn textLength(node: parser.Node) usize {
    const allocator = std.heap.c_allocator;
    const text = parser.nodeText(allocator, node) catch return 0;
    defer allocator.free(text);
    const normalized = normalizeWhitespace(allocator, text) catch return text.len;
    defer allocator.free(normalized);
    return normalized.len;
}

fn linkTextLength(root: parser.Node) usize {
    var total: usize = 0;
    var child_opt = parser.firstElementChild(root);
    while (child_opt) |child| : (child_opt = parser.nextElementSibling(child)) {
        if (std.mem.eql(u8, parser.name(child), "a")) {
            total += textLength(child);
        }
        total += linkTextLength(child);
    }
    return total;
}

fn renderNode(
    allocator: std.mem.Allocator,
    node: parser.Node,
    mode: RenderMode,
    output: *std.ArrayList(u8),
    list_depth: usize,
) !void {
    if (!parser.isElement(node)) return;
    const tag = parser.name(node);
    if (isSkippedTag(tag) or nodeLooksLikeBoilerplate(allocator, node)) return;

    if (std.mem.eql(u8, tag, "ul") or std.mem.eql(u8, tag, "ol")) {
        try ensureBlankLine(output, allocator);
        var child = parser.firstElementChild(node);
        var index: usize = 1;
        while (child) |item| : (child = parser.nextElementSibling(item)) {
            if (!std.mem.eql(u8, parser.name(item), "li")) continue;
            try renderListItem(allocator, item, mode, output, list_depth, if (std.mem.eql(u8, tag, "ol")) index else null);
            index += 1;
        }
        try ensureBlankLine(output, allocator);
        return;
    }

    if (isHeading(tag)) {
        const text = try inlineText(allocator, node);
        if (text.len == 0) return;
        try ensureBlankLine(output, allocator);
        if (mode == .markdown) {
            const level = @as(usize, tag[1] - '0');
            try output.appendNTimes(allocator, '#', level);
            try output.append(allocator, ' ');
        }
        try output.appendSlice(allocator, text);
        try output.appendSlice(allocator, "\n\n");
        return;
    }

    if (std.mem.eql(u8, tag, "pre")) {
        const text = try normalizeWhitespace(allocator, try parser.nodeText(allocator, node));
        if (text.len == 0) return;
        try ensureBlankLine(output, allocator);
        if (mode == .markdown) {
            try output.appendSlice(allocator, "```\n");
            try output.appendSlice(allocator, text);
            try output.appendSlice(allocator, "\n```\n\n");
        } else {
            try output.appendSlice(allocator, text);
            try output.appendSlice(allocator, "\n\n");
        }
        return;
    }

    if (isLeafBlock(node)) {
        const text = try inlineText(allocator, node);
        if (text.len == 0) return;
        try ensureBlankLine(output, allocator);
        try output.appendSlice(allocator, text);
        try output.appendSlice(allocator, "\n\n");
        return;
    }

    var child = parser.firstElementChild(node);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        try renderNode(allocator, current, mode, output, list_depth);
    }
}

fn renderListItem(
    allocator: std.mem.Allocator,
    node: parser.Node,
    mode: RenderMode,
    output: *std.ArrayList(u8),
    depth: usize,
    index: ?usize,
) !void {
    const text = try inlineText(allocator, node);
    if (text.len == 0) return;

    try output.appendNTimes(allocator, ' ', depth * 2);
    if (mode == .markdown) {
        if (index) |value| {
            try output.print(allocator, "{d}. ", .{value});
        } else {
            try output.appendSlice(allocator, "- ");
        }
    }
    try output.appendSlice(allocator, text);
    try output.append(allocator, '\n');
}

fn inlineText(allocator: std.mem.Allocator, node: parser.Node) ![]const u8 {
    var buffer = std.ArrayList(u8).empty;
    defer buffer.deinit(allocator);
    try collectInlineText(allocator, node, &buffer);
    return normalizeWhitespace(allocator, buffer.items);
}

fn collectInlineText(allocator: std.mem.Allocator, node: parser.Node, output: *std.ArrayList(u8)) !void {
    var cursor = node.*.children;
    while (cursor != null) : (cursor = cursor.?.*.next) {
        const current = cursor.?;
        if (parser.isText(current)) {
            const content = current.*.content orelse continue;
            const ptr: [*:0]const u8 = @ptrCast(content);
            try output.appendSlice(allocator, std.mem.span(ptr));
            try output.append(allocator, ' ');
            continue;
        }
        if (current.*.type != parser.c.XML_ELEMENT_NODE) continue;
        const tag = parser.name(current);
        if (isSkippedTag(tag)) continue;
        if (isBlockTag(tag) and !std.mem.eql(u8, tag, "a")) continue;
        try collectInlineText(allocator, current, output);
    }
}

fn isLeafBlock(node: parser.Node) bool {
    const tag = parser.name(node);
    if (!isBlockTag(tag)) return false;
    var child = parser.firstElementChild(node);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        const child_tag = parser.name(current);
        if (isBlockTag(child_tag) and !std.mem.eql(u8, child_tag, "a")) return false;
    }
    return true;
}

fn isBlockTag(tag: []const u8) bool {
    return std.mem.eql(u8, tag, "article") or
        std.mem.eql(u8, tag, "aside") or
        std.mem.eql(u8, tag, "blockquote") or
        std.mem.eql(u8, tag, "div") or
        std.mem.eql(u8, tag, "figure") or
        std.mem.eql(u8, tag, "footer") or
        isHeading(tag) or
        std.mem.eql(u8, tag, "li") or
        std.mem.eql(u8, tag, "main") or
        std.mem.eql(u8, tag, "ol") or
        std.mem.eql(u8, tag, "p") or
        std.mem.eql(u8, tag, "pre") or
        std.mem.eql(u8, tag, "section") or
        std.mem.eql(u8, tag, "table") or
        std.mem.eql(u8, tag, "td") or
        std.mem.eql(u8, tag, "ul");
}

fn isSkippedTag(tag: []const u8) bool {
    return std.mem.eql(u8, tag, "aside") or
        std.mem.eql(u8, tag, "button") or
        std.mem.eql(u8, tag, "footer") or
        std.mem.eql(u8, tag, "form") or
        std.mem.eql(u8, tag, "iframe") or
        std.mem.eql(u8, tag, "nav") or
        std.mem.eql(u8, tag, "noscript") or
        std.mem.eql(u8, tag, "script") or
        std.mem.eql(u8, tag, "style") or
        std.mem.eql(u8, tag, "svg") or
        std.mem.eql(u8, tag, "canvas");
}

fn isHeading(tag: []const u8) bool {
    return tag.len == 2 and tag[0] == 'h' and tag[1] >= '1' and tag[1] <= '6';
}

fn ensureBlankLine(output: *std.ArrayList(u8), allocator: std.mem.Allocator) !void {
    if (output.items.len == 0) return;
    const trimmed = std.mem.trimRight(u8, output.items, " \n\t");
    output.items.len = trimmed.len;
    if (!std.mem.endsWith(u8, output.items, "\n\n")) {
        if (!std.mem.endsWith(u8, output.items, "\n")) try output.append(allocator, '\n');
        try output.append(allocator, '\n');
    }
}

fn trimDocument(text: []const u8) []const u8 {
    return std.mem.trim(u8, text, " \n\t");
}

fn normalizeWhitespace(allocator: std.mem.Allocator, input: []const u8) ![]const u8 {
    var output = std.ArrayList(u8).empty;
    defer output.deinit(allocator);

    var saw_space = true;
    for (input) |byte| {
        const is_space = std.ascii.isWhitespace(byte);
        if (is_space) {
            if (!saw_space) try output.append(allocator, ' ');
            saw_space = true;
            continue;
        }
        saw_space = false;
        try output.append(allocator, byte);
    }
    return allocator.dupe(u8, std.mem.trim(u8, output.items, " "));
}

fn countRune(text: []const u8, needle: u8) f64 {
    var total: usize = 0;
    for (text) |byte| {
        if (byte == needle) total += 1;
    }
    return @as(f64, @floatFromInt(total));
}

fn classForComparison(allocator: std.mem.Allocator, node: parser.Node) ?[]const u8 {
    return parser.attr(allocator, node, "class");
}

fn refineBestCandidate(allocator: std.mem.Allocator, best: parser.Node) parser.Node {
    if (findPreferredDescendant(allocator, best)) |node| return node;
    return best;
}

fn findPreferredDescendant(allocator: std.mem.Allocator, root: parser.Node) ?parser.Node {
    var child = parser.firstElementChild(root);
    var best_match: ?parser.Node = null;
    var best_len: usize = 0;

    while (child) |current| : (child = parser.nextElementSibling(current)) {
        if (!parser.isElement(current)) continue;
        if (nodeHasPreferredArticleMarker(allocator, current) and !nodeLooksLikeBoilerplate(allocator, current)) {
            const len = textLength(current);
            if (len > best_len and len > 250) {
                best_len = len;
                best_match = current;
            }
        }
        if (findPreferredDescendant(allocator, current)) |nested| {
            const len = textLength(nested);
            if (len > best_len and len > 250) {
                best_len = len;
                best_match = nested;
            }
        }
    }

    return best_match;
}

fn nodeHasPreferredArticleMarker(allocator: std.mem.Allocator, node: parser.Node) bool {
    const markers = [_][]const u8{
        "article-body",
        "articlecontent",
        "article-content",
        "bodycopy",
        "body-copy",
        "entry-content",
        "post-content",
        "story-body",
        "text-copy",
    };

    if (parser.attr(allocator, node, "id")) |id_name| {
        for (markers) |needle| {
            if (std.ascii.indexOfIgnoreCase(id_name, needle) != null) return true;
        }
    }
    if (parser.attr(allocator, node, "class")) |class_name| {
        for (markers) |needle| {
            if (std.ascii.indexOfIgnoreCase(class_name, needle) != null) return true;
        }
    }
    return false;
}

fn nodeLooksLikeBoilerplate(allocator: std.mem.Allocator, node: parser.Node) bool {
    const negative_markers = [_][]const u8{
        "author",
        "comments",
        "latest",
        "newsletter",
        "promo",
        "recommended",
        "related",
        "share",
        "social",
        "subscription",
        "trending",
        "utilitybar",
    };
    const text_markers = [_][]const u8{
        "why subscribe?",
        "share this article",
        "join the conversation",
        "subscribe to our newsletter",
        "stay on the cutting edge",
        "contributing writer",
        "read more",
        "latest in ",
        "don't miss these",
        "add us as a preferred source",
        "comment from the forums",
    };

    if (parser.attr(allocator, node, "id")) |id_name| {
        for (negative_markers) |needle| {
            if (std.ascii.indexOfIgnoreCase(id_name, needle) != null) return true;
        }
    }
    if (parser.attr(allocator, node, "class")) |class_name| {
        for (negative_markers) |needle| {
            if (std.ascii.indexOfIgnoreCase(class_name, needle) != null) return true;
        }
    }

    const scratch = std.heap.c_allocator;
    const text = parser.nodeText(scratch, node) catch return false;
    defer scratch.free(text);
    const normalized = normalizeWhitespace(scratch, text) catch return false;
    defer scratch.free(normalized);
    if (normalized.len > 1200) return false;
    for (text_markers) |needle| {
        if (std.ascii.indexOfIgnoreCase(normalized, needle) != null) return true;
    }
    return false;
}

fn findFallback(root: parser.Node) ?parser.Node {
    var child = parser.firstElementChild(root);
    while (child) |node| : (child = parser.nextElementSibling(node)) {
        if (std.mem.eql(u8, parser.name(node), "body")) return node;
        if (findFallback(node)) |found| return found;
    }
    return null;
}

fn extractMetadata(allocator: std.mem.Allocator, root: parser.Node) !Metadata {
    return .{
        .title = findMeta(allocator, root, "property", "og:title") orelse findMeta(allocator, root, "name", "twitter:title") orelse findTitleTag(allocator, root),
        .byline = findAuthorName(allocator, root),
        .site_name = findMeta(allocator, root, "property", "og:site_name"),
        .excerpt = findMeta(allocator, root, "name", "description") orelse findMeta(allocator, root, "property", "og:description"),
    };
}

fn findAuthorName(allocator: std.mem.Allocator, root: parser.Node) ?[]const u8 {
    if (findMeta(allocator, root, "name", "author")) |author| {
        if (!looksLikeUrl(author)) return author;
    }
    if (findMeta(allocator, root, "property", "article:author")) |author| {
        if (!looksLikeUrl(author)) return author;
    }
    return findRelAuthorText(allocator, root);
}

fn findRelAuthorText(allocator: std.mem.Allocator, root: parser.Node) ?[]const u8 {
    var child = parser.firstElementChild(root);
    while (child) |node| : (child = parser.nextElementSibling(node)) {
        if (std.mem.eql(u8, parser.name(node), "a")) {
            if (parser.attr(allocator, node, "rel")) |rel_value| {
                if (std.ascii.indexOfIgnoreCase(rel_value, "author") != null) {
                    const text = inlineText(allocator, node) catch return null;
                    if (text.len != 0) return text;
                }
            }
        }
        if (findRelAuthorText(allocator, node)) |found| return found;
    }
    return null;
}

fn looksLikeUrl(text: []const u8) bool {
    return std.ascii.indexOfIgnoreCase(text, "http://") == 0 or std.ascii.indexOfIgnoreCase(text, "https://") == 0;
}

fn looksLikeChallengePage(html: []const u8, title: ?[]const u8) bool {
    const title_value = title orelse "";
    const title_markers = [_][]const u8{
        "please wait for verification",
        "verify you are human",
        "just a moment",
        "security check",
        "access denied",
        "captcha",
        "verification",
    };
    const html_markers = [_][]const u8{
        "js_challenge",
        "captcha",
        "cf-challenge",
        "please wait for verification",
        "verify you are human",
        "security check",
    };

    for (title_markers) |needle| {
        if (std.ascii.indexOfIgnoreCase(title_value, needle) != null) return true;
    }
    for (html_markers) |needle| {
        if (std.ascii.indexOfIgnoreCase(html, needle) != null) return true;
    }
    return false;
}

fn buildLinkIndexFallback(
    allocator: std.mem.Allocator,
    loaded: model.LoadedInput,
    root: parser.Node,
    metadata: Metadata,
) !?model.Extracted {
    if (!isHomepageLike(loaded.final_url)) return null;

    var entries = std.ArrayList(LinkEntry).empty;
    defer entries.deinit(allocator);
    var seen = std.StringHashMap(void).init(allocator);
    defer seen.deinit();

    try collectLinkEntries(allocator, root, &entries, &seen);
    if (entries.items.len < 5) return null;
    std.sort.heap(LinkEntry, entries.items, {}, compareLinkEntries);

    const chosen_title = metadata.title orelse metadata.site_name orelse model.fallbackTitle(loaded.final_url);

    var markdown = std.ArrayList(u8).empty;
    defer markdown.deinit(allocator);
    var plain = std.ArrayList(u8).empty;
    defer plain.deinit(allocator);

    try markdown.print(allocator, "# {s}\n\n", .{chosen_title});
    try plain.print(allocator, "{s}\n\n", .{chosen_title});

    const limit = @min(entries.items.len, 12);
    for (entries.items[0..limit], 0..) |entry, index| {
        try markdown.print(allocator, "{d}. {s}\n", .{ index + 1, entry.text });
        try plain.print(allocator, "{d}. {s}\n", .{ index + 1, entry.text });
    }

    const markdown_text = try allocator.dupe(u8, trimDocument(markdown.items));
    const plain_text = try allocator.dupe(u8, trimDocument(plain.items));
    return .{
        .title = try allocator.dupe(u8, chosen_title),
        .byline = null,
        .site_name = if (metadata.site_name) |site_name| try allocator.dupe(u8, site_name) else null,
        .url = loaded.final_url,
        .excerpt = metadata.excerpt orelse buildExcerpt(allocator, plain_text),
        .text_markdown = markdown_text,
        .text_plain = plain_text,
        .length = plain_text.len,
        .rendered = loaded.rendered,
    };
}

fn collectLinkEntries(
    allocator: std.mem.Allocator,
    root: parser.Node,
    entries: *std.ArrayList(LinkEntry),
    seen: *std.StringHashMap(void),
) !void {
    if (!parser.isElement(root)) return;
    if (nodeHasHomepageNoiseMarker(allocator, root) and !nodeHasHomepageHeadlineMarker(allocator, root)) {
        return;
    }

    if (std.mem.eql(u8, parser.name(root), "a")) {
        if (try linkEntryForNode(allocator, root, seen)) |entry| {
            try entries.append(allocator, entry);
        }
    }

    var child = parser.firstElementChild(root);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        try collectLinkEntries(allocator, current, entries, seen);
    }
}

fn linkEntryForNode(
    allocator: std.mem.Allocator,
    node: parser.Node,
    seen: *std.StringHashMap(void),
) !?LinkEntry {
    const href = parser.attr(allocator, node, "href") orelse return null;
    if (!looksLikeReadableHref(href)) return null;

    const text = try linkTextForNode(allocator, node);
    if (!looksLikeReadableLinkText(text)) return null;
    const score = scoreHomepageLink(allocator, node, text, href);
    if (score < 4) return null;
    if (seen.contains(text)) return null;

    try seen.put(text, {});
    return .{
        .text = text,
        .href = href,
        .score = score,
    };
}

fn linkTextForNode(allocator: std.mem.Allocator, node: parser.Node) ![]const u8 {
    const text = try inlineText(allocator, node);
    if (looksLikeReadableLinkText(text)) return text;
    if (parser.attr(allocator, node, "aria-label")) |label| {
        return normalizeWhitespace(allocator, label);
    }
    return text;
}

fn scoreHomepageLink(
    allocator: std.mem.Allocator,
    node: parser.Node,
    text: []const u8,
    href: []const u8,
) i32 {
    var score: i32 = 0;

    if (text.len >= 28) score += 4;
    if (text.len >= 48) score += 4;
    if (countWords(text) >= 5) score += 3;
    if (containsSentencePunctuation(text)) score += 1;
    if (looksLikeHeadlineText(text)) score += 3;
    if (looksLikeSectionLabel(text)) score -= 6;

    if (hrefLooksArticleLike(href)) score += 7;
    if (hrefLooksSectionLike(href)) score -= 6;

    if (nodeOrAncestorHasMarker(allocator, node, &.{
        "article-link",
        "article-name",
        "featured-articles",
        "feature-block",
        "headline",
        "listingresult",
        "listingresultswrapper",
        "story",
        "titleline",
        "top-featured",
    }, 4)) score += 8;

    if (nodeOrAncestorHasMarker(allocator, node, &.{
        "category-link",
        "meganav",
        "menu",
        "nav",
        "newsletter",
        "premium",
        "subscribe",
        "tag",
        "trending",
    }, 4)) score -= 10;

    if (nodeOrAncestorIsTag(node, "nav", 4) or
        nodeOrAncestorIsTag(node, "aside", 4) or
        nodeOrAncestorIsTag(node, "footer", 4))
    {
        score -= 10;
    }

    return score;
}

fn looksLikeReadableHref(href: []const u8) bool {
    if (href.len == 0) return false;
    if (href[0] == '#') return false;
    if (std.ascii.indexOfIgnoreCase(href, "javascript:") == 0) return false;
    if (std.ascii.indexOfIgnoreCase(href, "mailto:") == 0) return false;
    if (std.ascii.indexOfIgnoreCase(href, "from?site=") == 0) return false;
    if (std.ascii.indexOfIgnoreCase(href, "vote?") == 0) return false;
    if (std.ascii.indexOfIgnoreCase(href, "hide?") == 0) return false;
    return true;
}

fn looksLikeReadableLinkText(text: []const u8) bool {
    if (text.len < 15 or text.len > 180) return false;
    if (looksLikeBareDomain(text)) return false;

    const nav_phrases = [_][]const u8{
        "login",
        "submit",
        "comments",
        "new comments",
        "guidelines",
        "faq",
        "more",
        "apply to yc",
        "contact",
    };
    for (nav_phrases) |needle| {
        if (std.ascii.eqlIgnoreCase(text, needle)) return false;
    }
    return true;
}

fn looksLikeHeadlineText(text: []const u8) bool {
    return countWords(text) >= 4 and text.len >= 24;
}

fn looksLikeBareDomain(text: []const u8) bool {
    if (std.mem.indexOfScalar(u8, text, ' ') != null) return false;
    if (std.mem.indexOfScalar(u8, text, '.') == null) return false;
    return std.ascii.indexOfIgnoreCase(text, ".com") != null or
        std.ascii.indexOfIgnoreCase(text, ".net") != null or
        std.ascii.indexOfIgnoreCase(text, ".org") != null or
        std.ascii.indexOfIgnoreCase(text, ".io") != null or
        std.ascii.indexOfIgnoreCase(text, ".dev") != null or
        std.ascii.indexOfIgnoreCase(text, ".ai") != null or
        std.ascii.indexOfIgnoreCase(text, ".co") != null;
}

fn compareLinkEntries(_: void, lhs: LinkEntry, rhs: LinkEntry) bool {
    if (lhs.score == rhs.score) {
        if (lhs.text.len == rhs.text.len) return lhs.href.len > rhs.href.len;
        return lhs.text.len > rhs.text.len;
    }
    return lhs.score > rhs.score;
}

fn nodeHasHomepageHeadlineMarker(allocator: std.mem.Allocator, node: parser.Node) bool {
    return nodeHasMarker(allocator, node, &.{
        "article-link",
        "article-name",
        "feature-block",
        "featured-articles",
        "headline",
        "listingresult",
        "listingresultswrapper",
        "story",
        "titleline",
        "top-featured",
    });
}

fn nodeHasHomepageNoiseMarker(allocator: std.mem.Allocator, node: parser.Node) bool {
    if (std.mem.eql(u8, parser.name(node), "nav") or
        std.mem.eql(u8, parser.name(node), "aside") or
        std.mem.eql(u8, parser.name(node), "footer"))
    {
        return true;
    }

    return nodeHasMarker(allocator, node, &.{
        "auth",
        "breadcrumb",
        "category-link",
        "forum",
        "meganav",
        "menu",
        "newsletter",
        "premium",
        "subscribe",
        "tag",
        "toolbar",
        "trending",
    });
}

fn nodeOrAncestorHasMarker(
    allocator: std.mem.Allocator,
    node: parser.Node,
    markers: []const []const u8,
    max_hops: usize,
) bool {
    var current: ?parser.Node = node;
    var hops: usize = 0;
    while (current != null and hops <= max_hops) : (hops += 1) {
        if (nodeHasMarker(allocator, current.?, markers)) return true;
        current = parser.parentElement(current.?);
    }
    return false;
}

fn nodeHasMarker(
    allocator: std.mem.Allocator,
    node: parser.Node,
    markers: []const []const u8,
) bool {
    if (parser.attr(allocator, node, "class")) |class_name| {
        for (markers) |needle| {
            if (std.ascii.indexOfIgnoreCase(class_name, needle) != null) return true;
        }
    }
    if (parser.attr(allocator, node, "id")) |id_name| {
        for (markers) |needle| {
            if (std.ascii.indexOfIgnoreCase(id_name, needle) != null) return true;
        }
    }
    return false;
}

fn nodeOrAncestorIsTag(node: parser.Node, wanted: []const u8, max_hops: usize) bool {
    var current: ?parser.Node = node;
    var hops: usize = 0;
    while (current != null and hops <= max_hops) : (hops += 1) {
        if (std.mem.eql(u8, parser.name(current.?), wanted)) return true;
        current = parser.parentElement(current.?);
    }
    return false;
}

fn hrefLooksArticleLike(href: []const u8) bool {
    const slug = lastPathSegment(href);
    if (slug.len < 24) return false;
    return countByte(slug, '-') >= 3;
}

fn hrefLooksSectionLike(href: []const u8) bool {
    if (std.ascii.indexOfIgnoreCase(href, "/tag/") != null) return true;
    if (std.ascii.indexOfIgnoreCase(href, "/premium") != null) return true;
    if (std.ascii.indexOfIgnoreCase(href, "/forums") != null) return true;

    const slug = lastPathSegment(href);
    if (slug.len == 0) return false;
    return countPathSegments(href) <= 2 and slug.len < 24 and countByte(slug, '-') < 3;
}

fn lastPathSegment(href: []const u8) []const u8 {
    var trimmed = std.mem.trimRight(u8, href, "/");
    if (std.mem.lastIndexOfScalar(u8, trimmed, '/')) |index| {
        trimmed = trimmed[index + 1 ..];
    }
    if (std.mem.indexOfScalar(u8, trimmed, '?')) |index| {
        trimmed = trimmed[0..index];
    }
    if (std.mem.indexOfScalar(u8, trimmed, '#')) |index| {
        trimmed = trimmed[0..index];
    }
    return trimmed;
}

fn countPathSegments(href: []const u8) usize {
    var count: usize = 0;
    var in_segment = false;
    for (href) |byte| {
        if (byte == '?' or byte == '#') break;
        if (byte == '/') {
            in_segment = false;
            continue;
        }
        if (!in_segment) {
            count += 1;
            in_segment = true;
        }
    }
    return count;
}

fn countByte(text: []const u8, needle: u8) usize {
    var total: usize = 0;
    for (text) |byte| {
        if (byte == needle) total += 1;
    }
    return total;
}

fn materializeOutput(allocator: std.mem.Allocator, base: []const u8, lead: ?[]const u8) ![]const u8 {
    if (lead) |lead_text| {
        if (lead_text.len != 0 and std.mem.indexOf(u8, base, lead_text) == null) {
            return std.fmt.allocPrint(allocator, "{s}\n\n{s}", .{ lead_text, base });
        }
    }
    return allocator.dupe(u8, base);
}

fn isHomepageLike(url: []const u8) bool {
    const uri = std.Uri.parse(url) catch return false;
    if (!std.ascii.eqlIgnoreCase(uri.scheme, "http") and !std.ascii.eqlIgnoreCase(uri.scheme, "https")) return false;
    return uri.path.isEmpty() or std.mem.eql(u8, uri.path.toRawMaybeAlloc(std.heap.c_allocator) catch return false, "/");
}

fn findHomepageLead(allocator: std.mem.Allocator, root: parser.Node, best: parser.Node) !?[]const u8 {
    const body = findBody(root) orelse return null;
    const top_level_best = topLevelBodyAncestor(best, body) orelse return null;

    var child = parser.firstElementChild(body);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        if (@intFromPtr(current) == @intFromPtr(top_level_best)) break;
        if (try findLeadInSubtree(allocator, current)) |lead| return lead;
    }

    return null;
}

fn findBody(root: parser.Node) ?parser.Node {
    if (std.mem.eql(u8, parser.name(root), "body")) return root;
    var child = parser.firstElementChild(root);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        if (findBody(current)) |body| return body;
    }
    return null;
}

fn topLevelBodyAncestor(node: parser.Node, body: parser.Node) ?parser.Node {
    var current = node;
    var ancestor = current;
    while (parser.parentElement(current)) |parent| {
        if (@intFromPtr(parent) == @intFromPtr(body)) return ancestor;
        ancestor = parent;
        current = parent;
    }
    return null;
}

fn findLeadInSubtree(allocator: std.mem.Allocator, root: parser.Node) !?[]const u8 {
    if (std.mem.eql(u8, parser.name(root), "p")) {
        if (try qualifiesAsHomepageLead(allocator, root)) |lead| return lead;
    }

    var child = parser.firstElementChild(root);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        if (try findLeadInSubtree(allocator, current)) |lead| return lead;
    }

    return null;
}

fn qualifiesAsHomepageLead(allocator: std.mem.Allocator, node: parser.Node) !?[]const u8 {
    const text = try inlineText(allocator, node);
    if (text.len < 12 or text.len > 120) return null;
    if (linkDensity(node) > 0.15) return null;
    if (!looksLikeLeadContainer(allocator, node)) return null;
    if (looksLikeCallToAction(text)) return null;
    if (hasInteractiveDescendant(node)) return null;
    return text;
}

fn looksLikeLeadContainer(allocator: std.mem.Allocator, node: parser.Node) bool {
    const positive = [_][]const u8{ "hero", "intro", "lead", "tagline", "slogan" };

    var current: ?parser.Node = node;
    var hops: usize = 0;
    while (current != null and hops < 3) : (hops += 1) {
        const candidate = current.?;
        if (parser.attr(allocator, candidate, "class")) |class_name| {
            for (positive) |needle| {
                if (std.ascii.indexOfIgnoreCase(class_name, needle) != null) return true;
            }
        }
        if (parser.attr(allocator, candidate, "id")) |id_name| {
            for (positive) |needle| {
                if (std.ascii.indexOfIgnoreCase(id_name, needle) != null) return true;
            }
        }
        current = parser.parentElement(candidate);
    }

    const parent = parser.parentElement(node) orelse return false;
    return hasEmptyOrShortHeading(allocator, parent);
}

fn hasEmptyOrShortHeading(allocator: std.mem.Allocator, node: parser.Node) bool {
    var child = parser.firstElementChild(node);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        if (isHeading(parser.name(current))) {
            const text = inlineText(allocator, current) catch return false;
            return text.len <= 24;
        }
    }
    return false;
}

fn looksLikeCallToAction(text: []const u8) bool {
    const negative = [_][]const u8{
        "start your project",
        "start free",
        "free trial",
        "get started",
        "learn more",
        "book a demo",
        "request a demo",
        "contact us",
        "join the waitlist",
        "join waitlist",
        "subscribe",
        "sign up",
    };

    for (negative) |needle| {
        if (std.ascii.indexOfIgnoreCase(text, needle) != null) return true;
    }
    return false;
}

fn hasInteractiveDescendant(root: parser.Node) bool {
    var child = parser.firstElementChild(root);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        const tag = parser.name(current);
        if (std.mem.eql(u8, tag, "button") or
            std.mem.eql(u8, tag, "input") or
            std.mem.eql(u8, tag, "select") or
            std.mem.eql(u8, tag, "textarea") or
            std.mem.eql(u8, tag, "form"))
        {
            return true;
        }
        if (hasInteractiveDescendant(current)) return true;
    }
    return false;
}

fn findMeta(allocator: std.mem.Allocator, root: parser.Node, key: []const u8, wanted: []const u8) ?[]const u8 {
    var child = parser.firstElementChild(root);
    while (child) |node| : (child = parser.nextElementSibling(node)) {
        if (std.mem.eql(u8, parser.name(node), "meta")) {
            if (parser.attr(allocator, node, key)) |value| {
                if (std.ascii.eqlIgnoreCase(value, wanted)) {
                    if (parser.attr(allocator, node, "content")) |content| {
                        const trimmed = std.mem.trim(u8, content, " \t\r\n");
                        if (trimmed.len != 0) return allocator.dupe(u8, trimmed) catch null;
                    }
                }
            }
        }
        if (findMeta(allocator, node, key, wanted)) |found| return found;
    }
    return null;
}

fn findTitleTag(allocator: std.mem.Allocator, root: parser.Node) ?[]const u8 {
    var child = parser.firstElementChild(root);
    while (child) |node| : (child = parser.nextElementSibling(node)) {
        if (std.mem.eql(u8, parser.name(node), "title")) {
            const text = parser.nodeText(allocator, node) catch return null;
            const normalized = normalizeWhitespace(allocator, text) catch return null;
            if (normalized.len != 0) return normalized;
        }
        if (findTitleTag(allocator, node)) |found| return found;
    }
    return null;
}

fn findHeadingTitle(allocator: std.mem.Allocator, node: parser.Node) ?[]const u8 {
    var child = parser.firstElementChild(node);
    while (child) |current| : (child = parser.nextElementSibling(current)) {
        if (std.mem.eql(u8, parser.name(current), "h1")) {
            const text = inlineText(allocator, current) catch return null;
            if (text.len != 0) return text;
        }
        if (findHeadingTitle(allocator, current)) |found| return found;
    }
    return null;
}

fn buildExcerpt(allocator: std.mem.Allocator, text: []const u8) ?[]const u8 {
    if (text.len == 0) return null;
    const limit = @min(text.len, 180);
    return allocator.dupe(u8, text[0..limit]) catch null;
}

test "extracts article-like content" {
    const html =
        \\<html><head><title>Sample</title><meta name="author" content="Test Author"></head>
        \\<body><nav>Links links links</nav><article><h1>Sample</h1><p>This is the first real paragraph with enough substance to be scored well, and it should win.</p><p>Second paragraph with some extra useful detail for the extractor.</p></article></body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    const result = try extract(arena, .{
        .source_url = "test",
        .final_url = "https://example.com/post",
        .html = html,
    });

    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "first real paragraph") != null);
    try std.testing.expectEqualStrings("Sample", result.title);
    try std.testing.expectEqualStrings("Test Author", result.byline.?);
}

test "prepends homepage hero tagline once" {
    const html =
        \\<html><head><title>Floof Logic</title></head>
        \\<body>
        \\  <section class="hero"><div class="hero-text"><h1></h1><p>Good for no reason.</p><a href="#contact">Start Your Project</a></div></section>
        \\  <section class="projects"><article class="post-content"><h3>Barkimedes</h3><p>This is a longer project description with enough text to become the main extracted content block for the homepage.</p></article></section>
        \\</body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    const result = try extract(arena, .{
        .source_url = "test",
        .final_url = "https://example.com/",
        .html = html,
    });

    try std.testing.expect(std.mem.startsWith(u8, result.text_plain, "Good for no reason."));
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Good for no reason.\n\nGood for no reason.") == null);
}

test "does not prepend homepage CTA copy" {
    const html =
        \\<html><head><title>Example</title></head>
        \\<body>
        \\  <section class="hero"><div class="hero-text"><h1></h1><p>Start your project today.</p><a href="#contact">Get Started</a></div></section>
        \\  <section class="projects"><article class="post-content"><h3>Work</h3><p>This is the longer main content area with enough descriptive copy to win normal extraction scoring.</p></article></section>
        \\</body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    const result = try extract(arena, .{
        .source_url = "test",
        .final_url = "https://example.com/",
        .html = html,
    });

    try std.testing.expect(!std.mem.startsWith(u8, result.text_plain, "Start your project today."));
}

test "detects verification challenge pages" {
    const html =
        \\<html><head><title>Reddit - Please wait for verification</title></head>
        \\<body><main><div class="logo"></div></main><form hidden><input name="js_challenge" value="1"></form></body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    try std.testing.expectError(error.ChallengePage, extract(arena, .{
        .source_url = "test",
        .final_url = "https://www.reddit.com/",
        .html = html,
    }));
}

test "falls back to readable link list for homepage indexes" {
    const html =
        \\<html><head><title>Hacker News</title></head>
        \\<body>
        \\  <table id="hnmain">
        \\    <tr><td><a href="news">Hacker News</a> <a href="newest">new</a> <a href="front">past</a></td></tr>
        \\    <tr class="athing"><td class="title"><span class="titleline"><a href="https://example.com/1">I ported Mac OS X to the Nintendo Wii</a></span></td></tr>
        \\    <tr class="athing"><td class="title"><span class="titleline"><a href="https://example.com/2">USB for Software Developers: An introduction to writing userspace USB drivers</a></span></td></tr>
        \\    <tr class="athing"><td class="title"><span class="titleline"><a href="https://example.com/3">Git commands I run before reading any code</a></span></td></tr>
        \\    <tr class="athing"><td class="title"><span class="titleline"><a href="https://example.com/4">Understanding the Kalman filter with a simple radar example</a></span></td></tr>
        \\    <tr class="athing"><td class="title"><span class="titleline"><a href="https://example.com/5">Muse Spark: Scaling towards personal superintelligence</a></span></td></tr>
        \\    <tr class="athing"><td class="title"><span class="titleline"><a href="https://example.com/6">They're made out of meat</a></span></td></tr>
        \\  </table>
        \\</body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    const result = try extract(arena, .{
        .source_url = "test",
        .final_url = "https://news.ycombinator.com/",
        .html = html,
    });

    try std.testing.expect(std.mem.startsWith(u8, result.text_plain, "Hacker News"));
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "I ported Mac OS X to the Nintendo Wii") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "USB for Software Developers") != null);
}

test "filters article boilerplate and normalizes byline" {
    const html =
        \\<html><head>
        \\  <title>Article Title</title>
        \\  <meta property="og:title" content="Article Title">
        \\  <meta property="article:author" content="https://example.com/author/jane-doe">
        \\</head><body>
        \\  <article class="news-article">
        \\    <header><a rel="author" href="https://example.com/author/jane-doe">Jane Doe</a></header>
        \\    <section class="content-wrapper">
        \\      <div class="subscription-box"><h2>Why subscribe?</h2><p>Get deeper insights.</p></div>
        \\      <div id="article-body" class="text-copy bodyCopy auto">
        \\        <p>The real article begins here with substantial text and context about the topic being discussed in detail.</p>
        \\        <p>The second paragraph continues the article and should remain in the extracted output.</p>
        \\      </div>
        \\      <div class="newsletter-module"><h2>Stay On the Cutting Edge</h2><p>Subscribe to our newsletter.</p></div>
        \\      <div class="author-bio"><h2>Contributing Writer</h2><p>Jane Doe covers technology.</p></div>
        \\      <div class="latest-in"><h2>Latest in Big Tech</h2><p>Other unrelated stories.</p></div>
        \\    </section>
        \\  </article>
        \\</body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    const result = try extract(arena, .{
        .source_url = "test",
        .final_url = "https://example.com/story",
        .html = html,
    });

    try std.testing.expectEqualStrings("Jane Doe", result.byline.?);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "The real article begins here") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Why subscribe?") == null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Stay On the Cutting Edge") == null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Contributing Writer") == null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Latest in Big Tech") == null);
}

test "homepage low-signal output falls back to featured headlines" {
    const html =
        \\<html><head><title>Tom's Hardware</title></head>
        \\<body>
        \\  <nav class="meganav"><a class="meganav-item__link" href="/deals">Deals</a><a class="meganav-item__link" href="/premium">Premium</a><a class="meganav-item__link" href="/cpus">CPUs</a></nav>
        \\  <main>
        \\    <section id="featured-articles">
        \\      <div class="feature-block top-featured">
        \\        <a class="article-link" href="/tech-industry/big-tech/framework-founder-says-that-personal-computing-as-we-know-it-is-dead-vows-to-keep-building-computers-that-you-can-own-at-the-deepest-level" aria-label="Framework founder says that personal computing as we know it is dead"></a>
        \\      </div>
        \\      <div class="listingResultsWrapper">
        \\        <a class="article-link" href="/pc-components/liquid-cooling/silverstone-icemyst-pro-360-pro-review" aria-label="Silverstone IceMyst Pro 360 Pro Review: Designed for RAM overclocking"></a>
        \\        <a class="article-link" href="/software/windows/french-government-say-its-ditching-windows-for-linux-country-accelerates-plans-to-ditch-us-based-software-in-digital-sovereignty-push" aria-label="French government says it's ditching Windows for Linux"></a>
        \\        <a class="article-link" href="/tech-industry/cyber-security/hwmonitor-and-cpu-z-developer-cpuid-breached-by-unknown-attackers-cyberattack-forced-users-to-download-malware-instead-of-valid-apps-for-approximately-six-hours" aria-label="HWMonitor and CPU-Z developer CPUID breached by unknown attackers"></a>
        \\        <a class="article-link" href="/pc-components/ssds/vdura-sharply-revises-its-enterprise-ssd-pricing-figures" aria-label="Vdura hikes its enterprise SSD pricing, now costs 22.6x more than hard drives"></a>
        \\        <a class="article-link" href="/pc-components/gpus/intel-arc-gpus-can-finally-boot-up-and-play-crimson-desert-but-youll-probably-want-to-wait-for-official-support" aria-label="Intel Arc GPUs can finally boot up and play Crimson Desert"></a>
        \\      </div>
        \\    </section>
        \\  </main>
        \\</body></html>
    ;
    var arena_instance = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_instance.deinit();
    const arena = arena_instance.allocator();

    const result = try extract(arena, .{
        .source_url = "test",
        .final_url = "https://www.tomshardware.com/",
        .html = html,
    });

    try std.testing.expect(std.mem.startsWith(u8, result.text_plain, "Tom's Hardware"));
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Framework founder says that personal computing as we know it is dead") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Silverstone IceMyst Pro 360 Pro Review") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Deals") == null);
    try std.testing.expect(std.mem.indexOf(u8, result.text_plain, "Premium") == null);
}
