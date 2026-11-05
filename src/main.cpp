#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <cerrno>
#include <cstdint>
#include <json/json.h>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>

struct FileLocation {
    lt::sha1_hash hash;
    lt::file_index_t index;
};

struct TorrentEntry {
    libtorrent::sha1_hash hash;
    libtorrent::torrent_handle handle;
    std::shared_ptr<libtorrent::torrent_info> info;
};

struct ActiveDownload {
    std::filesystem::path path;
    std::int64_t size;
    lt::torrent_handle handle;
    std::int64_t fileOffset;
    std::int64_t pieceLength;
};

using TorrentList = std::vector<TorrentEntry>;
using TorrentsMap = std::unordered_map<std::string, FileLocation>;
using ActiveFiles = std::unordered_map<std::string, size_t>;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
constexpr auto pieceWaitTimeout = std::chrono::minutes(15);
const std::filesystem::path torrentsDir { "torrents" };
const std::filesystem::path downloadDir { "downloads" };
const std::filesystem::path torrentMapPath { "torrent_map.json" };

struct Config {
    unsigned short httpPort;
    unsigned short torrentPort;
    std::string trustedProxyAddress;
    std::uint64_t maxUsedSpaceMb;
};
Config config;

std::mutex logMutex;

void logEvent(const char* level, const std::string& event, const std::string& details = { })
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime { };
    localtime_r(&time, &localTime);

    std::lock_guard<std::mutex> lock(logMutex);
    std::cout << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
              << " [" << level << "] " << event;
    if (!details.empty())
        std::cout << " " << details;
    std::cout << std::endl;
}

