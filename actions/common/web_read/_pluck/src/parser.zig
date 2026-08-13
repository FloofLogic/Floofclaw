const std = @import("std");

pub const c = @cImport({
    @cInclude("libxml/HTMLparser.h");
    @cInclude("libxml/tree.h");
    @cInclude("libxml/xmlstring.h");
});

pub const Node = [*c]c.xmlNode;

pub const Document = struct {
    raw: [*c]c.xmlDoc,
    root: Node,

    pub fn deinit(self: Document) void {
        c.xmlFreeDoc(self.raw);
    }
};

pub fn parse(html: []const u8, base_url: ?[]const u8) !Document {
    const url_ptr = if (base_url) |value| value.ptr else null;
    const options = c.HTML_PARSE_RECOVER | c.HTML_PARSE_NOERROR | c.HTML_PARSE_NOWARNING | c.HTML_PARSE_NONET;
    const raw = c.htmlReadMemory(
        html.ptr,
        @as(c_int, @intCast(html.len)),
        url_ptr,
        null,
        options,
    ) orelse return error.ParseFailed;

    const root = c.xmlDocGetRootElement(raw) orelse {
        c.xmlFreeDoc(raw);
        return error.ParseFailed;
    };

    return .{ .raw = raw, .root = root };
}

pub fn firstElementChild(node: Node) ?Node {
    var cursor = node.*.children;
    while (cursor != null) : (cursor = cursor.?.*.next) {
        if (cursor.?.*.type == c.XML_ELEMENT_NODE) return cursor.?;
    }
    return null;
}

pub fn nextElementSibling(node: Node) ?Node {
    var cursor = node.*.next;
    while (cursor != null) : (cursor = cursor.?.*.next) {
        if (cursor.?.*.type == c.XML_ELEMENT_NODE) return cursor.?;
    }
    return null;
}

pub fn parentElement(node: Node) ?Node {
    const parent = node.*.parent orelse return null;
    if (parent.*.type != c.XML_ELEMENT_NODE) return null;
    return parent;
}

pub fn isElement(node: Node) bool {
    return node.*.type == c.XML_ELEMENT_NODE;
}

pub fn isText(node: Node) bool {
    return node.*.type == c.XML_TEXT_NODE or node.*.type == c.XML_CDATA_SECTION_NODE;
}

pub fn name(node: Node) []const u8 {
    const ptr: [*:0]const u8 = @ptrCast(node.*.name);
    return std.mem.span(ptr);
}

pub fn attr(allocator: std.mem.Allocator, node: Node, key: []const u8) ?[]const u8 {
    var key_buffer = std.ArrayList(u8).empty;
    defer key_buffer.deinit(allocator);
    key_buffer.appendSlice(allocator, key) catch return null;
    key_buffer.append(allocator, 0) catch return null;
    const raw = c.xmlGetProp(node, @ptrCast(key_buffer.items.ptr)) orelse return null;
    defer c.xmlFree.?(raw);
    const ptr: [*:0]const u8 = @ptrCast(raw);
    return allocator.dupe(u8, std.mem.span(ptr)) catch null;
}

pub fn nodeText(allocator: std.mem.Allocator, node: Node) ![]const u8 {
    const raw = c.xmlNodeGetContent(node) orelse return allocator.dupe(u8, "");
    defer c.xmlFree.?(raw);
    const ptr: [*:0]const u8 = @ptrCast(raw);
    return allocator.dupe(u8, std.mem.span(ptr));
}
