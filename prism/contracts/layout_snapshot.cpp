#include "prism/contracts/layout_snapshot.hpp"
#include <bit>
#include <stdexcept>

namespace prism::contracts {
namespace {
void Require(bool valid)
{
    if (!valid) {
        throw std::invalid_argument("Invalid WM layout payload");
    }
}

struct Writer {
    std::vector<std::uint8_t> bytes;

    void U(std::uint64_t value, unsigned size)
    {
        for (unsigned i = size; i; --i) {
            bytes.push_back(value >> ((i - 1) * 8));
        }
    }

    void Number(double value)
    {
        U(std::bit_cast<std::uint64_t>(value), 8);
    }

    void Text(const std::string &text)
    {
        U(text.size(), 2);
        bytes.insert(bytes.end(), text.begin(), text.end());
    }

    void Rect(const LogicalRect &rect)
    {
        Number(rect.x);
        Number(rect.y);
        Number(rect.width);
        Number(rect.height);
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};

    std::uint64_t U(unsigned size)
    {
        Require(size <= bytes.size() - at);
        std::uint64_t value{};
        while (size--) {
            value = (value << 8) | bytes[at++];
        }
        return value;
    }

    double Number()
    {
        return std::bit_cast<double>(U(8));
    }

    bool Flag()
    {
        const auto value = U(1);
        Require(value <= 1);
        return value;
    }

    std::size_t Count(std::size_t maximum)
    {
        const auto value = U(2);
        Require(value <= maximum);
        return value;
    }

    std::string Text()
    {
        const auto size = Count(128);
        Require(size <= bytes.size() - at);
        std::string result(bytes.begin() + at, bytes.begin() + at + size);
        at += size;
        return result;
    }