Config loadConfig()
{
    std::ifstream file("config.json", std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("cannot open config.json");

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    if (!Json::parseFromStream(builder, file, &root, &errors))
        throw std::runtime_error("invalid config file: " + errors);
    if (!root.isObject())
        throw std::runtime_error("invalid config file: top level is not an object");

    const char* required[] = {
        "http_port", "torrent_port", "trusted_proxy_address", "max_used_space_mb"
    };
    for (const char* key : required) {
        if (!root.isMember(key))
            throw std::runtime_error("invalid config file: missing " + std::string(key));
    }
    if (!root["trusted_proxy_address"].isString()
        || !root["http_port"].isUInt() || !root["torrent_port"].isUInt()
        || !root["max_used_space_mb"].isUInt64())
        throw std::runtime_error("invalid config file: incorrect value type");

    const auto httpPort = root["http_port"].asUInt();
    const auto torrentPort = root["torrent_port"].asUInt();
    if (httpPort == 0 || httpPort > 65535 || torrentPort == 0 || torrentPort > 65535)
        throw std::runtime_error("invalid config file: port outside range 1-65535");
    if (root["max_used_space_mb"].asUInt64() > UINT64_MAX / (1024 * 1024))
        throw std::runtime_error("invalid config file: max_used_space_mb is too large");

    boost::system::error_code addressError;
    boost::asio::ip::make_address(root["trusted_proxy_address"].asString(), addressError);
    if (addressError)
        throw std::runtime_error("invalid config file: trusted_proxy_address is not an IP address");

    return {
        static_cast<unsigned short>(httpPort),
        static_cast<unsigned short>(torrentPort),
        root["trusted_proxy_address"].asString(),
        root["max_used_space_mb"].asUInt64()
    };
}

void initializeStorage()
{
    std::filesystem::create_directories(torrentsDir);
    std::filesystem::create_directories(downloadDir);
}

lt::torrent_handle getTorrentHandle(TorrentList& torrents, const lt::sha1_hash& infoHash)
{
    auto it = std::lower_bound(torrents.begin(), torrents.end(), infoHash,
        [](const TorrentEntry& element, const lt::sha1_hash& hash) {
            return element.hash < hash;
        });

    // Found
    if (it != torrents.end() && it->hash == infoHash)
        return it->handle;

    // Invalid
    return lt::torrent_handle { };
}

bool loadTorrentsMap(TorrentsMap& map)
{
    std::ifstream file(torrentMapPath, std::ios::binary);
    if (!file.is_open()) {
        logEvent("INFO", "torrent_map_missing", "path=" + torrentMapPath.string());
        return false;
    }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;

    if (!Json::parseFromStream(builder, file, &root, &errs)) {
        logEvent("ERROR", "torrent_map_invalid", errs);
        return false;
    }
    if (!root.isObject()) {
        logEvent("ERROR", "torrent_map_invalid", "reason=top_level_not_object");
        return false;
    }

    for (const auto& key : root.getMemberNames()) {
        const Json::Value& entry = root[key];
        if (!entry.isArray() || entry.size() != 2 || !entry[0].isString()
            || !entry[1].isInt())
            continue;

        std::stringstream ss(entry[0].asString());
        libtorrent::sha1_hash hash;
        ss >> hash;
        if (!ss.fail())
            map[key] = { hash, entry[1].asInt() };
    }

    logEvent("INFO", "torrent_map_loaded", "entries=" + std::to_string(map.size()));
    return true;
}

bool startDownloadByName(TorrentsMap& torrentsMap, TorrentList& torrents, const std::string& name)
{
    auto it = torrentsMap.find(name);
    if (it == torrentsMap.end())
        return false;

    const auto& loc = it->second;

    lt::torrent_handle handle = getTorrentHandle(torrents, loc.hash);

    if (handle.is_valid())
        handle.file_priority(loc.index, lt::default_priority);

    return true;
}

void initSettingsPack(lt::settings_pack& sp)
{
    sp = lt::settings_pack { };
    sp.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:" + std::to_string(config.torrentPort) + ",[::]:" + std::to_string(config.torrentPort));
    sp.set_int(lt::settings_pack::disk_io_write_mode, lt::settings_pack::enable_os_cache);
    sp.set_int(lt::settings_pack::disk_io_read_mode, lt::settings_pack::enable_os_cache);
    sp.set_int(lt::settings_pack::suggest_mode, lt::settings_pack::no_piece_suggestions);
    sp.set_bool(lt::settings_pack::prioritize_partial_pieces, true);
    sp.set_int(lt::settings_pack::aio_threads, 4);
    sp.set_int(lt::settings_pack::cache_size, 64 * 1024);
    sp.set_int(lt::settings_pack::max_queued_disk_bytes, 16 * 1024 * 1024);
    sp.set_int(lt::settings_pack::connections_limit, 1024);
    sp.set_int(lt::settings_pack::active_downloads, -1);
    sp.set_int(lt::settings_pack::active_seeds, 0);
    sp.set_int(lt::settings_pack::active_dht_limit, -1);
    sp.set_bool(lt::settings_pack::announce_to_all_tiers, true);
    sp.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    sp.set_int(lt::settings_pack::tracker_completion_timeout, 10);
    sp.set_int(lt::settings_pack::tracker_receive_timeout, 5);
    sp.set_int(lt::settings_pack::peer_connect_timeout, 5);
    sp.set_int(lt::settings_pack::handshake_timeout, 5);
    sp.set_int(lt::settings_pack::connection_speed, 100);
    sp.set_int(lt::settings_pack::active_checking, 4);
    sp.set_int(lt::settings_pack::listen_queue_size, 1024);
    sp.set_int(lt::settings_pack::choking_algorithm, lt::settings_pack::fixed_slots_choker);
    sp.set_int(lt::settings_pack::seed_choking_algorithm, lt::settings_pack::round_robin);
    sp.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp);
    sp.set_bool(lt::settings_pack::seeding_outgoing_connections, false);
    sp.set_bool(lt::settings_pack::enable_upnp, false);
    sp.set_bool(lt::settings_pack::enable_natpmp, false);
    sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::error);
}

