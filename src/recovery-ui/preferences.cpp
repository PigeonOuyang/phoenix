// SPDX-License-Identifier: Unlicense
#include "ui.hpp"
#include "settings_codec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <json-c/json.h>

namespace p4ui {
namespace {

constexpr const char* mountpoint = "/run/overpressure/ui-preferences";
constexpr const char* filename = "preferences.json";
constexpr std::size_t json_limit = 4096;
constexpr std::array<const char*, 2> catalog{"zh-CN","en"};
constexpr auto unsupported_choice = "Unsupported language; saved locale was not changed";

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& detail) {
    if (!condition)
        throw Failure(detail);
}

std::string system_error(const std::string& detail) {
    return detail + ": " + std::strerror(errno);
}

bool supported(const std::string& locale) {
    return std::any_of(catalog.begin(), catalog.end(),
                       [&](const char* entry) { return locale == entry; });
}

struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (value >= 0) ::close(value);
        value = std::exchange(other.value, -1);
        return *this;
    }
};

struct JsonDelete { void operator()(json_object* obj) const { json_object_put(obj); } };
using Json = std::unique_ptr<json_object, JsonDelete>;

std::string read_fd(int fd, std::size_t limit) {
    std::string result;
    std::array<char, 1024> buffer{};
    for (;;) {
        ssize_t n = ::read(fd, buffer.data(), std::min(buffer.size(), limit + 1 - result.size()));
        if (n < 0 && errno == EINTR) continue;
        require(n >= 0, system_error("read"));
        if (n == 0) return result;
        result.append(buffer.data(), static_cast<std::size_t>(n));
        require(result.size() <= limit, "file exceeds size limit");
    }
}

std::string read_text(const std::string& path, std::size_t limit = 65536) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    require(fd.value >= 0, system_error("open " + path));
    return read_fd(fd.value, limit);
}

std::uint64_t number(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    std::uint64_t result = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    require(error == std::errc{} && end == text.data() + text.size(), "invalid sysfs number");
    return result;
}

std::string canonical(const std::string& path) {
    std::unique_ptr<char, decltype(&std::free)> resolved(::realpath(path.c_str(), nullptr), &std::free);
    require(resolved != nullptr, system_error("resolve " + path));
    return resolved.get();
}

std::string basename(const std::string& path) {
    return path.substr(path.find_last_of('/') + 1);
}

bool device_name(const std::string& name) {
    return !name.empty() && name.size() <= 64 &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
            std::string::npos && name != "." && name != "..";
}

void check_directory(int fd, dev_t device = 0, bool android_cache_root = false, bool recovery_root = false) {
    struct stat st{};
    require(::fstat(fd, &st) == 0, system_error("stat directory"));
    // Android's cache filesystem root is system:cache 0770. Preserve its
    // ownership/mode; only this pinned cache mount root may use that schema.
    // Recovery settings are ordinary root-owned configuration. Preserve the
    // factory filesystem root's metadata; never make it writable to other users.
    const bool root_owned = st.st_uid == 0 && !(st.st_mode & 0022);
    // The kernel's initramfs root is root-owned sticky 1777. Accept that
    // upstream mode only for the initial "/" descriptor on a memory filesystem;
    // every component below it still goes through the ordinary no-write check.
    struct statfs root_fs{};
    const bool sticky_memory_root = recovery_root && st.st_uid == 0 && st.st_gid == 0 &&
        (st.st_mode & 07777) == 01777 && ::fstatfs(fd, &root_fs) == 0 &&
        (root_fs.f_type == TMPFS_MAGIC || root_fs.f_type == RAMFS_MAGIC);
    const bool cache_owned = android_cache_root && device != 0 && st.st_dev == device &&
        st.st_uid == 1000 && st.st_gid == 2001 && (st.st_mode & 07777) == 0770;
    require(S_ISDIR(st.st_mode) && (root_owned || sticky_memory_root || cache_owned),
            "directory must be root-owned and not writable by other users");
    require(device == 0 || st.st_dev == device, "directory crosses a filesystem boundary");
}

// Walk by directory descriptors: symlinks in either persistent path are never followed.
Fd child_directory(int parent, const char* name, bool create, dev_t device = 0, mode_t mode = 0700) {
    int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        require(::mkdirat(parent, name, mode) == 0 || errno == EEXIST,
                system_error("mkdir " + std::string(name)));
        require(::fsync(parent) == 0, system_error("sync new directory's parent"));
        fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0 && errno == ENOENT && !create) return Fd{};
    require(fd >= 0, system_error("open directory " + std::string(name)));
    Fd result(fd);
    check_directory(fd, device);
    return result;
}

