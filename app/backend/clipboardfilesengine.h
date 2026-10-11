/**
 * @file
 * @brief Shared v1 bounded file clipboard wire format and Windows staging engine.
 */
#pragma once
#ifndef MOONLIGHT_CLIPBOARD_FILES_ENGINE_H
  #define MOONLIGHT_CLIPBOARD_FILES_ENGINE_H

  // The v1 wire format and filesystem rules are shared verbatim with Sunshine.
  // No remote absolute path is ever accepted. Batches contain bounded file ID,
  // offset and byte-count records; the receiver verifies the deterministic plan.
  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <future>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <set>
  #include <stdexcept>
  #include <string>
  #include <string_view>
  #include <vector>

  #ifdef _WIN32
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <openssl/evp.h>
    #include <openssl/rand.h>
  // clang-format off
    #include <Windows.h>
    #include <shellapi.h>
    #include <ShlObj.h>
  // clang-format on
  #endif

/**
 * @brief Clipboard-only authenticated bulk data and private staging operations.
 */
namespace clipboard_files {
  constexpr uint32_t version = 1;  ///< Negotiated bulk protocol version.
  constexpr uint32_t max_wire_bytes = 4 * 1024 * 1024;  ///< Maximum raw batch bytes.
  constexpr uint32_t max_manifest_bytes = 32 * 1024 * 1024;  ///< Maximum manifest bytes.
  constexpr uint32_t max_drop_bytes = 32 * 1024 * 1024;  ///< Maximum native CF_HDROP selection bytes, independent of batch size.
  constexpr uint32_t max_entries = 100000;  ///< Maximum combined file and directory count.
  constexpr uint64_t max_total_bytes = 64ULL * 1024 * 1024 * 1024;  ///< Maximum total regular file bytes.
  constexpr unsigned parallel_workers = 4;  ///< Maximum concurrent transport lanes.

  /**
   * @brief Validated relative file or directory without a remote absolute path.
   */
  struct entry_t {
    std::string path;  ///< UTF-8 relative Windows-safe name, using slash separators.
    bool directory = false;  ///< Whether this entry denotes a directory.
    uint64_t size = 0;  ///< Regular file length, zero for directories.
    std::array<unsigned char, 32> hash {};  ///< SHA-256 file digest, zero for directories.
  };

  /**
   * @brief A bounded copy-only selection independent of its local source paths.
   */
  struct manifest_t {
    std::vector<entry_t> entries;  ///< Files and their explicit parent directories.
    uint32_t sequence = 0;  ///< Clipboard generation when the selection was captured.
  };

  /**
   * @brief One deterministic slice of a file, serialized as little-endian ID/offset/length.
   */
  struct fragment_t {
    uint32_t file;  ///< Manifest index of the regular file.
    uint64_t offset;  ///< Byte offset within that file.
    uint32_t length;  ///< Number of raw payload bytes in this fragment.
  };

  using batch_t = std::vector<fragment_t>;  ///< Fragments packed into one bounded request.
  using plan_t = std::vector<batch_t>;  ///< Deterministic batches reconstructed independently by each endpoint.
  using cancel_t = std::function<bool()>;  ///< Return true if this transfer generation is invalid.
  using context_t = std::function<std::shared_ptr<void>()>;  ///< Enter a scoped worker security identity, or leave empty for the current process user.

  /**
   * @brief Enforce a protocol or filesystem precondition before proceeding.
   */
  inline void require(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  /**
   * @brief Serialize a bounded little-endian integer.
   */
  inline void append_number(std::string &out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
      out.push_back(static_cast<char>((value >> (i * 8)) & 255));
    }
  }