lt::torrent_handle addTorrent(lt::session& s,
    const std::shared_ptr<lt::torrent_info>& ti)
{
    lt::add_torrent_params atp;
    atp.ti = ti;
    atp.save_path = downloadDir;
    atp.storage_mode = lt::storage_mode_sparse;
    atp.flags |= lt::torrent_flags::paused;
    atp.flags &= ~lt::torrent_flags::auto_managed;
    atp.max_connections = 96;
    atp.max_uploads = 8;
    atp.file_priorities.assign(ti->num_files(), lt::dont_download);
    return s.add_torrent(std::move(atp));
}

void loadTorrents(TorrentList& torrents, lt::session& s)
{
    for (const auto& entry : std::filesystem::directory_iterator(torrentsDir)) {
        if (!entry.is_regular_file())
            continue;

        std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
        if (!file.is_open())
            continue;

        std::streamsize size = file.tellg();
        if (size <= 0 || size > 32 * 1024 * 1024)
            continue;

        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(static_cast<size_t>(size));
        if (!file.read(buffer.data(), size))
            continue;

        std::shared_ptr<lt::torrent_info> ti;
        try {
            ti = std::make_shared<lt::torrent_info>(
                lt::span<char const>(buffer.data(), buffer.size()), lt::from_span);
        } catch (const std::exception& error) {
            logEvent("ERROR", "torrent_skipped",
                "path=" + entry.path().string() + " reason=" + error.what());
            continue;
        }

        lt::torrent_handle handle = addTorrent(s, ti);
        auto infoHash = ti->info_hash();
        torrents.push_back({ infoHash, handle, ti });
    }

    std::sort(torrents.begin(), torrents.end(),
        [](const TorrentEntry& a, const TorrentEntry& b) {
            return a.hash < b.hash;
        });

    logEvent("INFO", "torrents_loaded", "count=" + std::to_string(torrents.size()));
}

