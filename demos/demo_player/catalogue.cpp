#include "catalogue.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace prism::music {
namespace {
using Json = nlohmann::json;

void Fields(const Json &value, std::initializer_list<std::string_view> names)
{
    if (!value.is_object() || value.size() != names.size()) {
        throw std::runtime_error("Catalogue fields do not match their schema");
    }
    for (const auto &name : names) {
        if (!value.contains(std::string(name))) {
            throw std::runtime_error("Catalogue is missing a required field");
        }
    }
}

std::string Text(const Json &value, std::size_t maximum)
{
    if (!value.is_string()) {
        throw std::runtime_error("Catalogue text must be a string");
    }
    auto text = value.get<std::string>();
    if (text.empty() || text.size() > maximum || text.find('\0') != std::string::npos) {
        throw std::runtime_error("Catalogue text is empty or exceeds its bound");
    }
    return text;
}

std::uint32_t Integer(const Json &value, std::uint32_t maximum)
{
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw std::runtime_error("Catalogue integer is invalid");
    }
    const auto number = value.get<std::uint64_t>();
    if (number == 0 || number > maximum) {
        throw std::runtime_error("Catalogue integer is outside its range");
    }
    return static_cast<std::uint32_t>(number);
}

struct JsonBoundary {
    std::vector<std::set<std::string>> objects;
    std::size_t values{};

    bool operator()(int depth, Json::parse_event_t event, Json &value)
    {
        if (depth > 8 || ++values > 1024) {
            throw std::runtime_error("Catalogue JSON exceeds its structure bound");
        }
        if (event == Json::parse_event_t::object_start) {
            objects.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            if (objects.empty() || !objects.back().insert(value.get<std::string>()).second) {
                throw std::runtime_error("Catalogue JSON contains a duplicate key");
            }
        } else if (event == Json::parse_event_t::object_end) {
            objects.pop_back();
        }
        return true;
    }
};

Json Parse(std::string_view source)
{
    if (source.empty() || source.size() > MaxCatalogueBytes) {
        throw std::runtime_error("Catalogue JSON exceeds its byte bound");
    }
    return Json::parse(source.begin(), source.end(), JsonBoundary{});
}

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd)
    {
        if (fd_ < 0) {
            throw std::runtime_error("Cannot open catalogue asset");
        }
    }

    ~FileDescriptor()
    {
        close(fd_);
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    int Get() const noexcept
    {
        return fd_;
    }

private:
    int fd_;
};

bool Cancelled(const PrismWorkContextV1 &context)
{
    return context.is_cancelled(context.context) != 0;
}