    LogicalRect Rect()
    {
        return {Number(), Number(), Number(), Number()};
    }
};

void WriteOutput(Writer &writer, const LayoutOutput &output)
{
    writer.U(output.id, 8);
    writer.Text(output.name);
    writer.Rect(output.logical_bounds);
    writer.Number(output.scale);
    writer.U(output.primary, 1);
    writer.U(output.supported, 1);
}

LayoutOutput ReadOutput(Reader &reader)
{
    LayoutOutput output;
    output.id = reader.U(8);
    output.name = reader.Text();
    output.logical_bounds = reader.Rect();
    output.scale = reader.Number();
    output.primary = reader.Flag();
    output.supported = reader.Flag();
    return output;
}

void WriteWorkspace(Writer &writer, const LayoutWorkspace &workspace)
{
    writer.U(workspace.id, 8);
    writer.U(workspace.root, 8);
    writer.U(workspace.output, 8);
    writer.Text(workspace.name);
    writer.U(workspace.active, 1);
    writer.U(static_cast<unsigned>(workspace.mode), 1);
    writer.U(workspace.mode_revision, 8);
}

LayoutWorkspace ReadWorkspace(Reader &reader)
{
    LayoutWorkspace workspace;
    workspace.id = reader.U(8);
    workspace.root = reader.U(8);
    workspace.output = reader.U(8);
    workspace.name = reader.Text();
    workspace.active = reader.Flag();
    workspace.mode = static_cast<LayoutGroupMode>(reader.U(1));
    workspace.mode_revision = reader.U(8);
    return workspace;
}

void WriteNode(Writer &writer, const LayoutNode &node)
{
    writer.U(node.id, 8);
    writer.U(node.parent, 8);
    writer.U(node.workspace, 8);
    writer.U(static_cast<unsigned>(node.kind), 1);
    writer.U(static_cast<unsigned>(node.layout), 1);
    writer.U(node.children.size(), 2);
    for (const auto child : node.children) {
        writer.U(child, 8);
    }
    writer.Rect(node.tile_bounds);
    writer.Rect(node.target_bounds);
    writer.Rect(node.committed_bounds);
    writer.Number(node.width_fraction);
    writer.Number(node.height_fraction);
    writer.U(node.instance.value, 8);
    writer.U(node.focused, 1);
    writer.U(node.visible, 1);
    writer.U(node.fullscreen, 1);
    writer.U(node.has_committed, 1);
}

LayoutNode ReadNode(Reader &reader, std::size_t &total_children)
{
    LayoutNode node;
    node.id = reader.U(8);
    node.parent = reader.U(8);
    node.workspace = reader.U(8);
    node.kind = static_cast<LayoutNodeKind>(reader.U(1));
    node.layout = static_cast<LayoutArrangement>(reader.U(1));
    const auto count = reader.Count(kMaxLayoutNodes - total_children);
    total_children += count;
    for (std::size_t i = 0; i < count; ++i) {
        node.children.push_back(reader.U(8));
    }
    node.tile_bounds = reader.Rect();
    node.target_bounds = reader.Rect();
    node.committed_bounds = reader.Rect();
    node.width_fraction = reader.Number();
    node.height_fraction = reader.Number();
    node.instance = {reader.U(8)};
    node.focused = reader.Flag();
    node.visible = reader.Flag();
    node.fullscreen = reader.Flag();
    node.has_committed = reader.Flag();
    return node;
}

void WriteBoundary(Writer &writer, const LayoutBoundary &boundary)
{
    writer.U(boundary.id, 8);
    writer.U(boundary.parent, 8);
    writer.U(boundary.first, 8);
    writer.U(boundary.second, 8);
    writer.U(boundary.workspace, 8);
    writer.U(static_cast<unsigned>(boundary.axis), 1);
    writer.Rect(boundary.bounds);
    writer.U(boundary.visible, 1);
    writer.U(boundary.resizable, 1);
}

LayoutBoundary ReadBoundary(Reader &reader)
{
    LayoutBoundary boundary;
    boundary.id = reader.U(8);
    boundary.parent = reader.U(8);
    boundary.first = reader.U(8);
    boundary.second = reader.U(8);
    boundary.workspace = reader.U(8);
    boundary.axis = static_cast<LayoutBoundaryAxis>(reader.U(1));
    boundary.bounds = reader.Rect();
    boundary.visible = reader.Flag();
    boundary.resizable = reader.Flag();
    return boundary;
}
} // namespace

std::vector<std::uint8_t> EncodeLayoutSnapshot(const LayoutSnapshot &snapshot)
{
    ValidateLayoutSnapshot(snapshot);

    Writer writer;
    writer.U(kLayoutSnapshotVersion, 2);
    writer.U(snapshot.session, 8);
    writer.U(snapshot.revision, 8);
    writer.U(snapshot.topology_revision, 8);
    writer.U(snapshot.layout_revision, 8);
    writer.U(snapshot.focus_revision, 8);
    writer.U(snapshot.active_instance.value, 8);
    writer.U(snapshot.outputs.size(), 2);
    writer.U(snapshot.workspaces.size(), 2);
    writer.U(snapshot.nodes.size(), 2);
    writer.U(snapshot.boundaries.size(), 2);
    for (const auto &output : snapshot.outputs) {
        WriteOutput(writer, output);
    }
    for (const auto &workspace : snapshot.workspaces) {
        WriteWorkspace(writer, workspace);
    }
    for (const auto &node : snapshot.nodes) {
        WriteNode(writer, node);
    }
    for (const auto &boundary : snapshot.boundaries) {
        WriteBoundary(writer, boundary);
    }
    Require(writer.bytes.size() <= kMaxLayoutSnapshotPayload);
    return std::move(writer.bytes);
}

LayoutSnapshot DecodeLayoutSnapshot(std::span<const std::uint8_t> bytes)
{
    Require(bytes.size() <= kMaxLayoutSnapshotPayload);
    Reader reader{bytes};
    Require(reader.U(2) == kLayoutSnapshotVersion);

    LayoutSnapshot snapshot;
    snapshot.session = reader.U(8);
    snapshot.revision = reader.U(8);
    snapshot.topology_revision = reader.U(8);
    snapshot.layout_revision = reader.U(8);
    snapshot.focus_revision = reader.U(8);
    snapshot.active_instance = {reader.U(8)};
    const auto outputs = reader.Count(kMaxLayoutOutputs);
    const auto workspaces = reader.Count(kMaxLayoutWorkspaces);
    const auto nodes = reader.Count(kMaxLayoutNodes);
    const auto boundaries = reader.Count(kMaxLayoutBoundaries);
    for (std::size_t i = 0; i < outputs; ++i) {
        snapshot.outputs.push_back(ReadOutput(reader));
    }
    for (std::size_t i = 0; i < workspaces; ++i) {
        snapshot.workspaces.push_back(ReadWorkspace(reader));
    }
    std::size_t total_children{};
    for (std::size_t i = 0; i < nodes; ++i) {
        snapshot.nodes.push_back(ReadNode(reader, total_children));
    }
    for (std::size_t i = 0; i < boundaries; ++i) {
        snapshot.boundaries.push_back(ReadBoundary(reader));
    }
    Require(reader.at == bytes.size());
    ValidateLayoutSnapshot(snapshot);
    return snapshot;
}
} // namespace prism::contracts