Fd absolute_directory(const std::string& path, bool create, dev_t cache_root_device = 0) {
    require(!path.empty() && path.front() == '/', "directory must be absolute");
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(current.value >= 0, system_error("open root"));
    check_directory(current.value, 0, false, true);
    std::size_t start = 1;
    while (start < path.size()) {
        auto end = path.find('/', start);
        auto part = path.substr(start, end - start);
        require(!part.empty() && part != "." && part != "..", "unsafe directory path");
        if (end == std::string::npos && cache_root_device != 0) {
            Fd root(::openat(current.value, part.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            require(root.value >= 0, system_error("open cache mount root"));
            check_directory(root.value, cache_root_device, true);
            current = std::move(root);
        } else current = child_directory(current.value, part.c_str(), create);
        if (current.value < 0) return current;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return current;
}

// Every task writer must flock this SAME inode, holding it through RO restoration.
// Never unlink the lock; replacing it defeats flock. Separate opens also serialize threads.
struct Lock {
    Fd fd;
    Lock() {
        // Recovery's /init creates this shared runtime directory.
        Fd run = absolute_directory("/run/overpressure", false);
        require(run.value >= 0, "missing Recovery runtime directory");
        fd = Fd(::openat(run.value, "install.lock",
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
        require(fd.value >= 0, system_error("open install.lock"));
        struct stat st{};
        require(::fstat(fd.value, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                st.st_nlink == 1 && !(st.st_mode & 0022), "unsafe install.lock");
        if (::flock(fd.value, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                throw Failure("install.lock busy");
            throw Failure(system_error("flock install.lock"));
        }
    }
};

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos < line.size()) {
        auto start = line.find_first_not_of(" \t", pos);
        if (start == std::string::npos) break;
        pos = line.find_first_of(" \t", start);
        result.push_back(line.substr(start, pos - start));
        if (pos == std::string::npos) break;
    }
    return result;
}

bool option(const std::string& list, const std::string& value) {
    return ("," + list + ",").find("," + value + ",") != std::string::npos;
}

std::string unescape_mount(const std::string& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') {
            result += value[i];
            continue;
        }
        require(i + 3 < value.size(), "invalid mountinfo escape");
        unsigned n = 0;
        for (unsigned k = 1; k <= 3; ++k) {
            require(value[i + k] >= '0' && value[i + k] <= '7', "invalid mountinfo escape");
            n = n * 8 + static_cast<unsigned>(value[i + k] - '0');
        }
        require(n != 0 && n <= 255, "invalid mountinfo byte");
        result += static_cast<char>(n);
        i += 3;
    }
    return result;
}

struct MountInfo {
    std::uint64_t id = 0;
    dev_t device = 0;
    std::string root, path, type, options, super_options;
    bool shared = false;
    bool readonly() const { return option(options, "ro") && option(super_options, "ro"); }
};

std::vector<MountInfo> mounts() {
    const std::string text = read_text("/proc/self/mountinfo", 1024 * 1024);
    std::vector<MountInfo> result;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        auto fields = words(text.substr(pos, end - pos));
        require(fields.size() >= 10, "invalid mountinfo");
        auto split = std::find(fields.begin() + 6, fields.end(), "-");
        require(split != fields.end() && fields.end() - split == 4, "invalid mountinfo separator");
        auto colon = fields[2].find(':');
        require(colon != std::string::npos, "invalid mountinfo device");
        auto major_number = number(fields[2].substr(0, colon));
        auto minor_number = number(fields[2].substr(colon + 1));
        require(major_number <= 0xffffffffu && minor_number <= 0xffffffffu, "invalid device number");
        MountInfo entry;
        entry.id = number(fields[0]);
        entry.device = makedev(major_number, minor_number);
        entry.root = unescape_mount(fields[3]);
        entry.path = unescape_mount(fields[4]);
        entry.options = fields[5];
        entry.type = split[1];
        entry.super_options = split[3];
        entry.shared = std::any_of(fields.begin() + 6, split, [](const auto& field) {
            return field.starts_with("shared:") || field.starts_with("master:");
        });
        result.push_back(std::move(entry));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    require(!result.empty(), "empty mountinfo");
    return result;
}

std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (unsigned(p[1]) << 8));
}

struct Superblock {
    bool ext = false, ext4 = false, f2fs = false, clean = false;
    std::string label;
};

Superblock superblock(int fd, std::uint64_t size) {
    std::array<unsigned char, 1024> b{};
    std::size_t done = 0;
    while (done < b.size()) {
        auto n = ::pread(fd, b.data() + done, b.size() - done, 1024 + static_cast<off_t>(done));
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, system_error("read filesystem superblock"));
        done += static_cast<std::size_t>(n);
    }
    Superblock result;
    result.f2fs = le32(b.data()) == 0xf2f52010u;
    result.ext = le16(b.data() + 0x38) == 0xef53;
    if (!result.ext) return result;
    auto incompatible = le32(b.data() + 0x60);
    result.ext4 = (incompatible & 0x40) != 0; // extents: never write ext2/3 as ext4
    auto log_block_size = le32(b.data() + 0x18);
    require(log_block_size <= 6, "invalid ext block size");
    std::uint64_t blocks = le32(b.data() + 0x04);
    if (incompatible & 0x80) blocks |= std::uint64_t(le32(b.data() + 0x150)) << 32;
    const std::uint64_t block_size = std::uint64_t(1024) << log_block_size;
    require(blocks > 0 && blocks <= size / block_size, "filesystem exceeds actual partition size");
    auto label_end = std::find(b.begin() + 0x78, b.begin() + 0x88, 0);
    result.label.assign(reinterpret_cast<const char*>(b.data() + 0x78),
                        static_cast<std::size_t>(label_end - (b.begin() + 0x78)));
    result.clean = (le16(b.data() + 0x3a) & 3) == 1 && !(incompatible & 4) &&
                   le32(b.data() + 0xe8) == 0 && !(le32(b.data() + 0x64) & 0x10000);
    return result;
}