void startTorrentWorker(lt::session& s, std::atomic_bool& shuttingDown)
{
    while (!shuttingDown) {
        std::vector<lt::alert*> alerts;
        s.pop_alerts(&alerts);

        for (lt::alert const* a : alerts) {
            if (lt::alert_cast<lt::torrent_error_alert>(a))
                logEvent("ERROR", "torrent_error", a->message());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::string urlDecode(std::string_view value)
{
    std::string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            result.push_back(value[i]);
            continue;
        }

        if (i + 2 >= value.size())
            return { };

        unsigned int byte = 0;
        std::istringstream hex(std::string(value.substr(i + 1, 2)));
        hex >> std::hex >> byte;
        if (hex.fail())
            return { };

        result.push_back(static_cast<char>(byte));
        i += 2;
    }

    return result;
}

std::string safeHeaderFilename(std::string value)
{
    for (char& character : value) {
        if (static_cast<unsigned char>(character) < 0x20
            || static_cast<unsigned char>(character) == 0x7f
            || character == '"' || character == '\\')
            character = '_';
    }
    return value;
}

bool fileIsComplete(const lt::torrent_handle& handle, const lt::file_storage& files,
    lt::file_index_t index)
{
    const auto fileSize = files.file_size(index);
    if (fileSize <= 0)
        return false;
    const auto firstPiece = static_cast<lt::piece_index_t>(
        files.file_offset(index) / files.piece_length());
    const auto lastPiece = static_cast<lt::piece_index_t>(
        (files.file_offset(index) + fileSize - 1) / files.piece_length());
    for (auto piece = firstPiece; piece <= lastPiece; ++piece) {
        if (!handle.have_piece(piece))
            return false;
    }
    return true;
}

bool torrentHasActiveFile(const lt::sha1_hash& hash,
    const TorrentsMap& torrentsMap, const ActiveFiles& activeFiles)
{
    for (const auto& activeFile : activeFiles) {
        auto mapped = torrentsMap.find(activeFile.first);
        if (mapped != torrentsMap.end() && mapped->second.hash == hash)
            return true;
    }
    return false;
}

bool makeRoom(std::uint64_t requiredBytes, const lt::sha1_hash& requestedHash,
    TorrentsMap& torrentsMap, TorrentList& torrents, ActiveFiles& activeFiles,
    lt::session& session)
{
    const std::uint64_t limit = config.maxUsedSpaceMb * 1024 * 1024;
    if (requiredBytes > limit)
        return false;

    std::uint64_t used = 0;
    for (const auto& torrent : torrents) {
        if (!torrent.handle.is_valid())
            continue;
        const auto done = torrent.handle.status().total_done;
        if (done > 0)
            used += static_cast<std::uint64_t>(done);
    }

    // Evict whole inactive torrents so the requested file has room to download
    while (used > limit || requiredBytes > limit - used) {
        auto candidate = torrents.end();
        std::uint64_t candidateBytes = 0;
        for (auto it = torrents.begin(); it != torrents.end(); ++it) {
            if (it->hash == requestedHash || !it->handle.is_valid()
                || torrentHasActiveFile(it->hash, torrentsMap, activeFiles))
                continue;
            const auto done = it->handle.status().total_done;
            if (done > 0) {
                candidate = it;
                candidateBytes = static_cast<std::uint64_t>(done);
                break;
            }
        }
        if (candidate == torrents.end())
            return false;

        const auto hash = candidate->hash;
        const auto info = candidate->info;
        session.remove_torrent(candidate->handle, lt::session::delete_files);
        torrents.erase(candidate);
        used = candidateBytes >= used ? 0 : used - candidateBytes;
        auto handle = addTorrent(session, info);
        torrents.push_back({ hash, handle, info });
        std::sort(torrents.begin(), torrents.end(),
            [](const TorrentEntry& a, const TorrentEntry& b) {
                return a.hash < b.hash;
            });
        logEvent("INFO", "download_evicted", "bytes=" + std::to_string(candidateBytes));
    }
    return true;
}

bool getFileDetails(TorrentsMap& torrentsMap, TorrentList& torrents,
    const std::string& name, std::filesystem::path& path, std::int64_t& size,
    ActiveFiles& activeFiles,
    std::mutex& activationMutex, lt::session& session, ActiveDownload& download)
{
    std::lock_guard<std::mutex> lock(activationMutex);
    auto it = torrentsMap.find(name);
    if (it == torrentsMap.end())
        return false;

    const auto& location = it->second;
    lt::torrent_handle handle = getTorrentHandle(torrents, location.hash);
    if (!handle.is_valid())
        return false;

    auto torrentFile = handle.torrent_file();
    if (!torrentFile || location.index < lt::file_index_t(0)
        || location.index >= torrentFile->files().num_files())
        return false;

    const auto& files = torrentFile->files();
    const std::int64_t fileOffset = files.file_offset(location.index);
    const std::int64_t fileSize = files.file_size(location.index);
    const std::int64_t pieceLength = files.piece_length();
    if (fileSize <= 0 || pieceLength <= 0)
        return false;

    const std::uint64_t limit = config.maxUsedSpaceMb * 1024 * 1024;
    if (static_cast<std::uint64_t>(fileSize) > limit)
        return false;

    const bool complete = fileIsComplete(handle, files, location.index);
    if (!complete && !makeRoom(static_cast<std::uint64_t>(fileSize), location.hash, torrentsMap, torrents, activeFiles, session))
        return false;
    // Remove priorities only when no one is downloading
    bool activate = activeFiles[name]++ == 0;
    if (activate) {
        handle.file_priority(location.index, lt::default_priority);
        const auto firstPiece = static_cast<lt::piece_index_t>(fileOffset / pieceLength);
        const auto lastPiece = static_cast<lt::piece_index_t>(
            (fileOffset + fileSize - 1) / pieceLength);
        int deadline = 0;
        for (auto piece = firstPiece; piece <= lastPiece; ++piece) {
            handle.piece_priority(piece, lt::default_priority);
            handle.set_piece_deadline(piece, deadline);
            deadline = std::min(deadline + 25, 60000);
        }
    }
    handle.resume();
    handle.force_dht_announce();
    handle.force_reannounce();

    path = files.file_path(location.index, downloadDir);
    size = fileSize;
    download = { path, fileSize, handle, fileOffset, pieceLength };
    return true;
}

void releaseFile(const std::string& name,
    TorrentsMap& torrentsMap, TorrentList& torrents, ActiveFiles& activeFiles,
    std::mutex& activationMutex)
{
    std::lock_guard<std::mutex> lock(activationMutex);
    auto active = activeFiles.find(name);
    if (active == activeFiles.end())
        return;
    if (--active->second != 0)
        return;
    activeFiles.erase(active);

    auto mapped = torrentsMap.find(name);
    if (mapped == torrentsMap.end()) {
        return;
    }

    const FileLocation location = mapped->second;
    lt::torrent_handle handle = getTorrentHandle(torrents, location.hash);
    if (handle.is_valid()) {
        auto torrentFile = handle.torrent_file();
        if (torrentFile) {
            const auto& files = torrentFile->files();
            handle.file_priority(location.index, lt::dont_download);
            logEvent("INFO", "download_deactivated", "name=" + name + " reason=file_released");
            const auto firstPiece = static_cast<lt::piece_index_t>(
                files.file_offset(location.index) / files.piece_length());
            const auto lastPiece = static_cast<lt::piece_index_t>(
                (files.file_offset(location.index) + files.file_size(location.index) - 1)
                / files.piece_length());
            for (auto piece = firstPiece; piece <= lastPiece; ++piece)
                handle.reset_piece_deadline(piece);

            for (const auto& activeFile : activeFiles) {
                if (activeFile.first == name)
                    continue;
                auto other = torrentsMap.find(activeFile.first);
                if (other == torrentsMap.end())
                    continue;
                if (other->second.hash == location.hash) {
                    const auto otherIndex = other->second.index;
                    handle.file_priority(otherIndex, lt::default_priority);
                    const auto otherFirst = static_cast<lt::piece_index_t>(
                        files.file_offset(otherIndex) / files.piece_length());
                    const auto otherLast = static_cast<lt::piece_index_t>(
                        (files.file_offset(otherIndex) + files.file_size(otherIndex) - 1)
                        / files.piece_length());
                    for (auto piece = otherFirst; piece <= otherLast; ++piece)
                        handle.piece_priority(piece, lt::default_priority);
                }
            }
            if (!torrentHasActiveFile(location.hash, torrentsMap, activeFiles))
                handle.pause();
        }
    }
}

bool streamFile(boost::beast::tcp_stream& stream, const std::filesystem::path& path,
    std::int64_t fileSize, const std::string& fileName, lt::torrent_handle handle,
    std::int64_t fileOffset, std::int64_t pieceLength, std::int64_t& bytesSent)
{
    http::response<http::empty_body> response { http::status::ok, 11 };
    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Expose-Headers", "Content-Disposition, Content-Length, Content-Type");
    response.set(http::field::content_type, "application/octet-stream");
    response.set(http::field::content_disposition, "attachment; filename=\"" + safeHeaderFilename(fileName) + "\"");
    response.set("X-Accel-Buffering", "no");
    response.set(http::field::cache_control, "no-cache");
    response.set("X-Content-Type-Options", "nosniff");
    response.content_length(static_cast<uint64_t>(fileSize));

    boost::system::error_code error;
    bytesSent = 0;
    http::response_serializer<http::empty_body> serializer(response);
    stream.expires_after(std::chrono::seconds(60));
    http::write_header(stream, serializer, error);
    if (error)
        return false;

    constexpr size_t bufferSize = 256 * 1024;
    std::vector<char> buffer(bufferSize);
    off_t offset = 0;
    int fd = -1;
    auto pieceWaitStarted = std::chrono::steady_clock::now();

    auto clientDisconnected = [&stream]() {
        pollfd descriptor { stream.socket().native_handle(), POLLIN | POLLERR | POLLHUP | POLLNVAL, 0 };
        if (::poll(&descriptor, 1, 0) <= 0)
            return false;
        if (descriptor.revents & POLLNVAL)
            return true;

        char byte = 0;
        const ssize_t bytesRead = ::recv(stream.socket().native_handle(), &byte, 1,
            MSG_PEEK | MSG_DONTWAIT);
        if (bytesRead == 0)
            return true;
        if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return true;
        return false;
    };

    // Wait for each piece before reading it
    while (offset < fileSize) {
        if (clientDisconnected())
            break;
        if (fd == -1) {
            fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd == -1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        }

        const auto absoluteOffset = fileOffset + offset;
        const auto piece = static_cast<lt::piece_index_t>(absoluteOffset / pieceLength);
        if (!handle.have_piece(piece)) {
            if (std::chrono::steady_clock::now() - pieceWaitStarted >= pieceWaitTimeout)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        pieceWaitStarted = std::chrono::steady_clock::now();

        const auto pieceOffset = absoluteOffset % pieceLength;
        size_t bytesToRead = static_cast<size_t>(std::min<off_t>(
            { static_cast<off_t>(buffer.size()), pieceLength - pieceOffset,
                fileSize - offset }));
        ssize_t bytesRead = ::pread(fd, buffer.data(), bytesToRead, offset);
        if (bytesRead == 0) {
            ::close(fd);
            fd = -1;
            continue;
        }
        if (bytesRead < 0)
            break;

        stream.expires_after(std::chrono::seconds(60));
        boost::asio::write(stream, boost::asio::buffer(buffer.data(), bytesRead), error);
        if (error)
            break;
        offset += bytesRead;
        bytesSent = offset;
    }

    if (fd != -1)
        ::close(fd);
    return !error && offset == fileSize;
}

template <typename Body, typename Fields>
void addCorsHeaders(http::response<Body, Fields>& response)
{
    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "GET, OPTIONS");
    response.set("Access-Control-Allow-Headers", "Range, Content-Type");
    response.set("Access-Control-Expose-Headers", "Content-Disposition, Content-Length, Content-Type");
}

std::string getClientAddress(const tcp::socket& socket,
    const http::request<http::string_body>& request)
{
    boost::system::error_code endpointError;
    const std::string peerAddress = socket.remote_endpoint(endpointError).address().to_string();
    if (endpointError || peerAddress != config.trustedProxyAddress)
        return endpointError ? "unknown" : peerAddress;

    const auto forwardedAddress = request["X-Real-IP"];
    if (forwardedAddress.empty())
        return peerAddress;

    boost::system::error_code addressError;
    const auto parsedAddress = boost::asio::ip::make_address(std::string(forwardedAddress), addressError);
    return addressError ? peerAddress : parsedAddress.to_string();
}

void handleHttpConnection(tcp::socket socket, TorrentsMap& torrentsMap,
    TorrentList& torrents, ActiveFiles& activeFiles, std::mutex& activationMutex,
    lt::session& session)
{
    bool fileSlot = false;
    std::string client;
    std::string name;
    try {
        boost::beast::tcp_stream stream(std::move(socket));
        boost::beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(8192);
        parser.header_limit(8192);
        boost::system::error_code error;
        stream.expires_after(std::chrono::seconds(30));
        http::read(stream, buffer, parser, error);
        if (error)
            return;
        const auto& request = parser.get();
        client = getClientAddress(stream.socket(), request);

        if (request.method() != http::verb::get
            || request.target().substr(0, 6) != "/file/") {
            http::response<http::string_body> response { http::status::not_found, 11 };
            addCorsHeaders(response);
            response.body() = "not found";
            response.prepare_payload();
            stream.expires_after(std::chrono::seconds(60));
            http::write(stream, response, error);
            return;
        }

        std::string_view target = request.target();
        target.remove_prefix(6);
        name = urlDecode(target);
        std::filesystem::path path;
        std::int64_t fileSize = 0;
        ActiveDownload download { };
        if (name.empty() || name.find('?') != std::string::npos
            || !getFileDetails(torrentsMap, torrents, name, path, fileSize, activeFiles,
                activationMutex, session, download)) {
            http::response<http::string_body> response { http::status::not_found, 11 };
            addCorsHeaders(response);
            response.body() = "not found";
            response.prepare_payload();
            stream.expires_after(std::chrono::seconds(60));
            http::write(stream, response, error);
            return;
        }
        // Keep the file active until streaming finishes or the client disconnects
        fileSlot = true;

        logEvent("INFO", "download_started", "client=" + client + " name=" + name + " bytes=" + std::to_string(fileSize));
        std::int64_t bytesSent = 0;
        bool completed = streamFile(stream, download.path, download.size,
            download.path.filename().string(), download.handle,
            download.fileOffset, download.pieceLength, bytesSent);
        logEvent("INFO", completed ? "download_completed" : "download_cancelled",
            "client=" + client + " name=" + name + " bytes=" + std::to_string(bytesSent) + " expected=" + std::to_string(fileSize));
    } catch (const std::exception& error) {
        logEvent("ERROR", "http_connection_error", "client=" + client + " reason=" + error.what());
    }
    if (fileSlot)
        releaseFile(name, torrentsMap, torrents, activeFiles, activationMutex);
}

void startHttpServer(TorrentsMap& torrentsMap, TorrentList& torrents,
    std::atomic_bool& shuttingDown, lt::session& session)
{
    boost::asio::io_context context;
    tcp::acceptor acceptor(context);
    acceptor.open(tcp::v4());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind({ tcp::v4(), config.httpPort });
    acceptor.listen(1024);
    constexpr size_t workerCount = 64;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable queueSpace;
    std::queue<tcp::socket> pending;
    constexpr size_t queueCapacity = 256;
    bool stopping = false;
    ActiveFiles activeFiles;
    std::mutex activationMutex;

    auto worker = [&]() {
        for (;;) {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueReady.wait(lock, [&]() { return stopping || !pending.empty(); });
            if (stopping && pending.empty())
                return;
            tcp::socket socket = std::move(pending.front());
            pending.pop();
            queueSpace.notify_one();
            lock.unlock();

            handleHttpConnection(std::move(socket), torrentsMap, torrents,
                activeFiles, activationMutex, session);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i)
        workers.emplace_back(worker);

    try {
        for (;;) {
            tcp::socket socket(context);
            acceptor.accept(socket);

            std::unique_lock<std::mutex> lock(queueMutex);
            queueSpace.wait(lock, [&]() {
                return stopping || pending.size() < queueCapacity;
            });
            if (stopping)
                break;
            pending.push(std::move(socket));
            lock.unlock();
            queueReady.notify_one();
        }
    } catch (const std::exception& error) {
        logEvent("ERROR", "http_server_stopped", error.what());
    }

    // Stop accepting work and let queued connections finish
    shuttingDown = true;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }
    queueReady.notify_all();
    for (auto& workerThread : workers)
        workerThread.join();
}

int main(void)
{
    try {
        config = loadConfig();
    } catch (const std::exception& error) {
        logEvent("ERROR", "config_load_failed", error.what());
        return 1;
    }
    try {
        initializeStorage();
    } catch (const std::exception& error) {
        logEvent("ERROR", "storage_initialization_failed", error.what());
        return 1;
    }
    logEvent("INFO", "server_starting", "workdir=" + std::filesystem::current_path().string());

    TorrentsMap torrentsMap;
    if (!loadTorrentsMap(torrentsMap))
        return 1;

    lt::settings_pack sp;
    initSettingsPack(sp);

    lt::session s(sp);
    std::atomic_bool shuttingDown { false };

    TorrentList torrents;
    loadTorrents(torrents, s);

    std::thread torrentWorkerThread([&s, &shuttingDown]() {
        startTorrentWorker(s, shuttingDown);
    });

    int result = 0;
    try {
        startHttpServer(torrentsMap, torrents, shuttingDown, s);
    } catch (const std::exception& error) {
        logEvent("ERROR", "http_server_start_failed", error.what());
        shuttingDown = true;
        result = 1;
    }
    torrentWorkerThread.join();

    logEvent("INFO", "server_stopped");

    return result;
}