std::string Read(const CatalogueRequest &request, const PrismWorkContextV1 &context)
{
    FileDescriptor directory(open(request.assets_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    FileDescriptor file(
        openat(directory.Get(), "catalog.json", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat before{};
    if (fstat(file.Get(), &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) > MaxCatalogueBytes) {
        throw std::runtime_error("Catalogue must be a regular file of at most 64 KiB");
    }

    std::string source;
    source.reserve(static_cast<std::size_t>(before.st_size));
    std::array<char, 4096> bytes{};
    while (!Cancelled(context)) {
        const auto count = read(file.Get(), bytes.data(), bytes.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Cannot read catalogue asset");
        }
        if (static_cast<std::size_t>(count) > MaxCatalogueBytes - source.size()) {
            throw std::runtime_error("Catalogue asset exceeds 64 KiB");
        }
        source.append(bytes.data(), static_cast<std::size_t>(count));
    }
    struct stat after{};
    if (fstat(file.Get(), &after) != 0 ||
        source.size() != static_cast<std::size_t>(before.st_size) ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        throw std::runtime_error("Catalogue asset changed while being read");
    }
    return source;
}
} // namespace

void to_json(Json &json, const Track &value)
{
    json = Json{{"id", value.id},
                {"title", value.title},
                {"artist", value.artist},
                {"duration_seconds", value.duration_seconds},
                {"favorite", value.favorite}};
}

void from_json(const Json &json, Track &value)
{
    Fields(json, {"id", "title", "artist", "duration_seconds", "favorite"});
    value.id = Text(json.at("id"), 64);
    for (const auto c : value.id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            throw std::runtime_error("Catalogue track ID contains an invalid character");
        }
    }
    value.title = Text(json.at("title"), 96);
    value.artist = Text(json.at("artist"), 96);
    value.duration_seconds = Integer(json.at("duration_seconds"), 7200);
    if (!json.at("favorite").is_boolean()) {
        throw std::runtime_error("Catalogue favorite must be a boolean");
    }
    value.favorite = json.at("favorite").get<bool>();
}

void to_json(Json &json, const Catalogue &value)
{
    json = Json{{"version", value.version}, {"tracks", value.tracks}};
}

void from_json(const Json &json, Catalogue &value)
{
    Fields(json, {"version", "tracks"});
    value.version = Integer(json.at("version"), 1);
    const auto &tracks = json.at("tracks");
    if (!tracks.is_array() || tracks.empty() || tracks.size() > MaxTracks) {
        throw std::runtime_error("Catalogue must contain one to three tracks");
    }
    value.tracks = tracks.get<std::vector<Track>>();
    std::set<std::string> ids;
    for (const auto &track : value.tracks) {
        if (!ids.insert(track.id).second) {
            throw std::runtime_error("Catalogue track IDs must be unique");
        }
    }
}

void to_json(Json &json, const CatalogueRequest &value)
{
    json = Json{{"assets_root", value.assets_root}};
}

void from_json(const Json &json, CatalogueRequest &value)
{
    Fields(json, {"assets_root"});
    value.assets_root = Text(json.at("assets_root"), 4096);
    if (value.assets_root.front() != '/') {
        throw std::runtime_error("Catalogue assets directory must be absolute");
    }
}

void to_json(Json &json, const CatalogueResult &value)
{
    json = value.ok ? Json{{"ok", true}, {"catalogue", value.catalogue}}
                    : Json{{"ok", false}, {"error", value.error}};
}

void from_json(const Json &json, CatalogueResult &value)
{
    if (!json.is_object() || !json.contains("ok") || !json.at("ok").is_boolean()) {
        throw std::runtime_error("Catalogue result has no valid status");
    }
    value.ok = json.at("ok").get<bool>();
    if (value.ok) {
        Fields(json, {"ok", "catalogue"});
        value.catalogue = json.at("catalogue").get<Catalogue>();
    } else {
        Fields(json, {"ok", "error"});
        value.error = Text(json.at("error"), 256);
    }
}

CatalogueResult DecodeCatalogueResult(std::string_view source)
{
    return Parse(source).get<CatalogueResult>();
}

std::int32_t PrepareCatalogue(const PrismWorkContextV1 *context, PrismBytesViewV1 input) noexcept
{
    if (!context || context->struct_size < sizeof(*context) || !context->is_cancelled ||
        !context->set_result || (!input.data && input.size)) {
        return -1;
    }
    try {
        CatalogueResult result;
        try {
            if (Cancelled(*context)) {
                return 1;
            }
            const std::string_view bytes(reinterpret_cast<const char *>(input.data), input.size);
            const auto request = Parse(bytes).get<CatalogueRequest>();
            const auto source = Read(request, *context);
            if (Cancelled(*context)) {
                return 1;
            }
            result.catalogue = Parse(source).get<Catalogue>();
            result.ok = true;
        } catch (const std::exception &error) {
            result.error = std::string(error.what()).substr(0, 256);
        }
        if (Cancelled(*context)) {
            return 1;
        }
        const auto encoded = Json(result).dump();
        if (Cancelled(*context)) {
            return 1;
        }
        return context->set_result(
            context->context,
            {reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size()});
    } catch (...) {
        return -1;
    }
}
} // namespace prism::music