struct Device {
    std::string name, path, sys, parent_path, parent_sys;
    Fd fd, parent;
    dev_t id = 0, parent_id = 0;
    std::uint64_t bytes = 0, parent_bytes = 0;
    Superblock fs;

    explicit Device(const char* partition) : name(partition) {
        require(name == "userdata" || name == "cache", "partition not allowed");
        path = canonical("/dev/block/by-name/" + name);
        auto node = basename(path);
        require(device_name(node) && path == "/dev/" + node, "unsafe partition device");
        sys = canonical("/sys/class/block/" + node);
        require(read_text(sys + "/uevent").find("PARTNAME=" + name + "\n") != std::string::npos,
                "by-name link does not match kernel PARTNAME");
        require(number(read_text(sys + "/partition")) > 0, "device is not a partition");
        auto slash = sys.find_last_of('/');
        parent_sys = sys.substr(0, slash);
        auto disk = basename(parent_sys);
        require(device_name(disk) && disk != node, "invalid parent disk");
        parent_path = "/dev/" + disk;
        fd = Fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        parent = Fd(::open(parent_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        require(fd.value >= 0 && parent.value >= 0, system_error("open block devices"));
        struct stat part_st{}, disk_st{};
        require(::fstat(fd.value, &part_st) == 0 && S_ISBLK(part_st.st_mode) &&
                ::fstat(parent.value, &disk_st) == 0 && S_ISBLK(disk_st.st_mode),
                "partition and parent must be block devices");
        id = part_st.st_rdev;
        parent_id = disk_st.st_rdev;
        require(id != parent_id && canonical("/sys/dev/block/" + dev_string(id)) == sys &&
                canonical("/sys/dev/block/" + dev_string(parent_id)) == parent_sys,
                "sysfs and device identity differ");
        verify_size();
        fs = superblock(fd.value, bytes);
    }

    static std::string dev_string(dev_t id) {
        return std::to_string(major(id)) + ":" + std::to_string(minor(id));
    }

    void verify_size() {
        std::uint64_t part = 0, disk = 0;
        require(::ioctl(fd.value, BLKGETSIZE64, &part) == 0 &&
                ::ioctl(parent.value, BLKGETSIZE64, &disk) == 0, system_error("BLKGETSIZE64"));
        auto sectors = number(read_text(sys + "/size"));
        auto disk_sectors = number(read_text(parent_sys + "/size"));
        auto start = number(read_text(sys + "/start"));
        require(part >= 4096 && part % 512 == 0 && part / 512 == sectors &&
                disk % 512 == 0 && disk / 512 == disk_sectors && start <= disk_sectors &&
                sectors <= disk_sectors - start, "block size or partition geometry differs from sysfs");
        require(bytes == 0 || (bytes == part && parent_bytes == disk), "block size changed");
        bytes = part;
        parent_bytes = disk;
    }

    bool ro(int descriptor, const std::string& syspath, bool expected) const {
        int value = -1;
        return ::ioctl(descriptor, BLKROGET, &value) == 0 &&
               (value == 0 || value == 1) && value == int(expected) &&
               number(read_text(syspath + "/ro")) == std::uint64_t(expected);
    }

    void verify_ro(bool expected) const {
        bool part_ro = false, disk_ro = false;
        try { part_ro = ro(fd.value, sys, expected); } catch (const Failure&) {}
        try { disk_ro = ro(parent.value, parent_sys, expected); } catch (const Failure&) {}
        require(part_ro && disk_ro,
                expected ? "partition or parent is not read-only" : "partition or parent stays read-only");
    }

    void verify_filesystem(bool writing) {
        verify_size();
        auto current = superblock(fd.value, bytes);
        require(current.ext4 && current.label == fs.label, "ext4 filesystem or label changed");
        require(name == "userdata" ? current.label == "pico4-data" :
                (current.label.empty() || current.label == "cache"), "filesystem label not allowed");
        require(!writing || current.clean, "filesystem needs recovery; no repair or journal replay permitted");
    }
};

struct Mounted {
    Device& device;
    Fd root;
    std::string path;
    std::uint64_t mount_id = 0;
    bool owned = false, restore = false, finished = false;

    Mounted(Device& dev, bool writing) : device(dev) {
        try {
            device.verify_ro(true);
            device.verify_filesystem(writing);
            auto entries = mounts();
            const MountInfo* borrowed = nullptr;
            for (const auto& entry : entries) {
                if (entry.device != device.id) continue;
                require(!writing, "partition is already mounted; never remount an existing mount");
                require(entry.readonly() && entry.type == "ext4", "existing mount is not read-only ext4");
                if (entry.root == "/" && borrowed == nullptr) borrowed = &entry;
            }
            if (borrowed) {
                path = borrowed->path;
                mount_id = borrowed->id;
            } else {
                require(std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
                    return entry.path == mountpoint;
                }), "private mountpoint is already occupied");
                // Do not propagate our initial mount into another namespace.
                const MountInfo* container = nullptr;
                for (const auto& entry : entries) {
                    if (entry.path == "/" || std::string(mountpoint).starts_with(entry.path + "/")) {
                        if (!container || entry.path.size() > container->path.size()) container = &entry;
                    }
                }
                require(container && !container->shared, "private mountpoint has shared propagation");
                Fd directory = absolute_directory(mountpoint, true);
                struct stat st{};
                require(::fstat(directory.value, &st) == 0 && (st.st_mode & 0777) == 0700,
                        "private mountpoint must have mode 0700");
                path = mountpoint;
                if (writing) {
                    restore = true; // Arm cleanup BEFORE the first ioctl, including partial failures.
                    int rw = 0;
                    require(::ioctl(device.fd.value, BLKROSET, &rw) == 0, system_error("partition BLKROSET rw"));
                    require(::ioctl(device.parent.value, BLKROSET, &rw) == 0, system_error("parent BLKROSET rw"));
                    device.verify_ro(false);
                    device.verify_filesystem(true);
                }
                auto source = "/proc/self/fd/" + std::to_string(device.fd.value);
                unsigned long flags = MS_NODEV | MS_NOSUID | MS_NOEXEC | MS_NOATIME;
                if (!writing) flags |= MS_RDONLY;
                require(::mount(source.c_str(), path.c_str(), "ext4", flags, "noload") == 0,
                        system_error("mount ext4 noload"));
                owned = true;
                auto after = mounts();
                auto mounted = std::find_if(after.begin(), after.end(), [&](const auto& entry) {
                    return entry.path == path;
                });
                require(mounted != after.end(), "new mount absent from mountinfo");
                mount_id = mounted->id;
                require(::mount(nullptr, path.c_str(), nullptr, MS_PRIVATE, nullptr) == 0,
                        system_error("make mount private"));
            }
            verify_mount(writing);
            root = absolute_directory(path, false, device.name == "cache" ? device.id : 0);
            require(root.value >= 0, "mounted directory disappeared");
            struct stat st{};
            struct statfs fs{};
            require(::fstat(root.value, &st) == 0 && st.st_dev == device.id &&
                    ::fstatfs(root.value, &fs) == 0 && fs.f_type == EXT4_SUPER_MAGIC,
                    "mounted filesystem identity differs");
        } catch (...) {
            auto original = std::current_exception();
            auto cleanup = finish();
            if (!cleanup.empty()) throw Failure("mount failed; " + cleanup);
            std::rethrow_exception(original);
        }
    }