  /**
   * @brief Decode a little-endian integer with remaining-buffer checks.
   */
  inline uint64_t read_number(std::string_view in, size_t &position, unsigned bytes) {
    require(position <= in.size() && bytes <= in.size() - position, "Truncated clipboard wire data");
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) {
      value |= uint64_t(static_cast<unsigned char>(in[position++])) << (i * 8);
    }
    return value;
  }

  #ifdef _WIN32
  /**
   * @brief Restore thread background priority before a Windows pool callback returns.
   */
  class background_t {
    bool active = false;  ///< Whether background-priority restoration is necessary.

  public:
    /**
     * @brief Enter Windows background mode for this worker.
     */
    background_t():
        active(SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0) {}

    /**
     * @brief Restore the priority captured by background-mode entry.
     */
    ~background_t() {
      if (active) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
      }
    }
  };

  /**
   * @brief Decode a strictly valid bounded UTF-8 path.
   */
  inline std::wstring wide(std::string_view text) {
    require(!text.empty() && text.size() <= 8192, "Invalid path length");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    require(count > 0, "Invalid UTF-8 path");
    std::wstring result(count, 0);
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count) == count, "Invalid UTF-8 path");
    return result;
  }

  /**
   * @brief Encode a Unicode Windows path without replacement characters.
   */
  inline std::string utf8(std::wstring_view text) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(count > 0, "Invalid Unicode path");
    std::string result(count, 0);
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr) == count, "Invalid Unicode path");
    return result;
  }

  /**
   * @brief Compute invariant case-insensitive Windows collision keys.
   */
  inline std::wstring folded(std::string_view path) {
    auto value = wide(path);
    int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    require(count > 0, "Invalid Windows path");
    std::wstring result(count, 0);
    require(LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr, 0) == count, "Invalid Windows path");
    return result;
  }

  /**
   * @brief Reject traversal, alternate streams, reserved names and unsafe Windows components.
   */
  inline void validate_path(std::string_view path) {
    require(!path.empty() && path.size() <= 4096 && path.front() != '/' && path.back() != '/', "Unsafe relative path");
    auto unicode = wide(path);
    require(unicode.size() <= 2048, "Relative path too long");
    size_t begin = 0;
    while (begin < path.size()) {
      size_t end = path.find('/', begin);
      if (end == std::string_view::npos) {
        end = path.size();
      }
      auto name = path.substr(begin, end - begin);
      require(!name.empty() && name != "." && name != ".." && name.back() != '.' && name.back() != ' ', "Unsafe Windows filename");
      require(wide(name).size() <= 240, "Filename too long");
      for (unsigned char c : name) {
        require(c >= 32 && c != 127 && c != '\\' && c != ':' && c != '<' && c != '>' && c != '"' && c != '|' && c != '?' && c != '*', "Unsafe Windows filename");
      }
      auto base = folded(name.substr(0, name.find('.')));
      require(base != L"CON" && base != L"PRN" && base != L"AUX" && base != L"NUL" && base != L"CONIN$" && base != L"CONOUT$", "Reserved Windows filename");
      require(!(base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") && ((base[3] >= L'0' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3')), "Reserved Windows filename");
      begin = end + 1;
    }
  }

  /**
   * @brief Bound entries and total bytes and verify unique names and explicit parent directories.
   */
  inline void validate_manifest(const manifest_t &manifest) {
    require(!manifest.entries.empty() && manifest.entries.size() <= max_entries, "Invalid clipboard entry count");
    std::map<std::wstring, bool> seen;
    uint64_t total = 0;
    for (const auto &entry : manifest.entries) {
      validate_path(entry.path);
      require(entry.size <= max_total_bytes - total, "Clipboard exceeds total byte limit");
      total += entry.size;
      if (entry.directory) {
        require(entry.size == 0 && entry.hash == std::array<unsigned char, 32> {}, "Invalid directory entry");
      }
      require(seen.emplace(folded(entry.path), entry.directory).second, "Case-insensitive path collision");
    }
    for (const auto &entry : manifest.entries) {
      size_t slash = entry.path.rfind('/');
      if (slash != std::string::npos) {
        auto parent = seen.find(folded(std::string_view(entry.path).substr(0, slash)));
        require(parent != seen.end() && parent->second, "Missing or invalid parent directory");
      }
    }
  }

  /**
   * @brief Serialize a validated manifest within the negotiated byte bound.
   */
  inline std::string encode_manifest(const manifest_t &manifest) {
    validate_manifest(manifest);
    std::string wire("MLCF1", 5);
    append_number(wire, manifest.sequence, 4);
    append_number(wire, manifest.entries.size(), 4);
    for (const auto &entry : manifest.entries) {
      append_number(wire, entry.path.size(), 4);
      wire.append(entry.path);
      wire.push_back(entry.directory ? 1 : 0);
      append_number(wire, entry.size, 8);
      wire.append(reinterpret_cast<const char *>(entry.hash.data()), entry.hash.size());
      require(wire.size() <= max_manifest_bytes, "Clipboard manifest exceeds wire limit");
    }
    return wire;
  }

  /**
   * @brief Decode a bounded manifest without accepting trailing or malformed data.
   */
  inline manifest_t decode_manifest(std::string_view wire) {
    require(wire.size() <= max_manifest_bytes && wire.substr(0, 5) == "MLCF1", "Invalid clipboard manifest");
    size_t position = 5;
    manifest_t result;
    result.sequence = static_cast<uint32_t>(read_number(wire, position, 4));
    uint64_t count = read_number(wire, position, 4);
    require(count > 0 && count <= max_entries && count <= wire.size() / 45, "Invalid clipboard entry count");
    result.entries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
      entry_t entry;
      auto length = read_number(wire, position, 4);
      require(length > 0 && length <= 4096 && length <= wire.size() - position, "Invalid clipboard path length");
      entry.path = std::string(wire.substr(position, static_cast<size_t>(length)));
      position += static_cast<size_t>(length);
      auto type = read_number(wire, position, 1);
      require(type <= 1, "Invalid clipboard entry type");
      entry.directory = type == 1;
      entry.size = read_number(wire, position, 8);
      require(position + 32 <= wire.size(), "Missing clipboard file hash");
      std::copy_n(reinterpret_cast<const unsigned char *>(wire.data() + position), 32, entry.hash.begin());
      position += 32;
      result.entries.push_back(std::move(entry));
    }
    require(position == wire.size(), "Trailing clipboard manifest bytes");
    validate_manifest(result);
    return result;
  }

  /**
   * @brief Pack tiny files and large slices into deterministic bounded batches.
   */
  inline plan_t make_plan(const manifest_t &manifest) {
    validate_manifest(manifest);
    plan_t plan;
    batch_t batch;
    uint32_t used = 0;
    for (uint32_t id = 0; id < manifest.entries.size(); id++) {
      const auto &entry = manifest.entries[id];
      if (entry.directory || !entry.size) {
        continue;
      }
      uint64_t offset = 0;
      while (offset < entry.size) {
        if (used + 16 >= max_wire_bytes) {
          plan.push_back(std::move(batch));
          batch = {};
          used = 0;
        }
        uint32_t length = static_cast<uint32_t>(std::min<uint64_t>(entry.size - offset, max_wire_bytes - used - 16));
        batch.push_back({id, offset, length});
        used += 16 + length;
        offset += length;
      }
    }
    if (!batch.empty()) {
      plan.push_back(std::move(batch));
    }
    return plan;
  }

  /**
   * @brief Close a native file handle without copying ownership.
   */
  class handle_t {
  public:
    /**
     * @brief Take ownership of a native handle.
     * @param value Handle to close, or INVALID_HANDLE_VALUE.
     */
    explicit handle_t(HANDLE value = INVALID_HANDLE_VALUE):
        value(value) {}

    /**
     * @brief Close a still-owned native handle.
     */
    ~handle_t() {
      if (value != INVALID_HANDLE_VALUE) {
        CloseHandle(value);
      }
    }

    /**
     * @brief Prevent copying native handle ownership.
     */
    handle_t(const handle_t &) = delete;
    /**
     * @brief Prevent assigning native handle ownership.
     */
    handle_t &operator=(const handle_t &) = delete;
    HANDLE value;  ///< Owned native handle, or INVALID_HANDLE_VALUE.
  };

  /**
   * @brief Reject network and non-local roots before any filesystem probe.
   */
  inline void local_path(const std::filesystem::path &path) {
    require(path.is_absolute(), "Local clipboard path is not absolute");
    require(path.native().rfind(L"\\\\", 0) != 0, "Network clipboard paths are unsupported in protocol v1");
    auto type = GetDriveTypeW(path.root_path().c_str());
    require(type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK, "Network clipboard paths are unsupported in protocol v1");
  }

  /**
   * @brief Generate an internal extended local path after rejecting caller-supplied device and network roots.
   */
  inline std::filesystem::path native_path(const std::filesystem::path &path) {
    local_path(path);
    auto value = path.lexically_normal().native();
    require(value.size() < 32760, "Local clipboard path exceeds Windows limit");
    return std::filesystem::path(L"\\\\?\\" + value);
  }

  /**
   * @brief Reject missing paths and reparse points throughout the existing ancestor chain.
   */
  inline void plain_path(const std::filesystem::path &path) {
    local_path(path);
    auto current = path;
    for (;;) {
      auto attributes = GetFileAttributesW(native_path(current).c_str());
      require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Missing path or reparse point");
      auto parent = current.parent_path();
      if (parent == current) {
        break;
      }
      current = parent;
    }
  }

  /**
   * @brief Verify a native handle refers to a regular non-reparse file.
   */
  inline BY_HANDLE_FILE_INFORMATION file_info(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION result {};
    require(handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &result), "Cannot open clipboard file");
    require(!(result.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "Unsafe clipboard file");
    return result;
  }

  /**
   * @brief Return the unsigned file length from native handle metadata.
   */
  inline uint64_t file_size(const BY_HANDLE_FILE_INFORMATION &info) {
    return (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  }

  /**
   * @brief Compare volume, file identity, size and modification timestamp.
   */
  inline bool same_file(const BY_HANDLE_FILE_INFORMATION &a, const BY_HANDLE_FILE_INFORMATION &b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow && file_size(a) == file_size(b) && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
  }

  /**
   * @brief Open a source without allowing concurrent writes/deletion or following a leaf reparse point.
   */
  inline handle_t open_read(const std::filesystem::path &path) {
    plain_path(path);
    return handle_t(CreateFileW(native_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  }

  /**
   * @brief Hash file bytes incrementally while sampling cancellation.
   */
  inline std::array<unsigned char, 32> hash_handle(HANDLE handle, cancel_t cancelled = {}) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1, "Cannot initialize file hash");
    // Reuse one scratch allocation per hashing worker; tiny files otherwise
    // repeatedly allocate and zero a megabyte despite reading only a few bytes.
    thread_local std::vector<unsigned char> buffer(1024 * 1024);
    for (;;) {
      require(!cancelled || !cancelled(), "Clipboard transfer cancelled");
      DWORD count = 0;
      require(ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr), "Cannot read clipboard file");
      if (!count) {
        break;
      }
      require(EVP_DigestUpdate(context.get(), buffer.data(), count) == 1, "Cannot hash clipboard file");
    }
    std::array<unsigned char, 32> hash {};
    unsigned count = 0;
    require(EVP_DigestFinal_ex(context.get(), hash.data(), &count) == 1 && count == hash.size(), "Cannot finish file hash");
    return hash;
  }

  /**
   * @brief Create a cryptographically random opaque transfer/stage identity.
   */
  inline std::string random_id() {
    unsigned char bytes[16];
    require(RAND_bytes(bytes, sizeof(bytes)) == 1, "Cannot generate transfer identity");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
      result.push_back(hex[byte >> 4]);
      result.push_back(hex[byte & 15]);
    }
    return result;
  }

  /**
   * @brief Accept only canonical random identity strings.
   */
  inline bool valid_id(std::string_view id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
  }

  /**
   * @brief Add a native selection path length without exceeding the CF_HDROP allocation budget.
   * @param bytes Existing byte budget, including the DROPFILES header and final list terminator.
   * @param characters UTF-16 path length without its individual terminator.
   * @return Updated selection byte budget.
   */
  inline size_t add_drop_path_bytes(size_t bytes, size_t characters) {
    require(bytes <= max_drop_bytes && characters > 0 && characters < (max_drop_bytes - bytes) / sizeof(wchar_t), "Clipboard root list too large");
    return bytes + (characters + 1) * sizeof(wchar_t);
  }

  /**
   * @brief Bound a native source or destination selection before probing files or allocating its buffer.
   * @param paths Absolute paths to project into CF_HDROP.
   * @return Complete native selection byte budget.
   */
  inline size_t drop_list_bytes(const std::vector<std::filesystem::path> &paths) {
    require(!paths.empty() && paths.size() <= max_entries, "Too many clipboard roots");
    size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);
    for (const auto &path : paths) {
      bytes = add_drop_path_bytes(bytes, path.native().size());
    }
    return bytes;
  }

  /**
   * @brief Project received top-level roots and check their expanded native selection before staging.
   * @param manifest Validated incoming relative entries.
   * @param directory Random destination stage path, whose prefix expands every received root.
   * @return Bounded absolute roots ready for later native publication.
   */
  inline std::vector<std::filesystem::path> project_drop_roots(const manifest_t &manifest, const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> roots;
    for (const auto &entry : manifest.entries) {
      if (entry.path.find('/') == std::string::npos) {
        roots.push_back(directory / std::filesystem::path(wide(entry.path)));
      }
    }
    drop_list_bytes(roots);
    return roots;
  }

  /**
   * @brief Hash and track an immutable clipboard source selection without pinning all handles.
   */
  class source_t {
  public:
    manifest_t manifest;  ///< Advertised relative snapshot.
    plan_t plan;  ///< Deterministic bounded batch schedule.
    std::vector<std::filesystem::path> paths;  ///< Private local file mapping, never serialized.
    std::vector<BY_HANDLE_FILE_INFORMATION> identities;  ///< Size/mtime/file identity captured during hashing.

    /**
     * @brief Scan and hash a bounded local selection under the caller's security identity.
     * @param roots Absolute local file or directory roots.
     * @param sequence Clipboard generation that selected these roots.
     * @param cancelled Optional cancellation and generation check.
     */
    source_t(const std::vector<std::filesystem::path> &roots, uint32_t sequence, cancel_t cancelled = {}) {
      drop_list_bytes(roots);
      manifest.sequence = sequence;
      uint64_t total_bytes = 0;
      size_t manifest_bytes = 13;
      auto add = [&](const std::filesystem::path &path, const std::filesystem::path &relative) {
        require(!cancelled || !cancelled(), "Clipboard transfer cancelled");
        require(manifest.entries.size() < max_entries, "Too many clipboard entries");
        plain_path(path);
        entry_t entry;
        entry.path = utf8(relative.generic_wstring());
        validate_path(entry.path);
        manifest_bytes += 45 + entry.path.size();
        require(manifest_bytes <= max_manifest_bytes, "Clipboard manifest exceeds wire limit");
        entry.directory = std::filesystem::is_directory(native_path(path));
        BY_HANDLE_FILE_INFORMATION identity {};
        if (!entry.directory) {
          auto handle = open_read(path);
          identity = file_info(handle.value);
          entry.size = file_size(identity);
          require(entry.size <= max_total_bytes - total_bytes, "Clipboard exceeds total byte limit");
          total_bytes += entry.size;
          entry.hash = hash_handle(handle.value, cancelled);
          require(same_file(identity, file_info(handle.value)), "Clipboard source changed while hashing");
        }
        manifest.entries.push_back(std::move(entry));
        paths.push_back(path);
        identities.push_back(identity);
      };
      for (const auto &original : roots) {
        local_path(original);
        auto root = std::filesystem::absolute(original).lexically_normal();
        require(!root.filename().empty(), "Cannot copy a filesystem root");
        add(root, root.filename());
        if (manifest.entries.back().directory) {
          auto native_root = native_path(root);
          for (const auto &child : std::filesystem::recursive_directory_iterator(native_root)) {
            auto relative = child.path().lexically_relative(native_root);
            add(root / relative, root.filename() / relative);
          }
        }
      }
      // Encoding also bounds the manifest before any transfer is advertised.
      encode_manifest(manifest);
      plan = make_plan(manifest);
    }

    /**
     * @brief Read one deterministic batch while revalidating source file identities.
     * @param index Batch index from the manifest-derived plan.
     * @param cancelled Optional cancellation and generation check.
     * @return Bounded binary fragment records and raw file bytes.
     */
    std::string read_batch(uint32_t index, cancel_t cancelled = {}) const {
      require(index < plan.size(), "Invalid clipboard batch index");
      std::string result;
      result.reserve(max_wire_bytes);
      for (const auto &fragment : plan[index]) {
        require(!cancelled || !cancelled(), "Clipboard transfer cancelled");
        auto handle = open_read(paths[fragment.file]);
        require(same_file(identities[fragment.file], file_info(handle.value)), "Clipboard source changed");
        LARGE_INTEGER offset {};
        offset.QuadPart = static_cast<LONGLONG>(fragment.offset);
        require(SetFilePointerEx(handle.value, offset, nullptr, FILE_BEGIN), "Cannot seek clipboard source");
        append_number(result, fragment.file, 4);
        append_number(result, fragment.offset, 8);
        append_number(result, fragment.length, 4);
        size_t position = result.size();
        result.resize(position + fragment.length);
        DWORD read = 0;
        require(ReadFile(handle.value, result.data() + position, fragment.length, &read, nullptr) && read == fragment.length, "Clipboard source changed size");
        require(same_file(identities[fragment.file], file_info(handle.value)), "Clipboard source changed");
      }
      return result;
    }
  };

  /**
   * @brief Own a private stage until every deterministic batch and SHA-256 digest verifies.
   */
  class receiver_t {
  public:
    manifest_t manifest;  ///< Validated incoming relative snapshot.
    plan_t plan;  ///< Expected file fragments for each batch.
    std::filesystem::path directory;  ///< Random local stage beneath the fixed per-user storage root.
    std::vector<std::filesystem::path> roots;  ///< Verified roots to publish as a local CF_HDROP selection.
    std::atomic<bool> cancelled {false};  ///< Abort flag shared by parallel receiver lanes.

  private:
    std::mutex mutex;  ///< Protect batch lifecycle transitions.
    std::vector<unsigned char> completed;  ///< Zero for unsent, one for in-flight, two for accepted batches.
    bool retained = false;  ///< Whether verified clipboard roots must survive receiver destruction.
    std::filesystem::path marker;  ///< Ownership marker used for safe later cleanup.
    std::unique_ptr<handle_t> marker_lease;  ///< Open lease preventing cleanup while this stage is active.

  public:
    /**
     * @brief Create a private stage and preallocate the validated incoming files.
     * @param incoming Bounded relative manifest.
     * @param storage Fixed application-owned local staging parent.
     * @param check Optional cancellation callback.
     */
    receiver_t(manifest_t incoming, const std::filesystem::path &storage, cancel_t check = {}):
        manifest(std::move(incoming)),
        plan(make_plan(manifest)) {
      // Storage is a fixed local per-user path supplied by the application.
      local_path(storage);
      directory = storage / std::filesystem::path(wide(random_id()));
      roots = project_drop_roots(manifest, directory);
      auto existing = storage;
      while (!std::filesystem::exists(native_path(existing))) {
        existing = existing.parent_path();
      }
      plain_path(existing);
      std::filesystem::create_directories(native_path(storage));
      plain_path(storage);
      uint64_t total = 0;
      for (const auto &entry : manifest.entries) {
        total += entry.size;
      }
      ULARGE_INTEGER free {};
      require(GetDiskFreeSpaceExW(native_path(storage).c_str(), &free, nullptr, nullptr) && free.QuadPart >= total + 1024ULL * 1024 * 1024, "Insufficient clipboard staging space");
      require(std::filesystem::create_directory(native_path(directory)), "Cannot create private clipboard stage");
      marker = directory;
      marker += L".pending";
      try {
        marker_lease = std::make_unique<handle_t>(CreateFileW(native_path(marker).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(marker_lease->value != INVALID_HANDLE_VALUE, "Cannot mark clipboard stage");
        auto label = "MLCF1 " + utf8(directory.filename().native()) + "\n";
        DWORD written = 0;
        require(WriteFile(marker_lease->value, label.data(), static_cast<DWORD>(label.size()), &written, nullptr) && written == label.size(), "Cannot mark clipboard stage");
        std::vector<const entry_t *> directories;
        for (const auto &entry : manifest.entries) {
          if (entry.directory) {
            directories.push_back(&entry);
          }
        }
        std::sort(directories.begin(), directories.end(), [](auto a, auto b) {
          return a->path.size() < b->path.size();
        });
        for (const auto *entry : directories) {
          require(!check || !check(), "Clipboard transfer cancelled");
          std::filesystem::create_directory(native_path(directory / std::filesystem::path(wide(entry->path))));
        }
        for (const auto &entry : manifest.entries) {
          require(!check || !check(), "Clipboard transfer cancelled");
          auto path = directory / std::filesystem::path(wide(entry.path));
          if (!entry.directory) {
            plain_path(path.parent_path());
            handle_t file(CreateFileW(native_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            file_info(file.value);
            LARGE_INTEGER size {};
            size.QuadPart = static_cast<LONGLONG>(entry.size);
            require(SetFilePointerEx(file.value, size, nullptr, FILE_BEGIN) && SetEndOfFile(file.value), "Cannot allocate clipboard file");
          }
        }
        completed.resize(plan.size());
      } catch (...) {
        marker_lease.reset();
        std::filesystem::remove_all(native_path(directory));
        std::filesystem::remove(native_path(marker));
        throw;
      }
    }

    /**
     * @brief Release the active lease and remove an unpublished stage.
     */
    ~receiver_t() {
      marker_lease.reset();
      if (!retained && !directory.empty()) {
        // The remote peer cannot choose this unique private stage path.
        try {
          std::error_code error;
          std::filesystem::remove_all(native_path(directory), error);
          std::filesystem::remove(native_path(marker), error);
        } catch (...) {}  // A disappeared local drive is cleaned on a later run.
      }
    }

    /**
     * @brief Validate and apply one bounded batch without permitting duplicate writes.
     * @param index Manifest-derived batch index.
     * @param wire Raw deterministic fragment records and file bytes.
     */
    void accept_batch(uint32_t index, std::string_view wire) {
      require(wire.size() <= max_wire_bytes && index < plan.size() && !cancelled, "Invalid clipboard batch");
      {
        std::lock_guard<std::mutex> lock(mutex);
        require(completed[index] == 0, "Duplicate clipboard batch");
        completed[index] = 1;
      }
      try {
        size_t position = 0;
        for (const auto &fragment : plan[index]) {
          require(!cancelled, "Clipboard transfer cancelled");
          require(read_number(wire, position, 4) == fragment.file && read_number(wire, position, 8) == fragment.offset && read_number(wire, position, 4) == fragment.length, "Clipboard batch does not match manifest");
          require(fragment.length <= wire.size() - position, "Truncated clipboard batch");
          auto path = directory / std::filesystem::path(wide(manifest.entries[fragment.file].path));
          plain_path(path);
          handle_t file(CreateFileW(native_path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
          require(file_size(file_info(file.value)) == manifest.entries[fragment.file].size, "Clipboard stage changed size");
          LARGE_INTEGER offset {};
          offset.QuadPart = static_cast<LONGLONG>(fragment.offset);
          require(SetFilePointerEx(file.value, offset, nullptr, FILE_BEGIN), "Cannot seek clipboard stage");
          DWORD written = 0;
          require(WriteFile(file.value, wire.data() + position, fragment.length, &written, nullptr) && written == fragment.length, "Cannot write clipboard stage");
          position += fragment.length;
        }
        require(position == wire.size(), "Trailing clipboard batch bytes");
        std::lock_guard<std::mutex> lock(mutex);
        completed[index] = 2;
      } catch (...) {
        cancelled = true;
        throw;
      }
    }

    /**
     * @brief Verify every file with at most four workers, each holding its own security context and cancellation copy.
     */
    void verify(cancel_t check = {}, context_t enter_context = {}) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        require(std::all_of(completed.begin(), completed.end(), [](auto state) {
                  return state == 2;
                }),
                "Clipboard batches are incomplete");
      }
      std::atomic<size_t> next {0};
      auto worker = [&, check, enter_context]() mutable {
        background_t priority;
        try {
          auto context = enter_context ? enter_context() : std::shared_ptr<void>();
          require(!enter_context || context != nullptr, "Cannot enter clipboard file security context");
          for (;;) {
            auto index = next.fetch_add(1);
            if (index >= manifest.entries.size()) {
              break;
            }
            const auto &entry = manifest.entries[index];
            require(!cancelled && (!check || !check()), "Clipboard transfer cancelled");
            auto path = directory / std::filesystem::path(wide(entry.path));
            plain_path(path);
            if (!entry.directory) {
              auto file = open_read(path);
              require(file_size(file_info(file.value)) == entry.size && hash_handle(file.value, [&] {
                                                                          return cancelled || (check && check());
                                                                        }) == entry.hash,
                      "Clipboard file hash mismatch");
            }
          }
        } catch (...) {
          cancelled = true;
          throw;
        }
      };
      const unsigned workers = manifest.entries.size() < 32 ? 1 : parallel_workers;
      std::vector<std::future<void>> lanes;
      std::exception_ptr failure;
      try {
        for (unsigned i = 0; i < workers; i++) {
          lanes.push_back(std::async(std::launch::async, worker));
        }
      } catch (...) {
        cancelled = true;
        failure = std::current_exception();
      }
      for (auto &lane : lanes) {
        try {
          lane.get();
        } catch (...) {
          if (!failure) {
            failure = std::current_exception();
          }
        }
      }
      if (failure) {
        std::rethrow_exception(failure);
      }
    }

    /**
     * @brief Retain verified Explorer clipboard roots after application shutdown.
     */
    void retain() {
      retained = true;
      marker_lease.reset();
      auto received = directory;
      received += L".received";
      std::error_code error;
      std::filesystem::rename(native_path(marker), native_path(received), error);
      if (!error) {
        marker = received;
        std::filesystem::last_write_time(native_path(marker), std::filesystem::file_time_type::clock::now(), error);
      }
    }

    /**
     * @brief Leave a marked incomplete stage for later user-context cleanup after a failed security context restore.
     */
    void defer_cleanup() {
      retained = true;
    }
  };

  /**
   * @brief A native clipboard file selection and its captured sequence.
   */
  struct clipboard_snapshot_t {
    uint32_t sequence = 0;  ///< Native clipboard generation associated with the selection.
    std::vector<std::filesystem::path> paths;  ///< Private local source roots; never transmitted to peers.
  };

  /**
   * @brief Read native CF_HDROP on a worker thread without changing the clipboard.
   */
  inline clipboard_snapshot_t read_clipboard_files() {
    clipboard_snapshot_t result;
    require(OpenClipboard(nullptr), "Clipboard is busy");

    /**
     * @brief Close the read-only clipboard on every return or exception.
     */
    struct closer {
      /**
       * @brief Release the open native clipboard.
       */
      ~closer() {
        CloseClipboard();
      }
    } close;

    result.sequence = GetClipboardSequenceNumber();
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
      return result;
    }
    auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    require(drop != nullptr, "Clipboard files unavailable");
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    require(count > 0 && count <= max_entries, "Too many clipboard roots");
    size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);
    for (UINT i = 0; i < count; i++) {
      UINT length = DragQueryFileW(drop, i, nullptr, 0);
      require(length > 0 && length <= 32760, "Invalid clipboard root");
      bytes = add_drop_path_bytes(bytes, length);
      std::wstring path(length + 1, 0);
      require(DragQueryFileW(drop, i, path.data(), length + 1) == length, "Cannot read clipboard root");
      path.resize(length);
      result.paths.emplace_back(path);
    }
    return result;
  }

  /**
   * @brief Build and validate CF_HDROP outside authorization locks, then publish with a short sequence guard.
   */
  class prepared_drop_t {
    HGLOBAL memory = nullptr;  ///< Fully rendered file selection transferred to Windows on publication.

  public:
    /**
     * @brief Allocate an immediate-rendering file selection without holding authorization locks.
     * @param paths Verified absolute local roots to make pasteable.
     */
    explicit prepared_drop_t(const std::vector<std::filesystem::path> &paths) {
      require(!paths.empty(), "Empty clipboard file delivery");
      size_t bytes = drop_list_bytes(paths);
      for (const auto &path : paths) {
        plain_path(path);
      }
      memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
      require(memory != nullptr, "Cannot allocate clipboard file list");
      auto *drop = static_cast<DROPFILES *>(GlobalLock(memory));
      if (!drop) {
        GlobalFree(memory);
        memory = nullptr;
        throw std::runtime_error("Cannot lock clipboard file list");
      }
      drop->pFiles = sizeof(DROPFILES);
      drop->fWide = TRUE;
      auto *output = reinterpret_cast<wchar_t *>(reinterpret_cast<unsigned char *>(drop) + sizeof(DROPFILES));
      for (const auto &path : paths) {
        std::copy(path.native().begin(), path.native().end(), output);
        output += path.native().size();
        *output++ = 0;
      }
      *output = 0;
      GlobalUnlock(memory);
    }

    /**
     * @brief Release a file-selection buffer that Windows did not take ownership of.
     */
    ~prepared_drop_t() {
      if (memory) {
        GlobalFree(memory);
      }
    }

    /**
     * @brief Prevent copying pending clipboard memory ownership.
     */
    prepared_drop_t(const prepared_drop_t &) = delete;
    /**
     * @brief Prevent assigning pending clipboard memory ownership.
     */
    prepared_drop_t &operator=(const prepared_drop_t &) = delete;

    /**
     * @brief Publish a fully rendered file selection only if its destination generation still matches.
     * @param expected_sequence Clipboard generation captured before this transfer.
     * @return Published native clipboard generation.
     */
    uint32_t publish(uint32_t expected_sequence) {
      require(memory != nullptr, "Clipboard delivery already published");
      HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!owner || !OpenClipboard(owner)) {
        if (owner) {
          DestroyWindow(owner);
        }
        throw std::runtime_error("Clipboard is busy");
      }
      bool ok = GetClipboardSequenceNumber() == expected_sequence && EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
      uint32_t sequence = GetClipboardSequenceNumber();
      CloseClipboard();
      DestroyWindow(owner);
      if (!ok) {
        throw std::runtime_error("Clipboard changed before file delivery");
      }
      memory = nullptr;  // Windows owns the fully rendered global memory.
      return sequence;
    }
  };

  /**
   * @brief Prepare a fully verified selection then atomically check its destination generation.
   */
  inline uint32_t publish_clipboard_files(const std::vector<std::filesystem::path> &paths, uint32_t expected_sequence) {
    prepared_drop_t prepared(paths);
    return prepared.publish(expected_sequence);
  }

  // Retained Explorer copies survive app exit. Reclaim only our marked stages
  // older than seven days, excluding any roots still referenced by the clipboard.
  /**
   * @brief Reclaim only old owned inactive stages, protecting current clipboard roots and cancellation.
   */
  inline void prune_storage(const std::filesystem::path &storage, const std::vector<std::filesystem::path> &protected_roots, cancel_t check = {}) {
    if (!std::filesystem::exists(native_path(storage))) {
      return;
    }
    plain_path(storage);
    auto now = std::filesystem::file_time_type::clock::now();
    for (const auto &item : std::filesystem::directory_iterator(native_path(storage))) {
      if (check && check()) {
        return;
      }
      auto marker = storage / item.path().filename();
      bool pending = marker.extension() == L".pending";
      if ((!pending && marker.extension() != L".received") || !valid_id(utf8(marker.stem().native()))) {
        continue;
      }
      plain_path(marker);
      if (!item.is_regular_file() || item.file_size() > 64 || now - item.last_write_time() < std::chrono::hours(pending ? 24 : 24 * 7)) {
        continue;
      }
      // Live receivers deny DELETE on their pending marker for their lifetime.
      handle_t lease(CreateFileW(native_path(marker).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (lease.value == INVALID_HANDLE_VALUE) {
        continue;
      }
      char label[65];
      DWORD count = 0;
      if (!ReadFile(lease.value, label, sizeof(label), &count, nullptr)) {
        continue;
      }
      std::string text(label, count);
      if (text != "MLCF1 " + utf8(marker.stem().native()) + "\n") {
        continue;
      }
      auto stage = storage / marker.stem();
      if (!std::filesystem::is_directory(native_path(stage))) {
        continue;
      }
      plain_path(stage);
      bool protected_stage = false;
      auto stage_name = stage.lexically_normal().make_preferred().native();
      for (const auto &root : protected_roots) {
        auto root_name = root.lexically_normal().make_preferred().native();
        if (root_name.size() >= stage_name.size() && CompareStringOrdinal(root_name.data(), static_cast<int>(stage_name.size()), stage_name.data(), static_cast<int>(stage_name.size()), TRUE) == CSTR_EQUAL && (root_name.size() == stage_name.size() || root_name[stage_name.size()] == L'\\')) {
          protected_stage = true;
        }
      }
      if (protected_stage) {
        continue;
      }
      bool unsafe = false;
      std::vector<std::filesystem::path> entries;
      for (auto entry = std::filesystem::recursive_directory_iterator(native_path(stage)); entry != std::filesystem::recursive_directory_iterator(); ++entry) {
        if (check && check()) {
          return;
        }
        auto attributes = GetFileAttributesW(entry->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
          unsafe = true;
          break;
        }
        entries.push_back(entry->path());
      }
      if (unsafe) {
        continue;
      }
      for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
        if (check && check()) {
          return;
        }
        std::filesystem::remove(*entry);
      }
      std::filesystem::remove(native_path(stage));
      std::filesystem::remove(native_path(marker));
    }
  }
  #endif
}  // namespace clipboard_files
#endif  // MOONLIGHT_CLIPBOARD_FILES_ENGINE_H