    ~Mounted() { if (!finished) finish(); }

    void verify_mount(bool writing) const {
        auto entries = mounts();
        unsigned count = 0;
        for (const auto& entry : entries) {
            if (entry.path != path) continue;
            ++count;
            require(entry.id == mount_id && entry.device == device.id && entry.root == "/" &&
                    entry.type == "ext4" && !entry.shared &&
                    (writing ? option(entry.options, "rw") && option(entry.super_options, "rw") :
                               entry.readonly()), "mount identity or access mode differs");
        }
        require(count == 1, "mountpoint is missing or overmounted");
    }

    std::string finish() noexcept {
        if (finished) return {};
        finished = true;
        root = Fd{};
        std::string error;
        try {
            if (owned) {
                auto entries = mounts();
                auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                    return entry.path == path;
                });
                require(it != entries.end() && it->device == device.id &&
                        (mount_id == 0 || it->id == mount_id) &&
                        std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
                            return entry.path == path;
                        }) == 1, "owned mount changed; refusing to unmount another mount");
                require(::umount2(path.c_str(), 0) == 0, system_error("unmount preferences"));
            }
        } catch (const std::exception& e) { error = e.what(); }
        // Even an unmount/inspection failure must not skip either RO restoration.
        if (restore) {
            int ro = 1;
            bool part_ok = false, disk_ok = false;
            for (int attempt = 0; attempt != 3; ++attempt) {
                part_ok = ::ioctl(device.fd.value, BLKROSET, &ro) == 0;
                disk_ok = ::ioctl(device.parent.value, BLKROSET, &ro) == 0;
                if (part_ok && disk_ok) break;
            }
            if (!part_ok || !disk_ok) error += "; BLKROSET ro failed";
        }
        try {
            device.verify_ro(true);
            device.verify_size();
        } catch (const std::exception& e) { error += "; "; error += e.what(); }
        return error;
    }
};

Fd config_directory(Mounted& mounted, bool create) {
    Fd dir(::dup(mounted.root.value));
    require(dir.value >= 0, system_error("dup mount root"));
    if (mounted.device.name == "userdata") {
        // These are ordinary configuration directories, including the shared
        // /var and /var/lib used by SteamOS after installation.
        for (const auto& [component, mode] : std::array<std::pair<const char*, mode_t>, 4>{{
                {"var", 0755}, {"lib", 0755}, {"pico4", 0755}, {"recovery", 0755}}}) {
            dir = child_directory(dir.value, component, create, mounted.device.id, mode);
            if (dir.value < 0) return dir;
        }
    } else {
        dir = child_directory(dir.value, "pico4-recovery", create, mounted.device.id, 0755);
    }
    return dir;
}

std::optional<std::string> config_bytes(int directory) {
    if (directory < 0) return std::nullopt;
    Fd file(::openat(directory, filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.value < 0 && errno == ENOENT) return std::nullopt;
    require(file.value >= 0, system_error("open preferences.json"));
    struct stat st{};
    require(::fstat(file.value, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
            st.st_uid == 0 && !(st.st_mode & 0022) && st.st_size >= 0 &&
            static_cast<std::uint64_t>(st.st_size) <= json_limit, "unsafe preferences.json");
    return read_fd(file.value, json_limit);
}

Json parse(const std::string& bytes) {
    require(!bytes.empty() && bytes.size() <= json_limit, "invalid preferences size");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> tok(json_tokener_new_ex(16), &json_tokener_free);
    require(tok != nullptr, "json-c allocation failed");
    json_tokener_set_flags(tok.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json object(json_tokener_parse_ex(tok.get(), bytes.c_str(), static_cast<int>(bytes.size())));
    require(json_tokener_get_error(tok.get()) == json_tokener_success && object &&
            json_object_is_type(object.get(), json_type_object), "invalid preferences JSON");
    auto end = json_tokener_get_parse_end(tok.get());
    require(bytes.find_first_not_of(" \t\r\n", end) == std::string::npos, "trailing preferences content");
    json_object *version = nullptr, *locale = nullptr, *selected = nullptr;
    require(json_object_object_get_ex(object.get(), "version", &version) &&
            json_object_is_type(version, json_type_int) && json_object_get_int64(version) == 1,
            "unsupported preferences version");
    require(json_object_object_get_ex(object.get(), "locale", &locale) &&
            json_object_is_type(locale, json_type_string) && json_object_get_string_len(locale) > 0 &&
            json_object_get_string_len(locale) <= 64 &&
            std::strlen(json_object_get_string(locale)) == static_cast<std::size_t>(json_object_get_string_len(locale)) &&
            json_object_object_get_ex(object.get(), "language_selected", &selected) &&
            json_object_is_type(selected, json_type_boolean), "invalid preferences fields");
    return object;
}

Preferences decoded(json_object* object, const std::string& storage) {
    Preferences result;
    result.storage = storage;
    json_object *locale = nullptr, *selected = nullptr;
    json_object_object_get_ex(object, "locale", &locale);
    json_object_object_get_ex(object, "language_selected", &selected);
    std::string saved = json_object_get_string(locale);
    if (!supported(saved)) {
        result.error = "Unsupported saved locale; choose a supported language again";
        return result;
    }
    result.locale = saved;
    json_object* settings=nullptr;
    if (json_object_object_get_ex(object,"settings",&settings)) {
        const auto len=json_object_is_type(settings,json_type_string)?json_object_get_string_len(settings):0;
        require(len>0 && decode_settings({json_object_get_string(settings),static_cast<std::size_t>(len)},result)
                && result.locale==saved,"invalid settings fields");
    }
    result.language_selected = json_object_get_boolean(selected);
    result.durable = true;
    return result;
}

std::optional<std::string> read_config(Device& device) {
    Mounted mounted(device, false);
    try {
        auto dir = config_directory(mounted, false);
        auto bytes = config_bytes(dir.value);
        dir = Fd{};
        mounted.verify_mount(false);
        auto cleanup = mounted.finish();
        require(cleanup.empty(), cleanup);
        return bytes;
    } catch (...) {
        auto original = std::current_exception();
        auto cleanup = mounted.finish();
        if (!cleanup.empty()) throw Failure("read failed; " + cleanup);
        std::rethrow_exception(original);
    }
}

void atomic_write(int directory, const std::string& bytes, bool no_replace) {
    static std::atomic<unsigned long> serial{0};
    std::string temporary = ".preferences.json.tmp." + std::to_string(::getpid()) + "." +
                            std::to_string(serial.fetch_add(1));
    Fd file(::openat(directory, temporary.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    require(file.value >= 0, system_error("create preferences temporary"));
    bool renamed = false;
    try {
        std::size_t done = 0;
        while (done < bytes.size()) {
            auto n = ::write(file.value, bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, system_error("write preferences temporary"));
            done += static_cast<std::size_t>(n);
        }
        require(::fsync(file.value) == 0, system_error("sync preferences temporary"));
        int descriptor = std::exchange(file.value, -1);
        require(::close(descriptor) == 0, system_error("close preferences temporary"));
        if (no_replace) {
            require(::syscall(SYS_renameat2, directory, temporary.c_str(), directory, filename,
                              RENAME_NOREPLACE) == 0, system_error("migrate without replacing preferences"));
        } else {
            require(::renameat(directory, temporary.c_str(), directory, filename) == 0,
                    system_error("rename preferences"));
        }
        renamed = true;
        require(::fsync(directory) == 0, system_error("sync preferences directory"));
        require(config_bytes(directory) == std::optional<std::string>(bytes), "preferences readback differs");
    } catch (...) {
        if (!renamed) ::unlinkat(directory, temporary.c_str(), 0); // Only our own temporary.
        throw;
    }
}

std::string serialized(json_object* object) {
    const char* text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    require(text != nullptr, "json-c serialization failed");
    std::string result = std::string(text) + "\n";
    require(result.size() <= json_limit, "preferences JSON too large");
    return result;
}

Preferences write_config(Device& device, json_object* migration, const std::string& locale,
                         const Preferences* wanted=nullptr) {
    const std::string storage = device.name == "userdata" ? "data" : "cache";
    Mounted mounted(device, true);
    std::string bytes;
    try {
        auto dir = config_directory(mounted, true);
        auto old = config_bytes(dir.value);
        Json object;
        if (migration) {
            require(!old, "data preferences appeared; migration refused");
            object.reset(json_object_get(migration));
        } else {
            // Preserve extension fields, but never destroy a malformed/future-version config.
            object = old ? parse(*old) : Json(json_object_new_object());
            require(object != nullptr, "json-c allocation failed");
            if (old) (void)decoded(object.get(),storage);
            require(json_object_object_add(object.get(), "version", json_object_new_int(1)) == 0 &&
                    json_object_object_add(object.get(), "locale", json_object_new_string(locale.c_str())) == 0 &&
                    json_object_object_add(object.get(), "language_selected", json_object_new_boolean(true)) == 0,
                    "json-c allocation failed");
            json_object* existing_settings=nullptr;
            if (wanted || json_object_object_get_ex(object.get(),"settings",&existing_settings)) {
                Preferences merged;
                if (wanted) merged=*wanted;
                else {
                    require(decode_settings(json_object_get_string(existing_settings),merged),"invalid prior settings");
                    merged.locale=locale;
                }
                const auto packed=encode_settings(merged);
                require(json_object_object_add(object.get(),"settings",json_object_new_string(packed.c_str()))==0,
                        "json-c allocation failed");
            }
        }
        bytes = serialized(object.get());
        mounted.verify_mount(true);
        device.verify_ro(false);
        // A clean ext4 becomes dirty while mounted RW; only the PRE-mount check
        // requires clean state. noload plus that check prevents journal recovery.
        device.verify_filesystem(false);
        atomic_write(dir.value, bytes, migration != nullptr);
        dir = Fd{};
        auto cleanup = mounted.finish();
        require(cleanup.empty(), cleanup);
    } catch (...) {
        auto original = std::current_exception();
        auto cleanup = mounted.finish();
        if (!cleanup.empty()) throw Failure("save failed; " + cleanup);
        std::rethrow_exception(original);
    }
    // Verify again from a fresh read-only mount AFTER unmount and both BLKROSETs.
    require(read_config(device) == std::optional<std::string>(bytes), "read-only preferences readback differs");
    auto verified = parse(bytes);
    auto result = decoded(verified.get(), storage);
    require(result.durable, "saved locale is not in the language catalog");
    return result;
}

bool installed(const Device& data) {
    if (data.fs.ext && data.fs.label == "pico4-data") {
        require(data.fs.ext4, "pico4-data is not ext4");
        return true;
    }
    // A failed/unknown probe is NOT evidence of a preinstall system.
    require(data.fs.ext || data.fs.f2fs, "userdata filesystem unknown; refusing cache fallback");
    return false;
}

Preferences installer_reply(const std::string& bytes, int status, const std::string& requested) {
    auto object = parse(bytes);
    json_object *storage = nullptr, *error = nullptr, *durable = nullptr;
    require(json_object_object_get_ex(object.get(), "storage", &storage) &&
            json_object_is_type(storage, json_type_string) &&
            json_object_object_get_ex(object.get(), "error", &error) &&
            json_object_is_type(error, json_type_string) &&
            json_object_object_get_ex(object.get(), "durable", &durable) &&
            json_object_is_type(durable, json_type_boolean), "invalid installer preferences response");
    Preferences result = decoded(object.get(), json_object_get_string(storage));
    require(std::strlen(json_object_get_string(storage)) ==
            static_cast<std::size_t>(json_object_get_string_len(storage)), "invalid installer storage string");
    require(result.storage == "data" || result.storage == "cache" || result.storage == "none",
            "invalid installer storage");
    const char* detail = json_object_get_string(error);
    require(std::strlen(detail) == static_cast<std::size_t>(json_object_get_string_len(error)) &&
            json_object_get_string_len(error) <= 2048, "invalid installer error");
    result.error = detail;
    result.durable = json_object_get_boolean(durable);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        require(result.durable && result.language_selected && result.locale == requested &&
                result.storage != "none" && result.error.empty(), "installer did not verify the requested choice");
    } else {
        result.durable = false;
        require(!result.error.empty(), "installer failed without an error");
    }
    return result;
}

void reap_later(pid_t pid) {
    // Never SIGKILL a writer with temporary RW flags. The transaction owns the
    // lock until its cleanup completes, even if the UI's response deadline expires.
    std::thread([pid] {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }).detach();
}

Preferences run_installer(const std::string& locale,const Preferences* wanted=nullptr) {
    const auto payload=wanted?encode_settings(*wanted):locale;
    int descriptors[2]{-1, -1};
    require(::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) == 0, system_error("preferences response pipe"));
    Fd input(descriptors[0]), output(descriptors[1]);
    pid_t pid = ::fork();
    require(pid >= 0, system_error("fork pico4-install"));
    if (pid == 0) {
        int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0 || ::dup2(null, STDIN_FILENO) < 0 ||
            ::dup2(output.value, STDOUT_FILENO) < 0 || ::dup2(null, STDERR_FILENO) < 0)
            ::_exit(126);
        ::fcntl(STDOUT_FILENO, F_SETFL, 0);
        // Do not leak the compositor's DRM, input, socket or directory descriptors.
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0) {
            long limit = ::sysconf(_SC_OPEN_MAX);
            for (long fd = 3; fd < limit; ++fd) ::close(static_cast<int>(fd));
        }
        char* argv[] = {const_cast<char*>("/usr/bin/pico4-install"),
                        const_cast<char*>(wanted?"settings-set":"preferences-set"),
                        const_cast<char*>(payload.c_str()), nullptr};
        char* env[] = {const_cast<char*>("PATH=/usr/sbin:/usr/bin:/sbin:/bin"),
                       const_cast<char*>("LC_ALL=C"), const_cast<char*>("HOME=/root"), nullptr};
        ::execve(argv[0], argv, env);
        ::_exit(127);
    }
    output = Fd{};
    bool reaped = false;
    int status = 0;
    std::string bytes;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    try {
        bool eof = false;
        while (!reaped || !eof) {
            require(std::chrono::steady_clock::now() < deadline,
                    "Preferences save timeout; the transaction may still finish in the background. Check again later.");
            pollfd poller{input.value, POLLIN | POLLHUP, 0};
            int ready = ::poll(&poller, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            require(ready >= 0, system_error("poll installer response"));
            if (ready > 0) {
                std::array<char, 1024> buffer{};
                auto n = ::read(input.value, buffer.data(), buffer.size());
                if (n == 0) eof = true;
                else if (n > 0) {
                    bytes.append(buffer.data(), static_cast<std::size_t>(n));
                    require(bytes.size() <= json_limit, "installer response exceeds limit");
                } else {
                    require(errno == EAGAIN || errno == EINTR, system_error("read installer response"));
                }
            }
            if (!reaped) {
                pid_t waited = ::waitpid(pid, &status, WNOHANG);
                require(waited >= 0 || errno == EINTR, system_error("wait for installer"));
                reaped = waited == pid;
            }
        }
    } catch (...) {
        if (!reaped) reap_later(pid);
        throw;
    }
    auto result=installer_reply(bytes,status,locale);
    if (wanted && result.durable) require(same_settings(result,*wanted),"settings readback does not match request");
    return result;
}

} // namespace

std::vector<std::string> supported_locales() {
    return {catalog.begin(), catalog.end()};
}

Preferences load_preferences() {
    Preferences result;
    try {
        Lock lock;
        Device data("userdata");
        data.verify_ro(true);
        if (!installed(data)) {
            result.storage = "cache";
            Device cache("cache");
            auto bytes = read_config(cache);
            if (bytes) {
                auto object = parse(*bytes);
                result = decoded(object.get(), "cache");
            }
            return result;
        }
        result.storage = "data";
        auto bytes = read_config(data);
        if (bytes) {
            auto object = parse(*bytes);
            return decoded(object.get(), "data");
        }
        // A confirmed cache choice survives format-data and remains a durable
        // fallback until the next explicit save migrates it under its own lock.
        // An existing data configuration always wins; load never writes or
        // clears the cache, and never takes over an installer's inherited lock.
        Device cache("cache");
        auto cached = read_config(cache);
        if (cached) {
            auto object = parse(*cached);
            result = decoded(object.get(), "cache");
        }
    } catch (const std::exception& e) {
        result.durable = false;
        result.error = e.what();
    }
    return result;
}

// Called ONLY by the early --preferences-store CLI reached through pico4-install.
// The task agent must not hold install.lock across this exec: this transaction owns it.
static bool store_native(const std::string& locale, Preferences& result,const Preferences* wanted) {
    result = Preferences{};
    try {
        require(supported(locale), unsupported_choice);
        Lock lock;
        const auto publish=[&] {
            if(!wanted)return;
            auto run=absolute_directory("/run/overpressure",false);
            require(run.value>=0,"missing Recovery runtime directory");
            // This second receipt is an atomic tmpfs handoff, not a factory write.
            const std::string tmp="timeout-settings."+std::to_string(::getpid());
            Fd fd(::openat(run.value,tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
            require(fd.value>=0,system_error("open timeout handoff"));
            try {
                const auto bytes=std::to_string(result.connect)+" "+std::to_string(result.idle)+"\n";
                require(::write(fd.value,bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size()),
                        system_error("write timeout handoff"));
                require(::renameat(run.value,tmp.c_str(),run.value,"timeout-settings")==0,system_error("publish timeout handoff"));
            } catch (...) { ::unlinkat(run.value,tmp.c_str(),0);throw; }
        };
        Device data("userdata");
        data.verify_ro(true);
        if (installed(data)) {
            result.storage = "data";
            auto old = read_config(data);
            if (!old) {
                Device cache("cache");
                auto cached = read_config(cache);
                if (cached) {
                    auto object = parse(*cached);
                    auto choice = decoded(object.get(), "data");
                    if (choice.durable && choice.language_selected && choice.locale == locale &&
                        (!wanted || same_settings(choice,*wanted))) {
                        result = write_config(data, object.get(), locale);
                        publish();
                        return true;
                    }
                }
            }
            result = write_config(data, nullptr, locale,wanted);
        } else {
            result.storage = "cache";
            Device cache("cache");
            result = write_config(cache, nullptr, locale,wanted);
        }
        publish();
        return true;
    } catch (const std::exception& e) {
        result.durable = false;
        result.error = e.what();
        return false;
    }
}

bool save_preferences_native(const std::string& locale,Preferences& result) {
    return store_native(locale,result,nullptr);
}
bool save_settings_native(const Preferences& wanted,Preferences& result) {
    Preferences parsed;
    if (!decode_settings(encode_settings(wanted),parsed)) {
        result=Preferences{};result.error="invalid settings request";return false;
    }
    return store_native(wanted.locale,result,&wanted);
}
bool save_settings(const Preferences& wanted,Preferences& result) {
    result=Preferences{};
    try {
        Preferences checked;
        require(decode_settings(encode_settings(wanted),checked),"invalid settings request");
        result=run_installer(wanted.locale,&wanted);
        return result.durable && result.error.empty();
    } catch (const std::exception& e) {
        result.error=e.what();return false;
    }
}

bool save_preferences(const std::string& locale, Preferences& result) {
    result = Preferences{};
    try {
        require(supported(locale), unsupported_choice);
        result = run_installer(locale);
        return result.durable && result.error.empty();
    } catch (const std::exception& e) {
        result.durable = false;
        result.error = e.what();
        return false;
    }
}

} // namespace p4ui
