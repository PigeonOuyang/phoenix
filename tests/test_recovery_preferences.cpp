// SPDX-License-Identifier: Unlicense
// Synthetic syscall boundary tests. NO mounts, block ioctls or hardware access.
#include "preferences.cpp"
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace fs = std::filesystem;
namespace {
const std::string private_mount = "/run/overpressure/ui-preferences";
std::string fixture;
std::map<int, std::string> fd_paths;
std::map<std::string, int> calls;
std::string fault;
int fault_at = 1;
bool userdata_f2fs = false, part_ro = true, parent_ro = true, cache_ro = true;
bool size_mismatch = false, wrong_partname = false, wrong_fs = false, ro_mount_fallback = false;
bool post_ro_mismatch = false, raced = false, mounted = false, mount_rw = false;
bool inherited_mount = false, inherited_rw = false, bind_only = false, shared_root = false;
bool android_cache_root = false, unsafe_cache_root = false;
bool rw_saw_dirty = false;
std::string mount_device;
int mounts_rw = 0, mounts_ro = 0, ro_sets_part = 0, ro_sets_parent = 0;
std::string reply;
int reply_status = 0;
mode_t recovery_root_mode = 0755;
uid_t recovery_root_uid = 0;
long recovery_root_type = TMPFS_MAGIC;
std::string writable_runtime_directory;

void check(bool b, const std::string& message) {
    if (!b) throw std::runtime_error(message);
}

void put(const std::string& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    check(bool(file), "fixture write failed");
}

std::string get(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string block_name(const std::string& path) {
    if (path == "/dev/sda1") return "userdata";
    if (path == "/dev/sda2") return "cache";
    if (path == "/dev/sda") return "parent";
    return {};
}

dev_t block_id(const std::string& name) {
    return makedev(8, name == "userdata" ? 1 : name == "cache" ? 2 : 0);
}

std::string backing(const std::string& path) {
    if (path == "/dev/null") return path;
    if (mounted && (path == private_mount || path.starts_with(private_mount + "/")))
        return fixture + "/volumes/" + mount_device + path.substr(private_mount.size());
    if (inherited_mount && (path == "/mnt/borrowed" || path.starts_with("/mnt/borrowed/")))
        return fixture + "/volumes/userdata" + path.substr(std::string("/mnt/borrowed").size());
    return fixture + path;
}

std::string virtual_at(int directory, const char* path) {
    if (path[0] == '/') return path;
    auto it = fd_paths.find(directory);
    check(it != fd_paths.end(), "untracked directory FD");
    return it->second == "/" ? "/" + std::string(path) : it->second + "/" + path;
}

bool fails(const std::string& operation) {
    return ++calls[operation] == fault_at && fault == operation;
}

void update_sysfs() {
    put(fixture + "/sys/devices/fake/sda/ro", parent_ro ? "1\n" : "0\n");
    put(fixture + "/sys/devices/fake/sda/sda1/ro", (part_ro || parent_ro) ? "1\n" : "0\n");
    put(fixture + "/sys/devices/fake/sda/sda2/ro", (cache_ro || parent_ro) ? "1\n" : "0\n");
}

void update_mountinfo() {
    std::string text = "1 0 0:1 / / rw ";
    if (shared_root) text += "shared:1 ";
    text += "- tmpfs tmpfs rw\n";
    if (inherited_mount) {
        std::string options = inherited_rw ? "rw" : "ro";
        text += "40 1 8:1 " + std::string(bind_only ? "/subdir" : "/") +
                " /mnt/borrowed " + options + " - ext4 /dev/sda1 " + options + ",norecovery\n";
    }
    if (mounted) {
        std::string options = mount_rw ? "rw" : "ro";
        text += "50 1 " + p4ui::Device::dev_string(block_id(mount_device)) + " / " +
                private_mount + " " + options + " - ext4 /dev/fake " + options + ",norecovery\n";
    }
    put(fixture + "/proc/self/mountinfo", text);
}

void ext_image(const std::string& name, const std::string& label, bool clean = true) {
    std::string bytes(1024 * 1024, '\0');
    auto set32 = [&](std::size_t offset, std::uint32_t v) {
        for (int i = 0; i != 4; ++i) bytes[1024 + offset + i] = char(v >> (i * 8));
    };
    set32(4, 256);
    set32(0x18, 2);
    set32(0x60, 0x40);
    bytes[1024 + 0x38] = char(0x53);
    bytes[1024 + 0x39] = char(0xef);
    bytes[1024 + 0x3a] = clean ? 1 : 0;
    std::copy(label.begin(), label.end(), bytes.begin() + 1024 + 0x78);
    put(fixture + "/dev/" + (name == "userdata" ? "sda1" : "sda2"), bytes);
}

void clean_state(const std::string& name, bool clean) {
    auto path = fixture + "/dev/" + (name == "userdata" ? "sda1" : "sda2");
    auto bytes = get(path);
    bytes[1024 + 0x3a] = clean ? 1 : 0;
    put(path, bytes);
}

const std::string config = "{\"version\":1,\"locale\":\"zh-CN\",\"language_selected\":true}\n";

std::string config_path(const std::string& name) {
    return fixture + "/volumes/" + name + (name == "userdata" ? "/var/lib/pico4/recovery" : "/pico4-recovery") +
           "/preferences.json";
}

void reset(const std::string& name) {
    fixture = fs::current_path().string() + "/fixtures/" + name;
    fs::remove_all(fixture); // Test-owned synthetic directories only.
    fs::create_directories(fixture);
    fd_paths.clear();
    calls.clear();
    fault.clear();
    fault_at = 1;
    userdata_f2fs = false;
    part_ro = parent_ro = cache_ro = true;
    size_mismatch = wrong_partname = wrong_fs = ro_mount_fallback = post_ro_mismatch = raced = false;
    mounted = mount_rw = inherited_mount = inherited_rw = bind_only = shared_root = rw_saw_dirty = false;
    mount_device.clear();
    mounts_rw = mounts_ro = ro_sets_part = ro_sets_parent = 0;
    reply = config.substr(0, config.size() - 2) + ",\"storage\":\"data\",\"durable\":true,\"error\":\"\"}\n";
    reply_status = 0;
    recovery_root_mode = 0755;
    recovery_root_uid = 0;
    recovery_root_type = TMPFS_MAGIC;
    writable_runtime_directory.clear();
    for (const auto& dir : {"/run/overpressure", "/sys/devices/fake/sda", "/volumes/userdata",
                           "/volumes/cache", "/mnt/borrowed", "/run/overpressure/ui-preferences"})
        fs::create_directories(fixture + dir);
    fs::permissions(fixture + private_mount, fs::perms::owner_all);
    put(fixture + "/dev/sda", std::string(4096, '\0'));
    for (int i = 1; i != 3; ++i) {
        std::string base = fixture + "/sys/devices/fake/sda/sda" + std::to_string(i);
        put(base + "/size", "2048\n");
        put(base + "/start", i == 1 ? "2048\n" : "4096\n");
        put(base + "/partition", std::to_string(i) + "\n");
        put(base + "/uevent", std::string("MAJOR=8\nPARTNAME=") + (i == 1 ? "userdata" : "cache") + "\n");
    }
    put(fixture + "/sys/devices/fake/sda/size", "8192\n");
    ext_image("userdata", "pico4-data");
    ext_image("cache", "cache");
    update_sysfs();
    update_mountinfo();
}

void preinstall() {
    userdata_f2fs = true;
    auto bytes = get(fixture + "/dev/sda1");
    bytes[1024] = char(0x10);
    bytes[1025] = char(0x20);
    bytes[1026] = char(0xf5);
    bytes[1027] = char(0xf2);
    bytes[1024 + 0x38] = 0;
    bytes[1024 + 0x39] = 0;
    put(fixture + "/dev/sda1", bytes);
}

void readonly_after_failure() {
    check(part_ro && parent_ro && cache_ro, "failure left writable flags");
}

void no_temporary(const std::string& volume) {
    for (const auto& entry : fs::recursive_directory_iterator(fixture + "/volumes/" + volume))
        check(entry.path().filename().string().find(".tmp.") == std::string::npos, "temporary not cleaned");
}
} // namespace

extern "C" {
int __real_open(const char*, int, ...);
int __real_openat(int, const char*, int, ...);
int __real_close(int);
int __real_dup(int);
int __real_fstat(int, struct stat*);
int __real_fstatfs(int, struct statfs*);
int __real_mkdirat(int, const char*, mode_t);
int __real_fsync(int);
int __real_renameat(int, const char*, int, const char*);
long __real_syscall(long, ...);
char* __real_realpath(const char*, char*);
int __real_execve(const char*, char* const[], char* const[]);

int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (fails("open") || (fault == "open-temp" && std::string(path).find(".tmp.") != std::string::npos)) {
        errno = EIO;
        return -1;
    }
    int fd = __real_open(backing(path).c_str(), flags, mode);
    if (fd >= 0) fd_paths[fd] = path;
    return fd;
}

int __wrap_openat(int directory, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return __wrap_open(virtual_at(directory, path).c_str(), flags, mode);
}

int __wrap_close(int fd) {
    bool temporary = fd_paths.contains(fd) && fd_paths[fd].find(".tmp.") != std::string::npos;
    fd_paths.erase(fd);
    int result = __real_close(fd);
    if (temporary && fails("close-temp")) { errno = EIO; return -1; }
    return result;
}

int __wrap_dup(int fd) {
    int result = __real_dup(fd);
    if (result >= 0 && fd_paths.contains(fd)) fd_paths[result] = fd_paths[fd];
    return result;
}

int __wrap_fstat(int fd, struct stat* st) {
    int result = __real_fstat(fd, st);
    if (result == 0 && fd_paths.contains(fd)) {
        const auto& path = fd_paths[fd];
        st->st_uid = 0; // Production runs as recovery root; fixtures are unprivileged.
        // DrvFS does not preserve Unix modes. Model the recovery modes here;
        // O_NOFOLLOW, regular-file type and link counts still use real fixture inodes.
        st->st_mode = (st->st_mode & S_IFMT) |
                      (S_ISDIR(st->st_mode) ? (path == private_mount ? 0700 : 0755) : 0600);
        if (path == "/") {
            st->st_mode = S_IFDIR | recovery_root_mode;
            st->st_uid = recovery_root_uid;
            st->st_gid = 0;
        }
        if (path == writable_runtime_directory) st->st_mode = S_IFDIR | 01777;
        auto block = block_name(path);
        if (!block.empty()) {
            st->st_mode = S_IFBLK | 0600;
            st->st_rdev = block_id(block);
        } else if (mounted && (path == private_mount || path.starts_with(private_mount + "/"))) {
            st->st_dev = block_id(mount_device);
            if (path == private_mount && mount_device == "cache" && android_cache_root) {
                st->st_uid = 1000; st->st_gid = 2001;
                st->st_mode = S_IFDIR | (unsafe_cache_root ? 0777 : 0770);
            }
        } else if (inherited_mount && (path == "/mnt/borrowed" || path.starts_with("/mnt/borrowed/"))) {
            st->st_dev = block_id("userdata");
        }
    }
    return result;
}

int __wrap_fstatfs(int fd, struct statfs* st) {
    if (fd_paths.contains(fd) && fd_paths[fd] == "/" && fails("root-fstatfs")) {
        errno = EIO;
        return -1;
    }
    std::memset(st, 0, sizeof(*st));
    st->f_type = fd_paths.contains(fd) && fd_paths[fd] == "/" ? recovery_root_type :
        wrong_fs ? TMPFS_MAGIC : EXT4_SUPER_MAGIC;
    return 0;
}

char* __wrap_realpath(const char* raw, char* buffer) {
    std::string path = raw, resolved;
    if (path == "/dev/block/by-name/userdata") resolved = "/dev/sda1";
    else if (path == "/dev/block/by-name/cache") resolved = "/dev/sda2";
    else if (path == "/sys/class/block/sda1" || path == "/sys/dev/block/8:1")
        resolved = "/sys/devices/fake/sda/sda1";
    else if (path == "/sys/class/block/sda2" || path == "/sys/dev/block/8:2")
        resolved = "/sys/devices/fake/sda/sda2";
    else if (path == "/sys/dev/block/8:0") resolved = "/sys/devices/fake/sda";
    else return __real_realpath(raw, buffer);
    if (wrong_partname && path == "/dev/block/by-name/userdata") resolved = "/dev/sda2";
    if (fails("realpath")) { errno = ENOENT; return nullptr; }
    if (buffer) return std::strcpy(buffer, resolved.c_str());
    return ::strdup(resolved.c_str());
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* value = va_arg(ap, void*);
    va_end(ap);
    check(fd_paths.contains(fd) && !block_name(fd_paths[fd]).empty(), "unexpected ioctl");
    auto name = block_name(fd_paths[fd]);
    if (request == BLKGETSIZE64) {
        if (fails("size")) { errno = EIO; return -1; }
        *static_cast<std::uint64_t*>(value) = name == "parent" ? 4 * 1024 * 1024 : 1024 * 1024;
        if (size_mismatch && name == "userdata") *static_cast<std::uint64_t*>(value) += 512;
        return 0;
    }
    if (request == BLKROGET) {
        if (fails("getro")) { errno = EIO; return -1; }
        bool ro = name == "parent" ? parent_ro : name == "cache" ? cache_ro || parent_ro : part_ro || parent_ro;
        *static_cast<int*>(value) = post_ro_mismatch && ro_sets_parent > 0 ? 0 : int(ro);
        return 0;
    }
    check(request == BLKROSET, "ioctl not in allowlist");
    bool ro = *static_cast<int*>(value);
    if (!ro && fails(name == "parent" ? "setrw-parent" : "setrw-part")) { errno = EIO; return -1; }
    if (ro) {
        if (name == "parent") ++ro_sets_parent; else ++ro_sets_part;
        if (fault == "setro-permanent") { errno = EIO; return -1; }
        if (fails("setro-once")) { errno = EIO; return -1; }
    }
    if (name == "parent") parent_ro = ro;
    else if (name == "cache") cache_ro = ro;
    else part_ro = ro;
    update_sysfs();
    return 0;
}

int __wrap_mount(const char* source, const char* target, const char* type, unsigned long flags, const void* data) {
    check(std::string(target) == private_mount, "mount outside private point");
    if (flags == MS_PRIVATE) {
        check(source == nullptr && type == nullptr, "bad mount-private signature");
        if (fails("mount-private")) { errno = EIO; return -1; }
        return 0;
    }
    bool writing = !(flags & MS_RDONLY);
    if (fails(writing ? "mount-rw" : "mount-ro")) { errno = EIO; return -1; }
    check(type && std::string(type) == "ext4" && std::string(static_cast<const char*>(data)) == "noload",
          "mount filesystem/recovery options");
    check((flags & (MS_NOEXEC | MS_NOSUID | MS_NODEV)) == (MS_NOEXEC | MS_NOSUID | MS_NODEV),
          "unsafe mount options");
    int fd = std::stoi(std::string(source).substr(std::string("/proc/self/fd/").size()));
    mount_device = block_name(fd_paths.at(fd));
    check(mount_device == "userdata" || mount_device == "cache", "forbidden mount device");
    check(!mounted, "unexpected remount");
    if (writing) {
        check(!parent_ro && !(mount_device == "cache" ? cache_ro : part_ro), "mount while block RO");
        ++mounts_rw;
        clean_state(mount_device, false);
        rw_saw_dirty = true;
    } else ++mounts_ro;
    mounted = true;
    mount_rw = writing && !ro_mount_fallback;
    update_mountinfo();
    return 0;
}

int __wrap_umount2(const char* path, int flags) {
    check(mounted && std::string(path) == private_mount && flags == 0, "unmount unrelated or lazy mount");
    if (fails("umount")) { errno = EBUSY; return -1; }
    if (mount_rw) clean_state(mount_device, true);
    mounted = false;
    update_mountinfo();
    return 0;
}

int __wrap_mkdirat(int fd, const char* path, mode_t mode) {
    if (fails("mkdir")) { errno = ENOSPC; return -1; }
    return __real_mkdirat(fd, path, mode);
}

int __wrap_fsync(int fd) {
    std::string path = fd_paths.contains(fd) ? fd_paths[fd] : "";
    if (path.find(".tmp.") != std::string::npos && fails("fsync-file")) { errno = EIO; return -1; }
    bool leaf = path.ends_with("/recovery") || path.ends_with("/pico4-recovery");
    if (leaf && fs::exists(config_path(mount_device)) && fails("fsync-dir")) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

int __wrap_renameat(int from_fd, const char* from, int to_fd, const char* to) {
    if (fails("rename")) { errno = EIO; return -1; }
    if (std::string_view(to)=="timeout-settings") {
        check(from_fd==to_fd && fd_paths.at(to_fd)=="/run/overpressure" &&
              std::string_view(from).starts_with("timeout-settings."),"unexpected timeout handoff");
        return __real_renameat(from_fd,from,to_fd,to);
    }
    check(std::string(to) == "preferences.json" && std::string(from).starts_with(".preferences.json.tmp."),
          "rename outside config");
    return __real_renameat(from_fd, from, to_fd, to);
}

long __wrap_syscall(long request, ...) {
    va_list ap;
    va_start(ap, request);
    if (request == SYS_close_range) {
        va_end(ap);
        return 0; // Fake exec writes a fixture response; it never opens hardware.
    }
    check(request == SYS_renameat2, "unexpected syscall");
    int from_fd = va_arg(ap, int);
    const char* from = va_arg(ap, const char*);
    int to_fd = va_arg(ap, int);
    const char* to = va_arg(ap, const char*);
    unsigned flags = va_arg(ap, unsigned);
    va_end(ap);
    check(flags == RENAME_NOREPLACE, "migration must not replace");
    if (raced) put(config_path("userdata"), "{\"version\":1,\"locale\":\"future\",\"language_selected\":true}\n");
    check(fs::exists(backing(virtual_at(from_fd, from))), "migration temporary missing");
    // DrvFS does not implement ext4's renameat2 contract. Model NOREPLACE
    // explicitly; the raced target above must survive untouched.
    if (fs::exists(backing(virtual_at(to_fd, to)))) { errno = EEXIST; return -1; }
    return __real_renameat(from_fd, from, to_fd, to);
}

int __wrap_execve(const char* path, char* const argv[], char* const env[]) {
    bool safe = std::string(path) == "/usr/bin/pico4-install" &&
                std::string(argv[0]) == path && std::string(argv[1]) == "preferences-set" &&
                std::string(argv[2]) == "zh-CN" && argv[3] == nullptr &&
                std::string(env[0]) == "PATH=/usr/sbin:/usr/bin:/sbin:/bin" &&
                std::string(env[1]) == "LC_ALL=C" && std::string(env[2]) == "HOME=/root" && env[3] == nullptr;
    if (!safe) ::_exit(125);
    ::write(STDOUT_FILENO, reply.data(), reply.size());
    ::_exit(reply_status);
}
}

int main() {
    int count = 0;
    auto test = [&](const std::string& name, auto fn) {
        reset(name);
        fn();
        ++count;
        std::cout << "PASS " << name << '\n';
    };
    try {
        test("catalog-and-schema", [] {
            check(p4ui::supported_locales() == std::vector<std::string>{"zh-CN","en"}, "catalog");
            for (const auto& invalid : {"{}", "{\"version\":2,\"locale\":\"zh-CN\",\"language_selected\":true}",
                  "{\"version\":1,\"locale\":\"zh-CN\",\"language_selected\":1}",
                  "{\"version\":1.0,\"locale\":\"zh-CN\",\"language_selected\":true}",
                  "{\"version\":1,\"locale\":\"zh-CN\\u0000x\",\"language_selected\":true}"}) {
                bool rejected = false;
                try { p4ui::parse(invalid); } catch (const p4ui::Failure&) { rejected = true; }
                check(rejected, "accepted bad schema");
            }
            auto object = p4ui::parse(config);
            check(p4ui::decoded(object.get(), "data").durable, "valid schema");
        });
        test("installed-authority-readonly", [] {
            put(config_path("userdata"), config);
            put(config_path("cache"), "{\"version\":1,\"locale\":\"future\",\"language_selected\":true}\n");
            auto p = p4ui::load_preferences();
            check(p.durable && p.language_selected && p.storage == "data" && p.error.empty(), p.error);
            check(mounts_rw == 0 && ro_sets_part == 0 && ro_sets_parent == 0, "load wrote block flags");
        });
        test("unknown-locale-is-not-a-persisted-choice", [] {
            put(config_path("userdata"), "{\"version\":1,\"locale\":\"en-US\",\"language_selected\":true}\n");
            auto p = p4ui::load_preferences();
            check(p.locale == "zh-CN" && !p.durable && !p.language_selected && !p.error.empty(), "unknown locale");
        });
        test("preinstall-known-f2fs-cache", [] {
            preinstall();
            put(config_path("cache"), config);
            auto p = p4ui::load_preferences();
            check(p.durable && p.storage == "cache", p.error);
            check(mounts_rw == 0 && mounts_ro == 1, "readonly cache");
        });
        test("post-format-cache-choice-remains-durable", [] {
            put(config_path("cache"), config);
            auto p = p4ui::load_preferences();
            check(p.durable && p.language_selected && p.storage == "cache" && p.error.empty(), "retained choice");
            check(!fs::exists(config_path("userdata")) && mounts_rw == 0, "load migrated");
        });
        test("native-data-save-fresh-ro-readback", [] {
            put(config_path("userdata"), "{\"version\":1,\"locale\":\"zh-CN\",\"language_selected\":false,\"extra\":7}\n");
            put(fixture + "/volumes/userdata/other-file", "keep");
            p4ui::Preferences p;
            check(p4ui::save_preferences_native("zh-CN", p), p.error);
            check(p.durable && p.language_selected && p.storage == "data", "save result");
            check(mounts_rw == 1 && mounts_ro == 2 && rw_saw_dirty, "transaction/readback sequence");
            check(get(config_path("userdata")).find("\"extra\":7") != std::string::npos, "extension lost");
            check(get(fixture + "/volumes/userdata/other-file") == "keep", "unrelated file lost");
            readonly_after_failure();
            no_temporary("userdata");
        });
        test("first-save-uses-ordinary-settings-permissions", [] {
            const auto root = fixture + "/volumes/userdata";
            fs::remove_all(root + "/var");
            const auto old_mask = ::umask(0022);
            p4ui::Preferences p;
            const bool saved = p4ui::save_preferences_native("zh-CN", p);
            ::umask(old_mask);
            check(saved, p.error);
            auto mode = [](const std::string& path) {
                return fs::status(path).permissions() & fs::perms::all;
            };
            check(mode(root + "/var") == fs::perms(0755) && mode(root + "/var/lib") == fs::perms(0755),
                  "shared SteamOS directories are not searchable");
            check(mode(root + "/var/lib/pico4") == fs::perms(0755) &&
                  mode(root + "/var/lib/pico4/recovery") == fs::perms(0755) &&
                  mode(config_path("userdata")) == fs::perms(0644), "unexpected Recovery settings permissions");
        });
        test("upstream-sticky-memory-root-save-and-read", [] {
            recovery_root_mode = 01777;
            for (const auto type : std::array<long, 2>{TMPFS_MAGIC, RAMFS_MAGIC}) {
                recovery_root_type = type;
                p4ui::Preferences p;
                check(p4ui::save_preferences_native("en", p) && p.durable, p.error);
                auto loaded = p4ui::load_preferences();
                check(loaded.durable && loaded.locale == "en", loaded.error);
                check(recovery_root_mode == 01777, "Recovery root mode was changed");
                readonly_after_failure();
            }
        });
        test("world-writable-root-without-sticky-refused", [] {
            recovery_root_mode = 0777;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("en", p), "non-sticky writable root accepted");
            check(mounts_rw == 0, "unsafe root reached a writable mount");
            readonly_after_failure();
        });
        test("sticky-disk-root-refused", [] {
            recovery_root_mode = 01777;
            recovery_root_type = EXT4_SUPER_MAGIC;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("en", p), "sticky disk root accepted");
            check(mounts_rw == 0, "disk root reached a writable mount");
            readonly_after_failure();
        });
        test("sticky-root-with-another-owner-refused", [] {
            recovery_root_mode = 01777;
            recovery_root_uid = 1000;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("en", p), "non-root-owned sticky root accepted");
            check(mounts_rw == 0, "non-root-owned root reached a writable mount");
            readonly_after_failure();
        });
        test("sticky-root-does-not-exempt-runtime-children", [] {
            recovery_root_mode = 01777;
            for (const auto* path : {"/run", "/run/overpressure"}) {
                writable_runtime_directory = path;
                p4ui::Preferences p;
                check(!p4ui::save_preferences_native("en", p), "writable runtime child accepted");
                check(mounts_rw == 0, "unsafe runtime child reached a writable mount");
                readonly_after_failure();
            }
        });
        test("sticky-root-does-not-follow-runtime-symlink", [] {
            recovery_root_mode = 01777;
            fs::remove_all(fixture + "/run/overpressure");
            fs::create_directory_symlink(fixture + "/volumes/userdata", fixture + "/run/overpressure");
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("en", p), "runtime symlink accepted");
            check(mounts_rw == 0, "runtime symlink reached a writable mount");
            readonly_after_failure();
        });
        test("sticky-root-filesystem-probe-failure-refused", [] {
            recovery_root_mode = 01777;
            fault = "root-fstatfs";
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("en", p), "unproven root filesystem accepted");
            check(mounts_rw == 0, "unproven root filesystem reached a writable mount");
            readonly_after_failure();
        });
        test("native-cache-save", [] {
            preinstall();
            p4ui::Preferences p;
            check(p4ui::save_preferences_native("zh-CN", p), p.error);
            check(p.storage == "cache" && p.durable, "cache save result");
            check(!fs::exists(config_path("userdata")), "wrote userdata before install");
            readonly_after_failure();
        });
        test("android-cache-root-schema-preserved", [] {
            preinstall(); android_cache_root = true;
            p4ui::Preferences p;
            check(p4ui::save_preferences_native("zh-CN", p) && p.durable, p.error);
            auto loaded = p4ui::load_preferences();
            check(loaded.durable && loaded.storage == "cache", loaded.error);
            readonly_after_failure();
            android_cache_root = false;
        });
        test("android-cache-world-writable-root-refused", [] {
            preinstall(); android_cache_root = unsafe_cache_root = true;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p), "unsafe Android cache root accepted");
            readonly_after_failure();
            android_cache_root = unsafe_cache_root = false;
        });
        test("explicit-migration-no-cache-erase", [] {
            put(config_path("cache"), config);
            p4ui::Preferences p;
            check(p4ui::save_preferences_native("zh-CN", p), p.error);
            check(p.durable && p.storage == "data" && get(config_path("cache")) == config, "migration");
            check(get(config_path("userdata")) == p4ui::serialized(p4ui::parse(config).get()), "import changed");
        });
        test("migration-no-replace-race", [] {
            put(config_path("cache"), config);
            raced = true;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable, "overwrote raced config");
            check(get(config_path("userdata")).find("future") != std::string::npos, "race config lost");
            check(get(config_path("cache")) == config, "cache erased");
            readonly_after_failure();
            no_temporary("userdata");
        });
        test("unknown-userdata-never-cache-fallback", [] {
            put(fixture + "/dev/sda1", std::string(1024 * 1024, '\0'));
            put(config_path("cache"), config);
            auto p = p4ui::load_preferences();
            check(!p.durable && !p.error.empty() && mounts_ro == 0, "unknown data fallback");
        });
        test("wrong-link-size-label-and-dirty-cache", [] {
            p4ui::Preferences p;
            wrong_partname = true;
            check(!p4ui::save_preferences_native("zh-CN", p), "PARTNAME gate");
            wrong_partname = false;
            size_mismatch = true;
            check(!p4ui::save_preferences_native("zh-CN", p), "size gate");
            size_mismatch = false;
            preinstall();
            ext_image("cache", "persist");
            check(!p4ui::save_preferences_native("zh-CN", p), "cache label gate");
            ext_image("cache", "cache", false);
            check(!p4ui::save_preferences_native("zh-CN", p), "dirty cache write");
            check(mounts_rw == 0, "gate changed RW state");
        });
        test("data-config-errors-do-not-fallback", [] {
            put(config_path("cache"), config);
            for (const auto& bad : {"not-json", "{\"version\":3,\"locale\":\"zh-CN\",\"language_selected\":true}"}) {
                put(config_path("userdata"), bad);
                auto p = p4ui::load_preferences();
                check(!p.durable && p.storage == "data" && !p.error.empty(), "data error fallback");
                p4ui::Preferences saved;
                check(!p4ui::save_preferences_native("zh-CN", saved) && get(config_path("userdata")) == bad,
                      "destroyed malformed/future config");
                readonly_after_failure();
            }
        });
        test("readonly-existing-mount-reuse", [] {
            inherited_mount = true;
            update_mountinfo();
            put(config_path("userdata"), config);
            auto p = p4ui::load_preferences();
            check(p.durable && mounts_ro == 0, p.error);
            p4ui::Preferences saved;
            check(!p4ui::save_preferences_native("zh-CN", saved) && inherited_mount && mounts_rw == 0,
                  "remounted existing filesystem");
        });
        test("rw-existing-mount-refused-for-load", [] {
            inherited_mount = inherited_rw = true;
            update_mountinfo();
            put(config_path("userdata"), config);
            auto p = p4ui::load_preferences();
            check(!p.durable && !p.error.empty() && mounts_ro == 0, "reused RW mount");
        });
        test("bind-only-not-used-as-filesystem-root", [] {
            inherited_mount = bind_only = true;
            update_mountinfo();
            put(config_path("userdata"), config);
            auto p = p4ui::load_preferences();
            check(p.durable && mounts_ro == 1 && inherited_mount, p.error);
        });
        test("shared-propagation-refused", [] {
            shared_root = true;
            update_mountinfo();
            put(config_path("userdata"), config);
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && mounts_rw == 0, "shared mount");
        });
        test("expanded-settings-round-trip-and-timeout-handoff", [] {
            p4ui::Preferences wanted,p;
            wanted.locale="en";wanted.hour=12;wanted.visible=0x1fc;wanted.date_style=3;
            wanted.zone=5;wanted.units=2;wanted.reverse=true;wanted.connect=600;wanted.idle=0;
            check(p4ui::save_settings_native(wanted,p) && p.durable && p4ui::same_settings(wanted,p),p.error);
            check(p4ui::same_settings(wanted,p4ui::load_preferences()),"settings reload");
            check(get(fixture+"/run/overpressure/timeout-settings")=="600 0\n","committed timeout handoff");
            wanted.visible=512;
            check(!p4ui::save_settings_native(wanted,p),"invalid visibility mask accepted");
            check(get(fixture+"/run/overpressure/timeout-settings")=="600 0\n","invalid request changed timeout");
        });
        test("lock-conflict-chinese-no-writes", [] {
            std::string path = fixture + "/run/overpressure/install.lock";
            int lock = __real_open(path.c_str(), O_RDWR | O_CREAT, 0600);
            check(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "fixture lock");
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable &&
                  p.error.find("install.lock busy") != std::string::npos && mounts_rw == 0,
                  "lock conflict");
            __real_close(lock);
        });
        test("symlink-path-and-config-refused", [] {
            put(config_path("userdata"), config);
            auto path = config_path("userdata");
            fs::remove(path);
            fs::create_symlink(config_path("cache"), path);
            put(config_path("cache"), config);
            auto p = p4ui::load_preferences();
            check(!p.durable && !p.error.empty(), "followed file symlink");
            p4ui::Preferences saved;
            check(!p4ui::save_preferences_native("zh-CN", saved), "overwrote file symlink");
            readonly_after_failure();
        });
        for (const auto& operation : {"setrw-part", "setrw-parent", "mount-rw", "mount-private",
                                      "open-temp", "fsync-file", "close-temp", "rename", "fsync-dir"}) {
            test(std::string("fault-") + operation, [&] {
                put(config_path("userdata"), config);
                auto original = get(config_path("userdata"));
                fault = operation;
                if (fault == "mount-private") fault_at = 2; // Native save first reads data RO.
                p4ui::Preferences p;
                check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable && !p.error.empty(), "false success");
                readonly_after_failure();
                no_temporary("userdata");
                if (fault != "fsync-dir") check(get(config_path("userdata")) == original, "old config lost before rename");
            });
        }
        test("unmount-failure-still-restores-both-flags", [] {
            put(config_path("userdata"), config);
            fault = "umount";
            fault_at = 2;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable, "unmount false success");
            readonly_after_failure();
            check(ro_sets_part > 0 && ro_sets_parent > 0, "cleanup skipped RO");
        });
        test("transient-ro-failure-retried", [] {
            put(config_path("userdata"), config);
            fault = "setro-once";
            p4ui::Preferences p;
            check(p4ui::save_preferences_native("zh-CN", p), p.error);
            readonly_after_failure();
            check(ro_sets_part >= 2 && ro_sets_parent >= 2, "RO not retried");
        });
        test("permanent-ro-failure-reported", [] {
            put(config_path("userdata"), config);
            fault = "setro-permanent";
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable, "RO failure accepted");
            check(ro_sets_part == 3 && ro_sets_parent == 3, "both RO attempts required");
        });
        test("post-ro-readback-failure", [] {
            put(config_path("userdata"), config);
            fault = "mount-ro";
            fault_at = 2;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable, "failed fresh readback accepted");
            readonly_after_failure();
        });
        test("mount-rw-silent-ro-fallback-refused", [] {
            put(config_path("userdata"), config);
            ro_mount_fallback = true;
            p4ui::Preferences p;
            check(!p4ui::save_preferences_native("zh-CN", p) && !p.durable, "RO fallback accepted");
            readonly_after_failure();
        });
        test("wrong-mounted-filesystem-refused", [] {
            put(config_path("userdata"), config);
            wrong_fs = true;
            auto p = p4ui::load_preferences();
            check(!p.durable && !p.error.empty(), "wrong mounted FS");
            readonly_after_failure();
        });
        test("ui-save-only-installer-subcommand", [] {
            p4ui::Preferences p;
            check(p4ui::save_preferences("zh-CN", p) && p.durable && p.storage == "data", p.error);
            check(mounts_rw == 0 && ro_sets_part == 0, "UI performed transaction directly");
        });
        test("unsupported-ui-native-input-never-executed", [] {
            p4ui::Preferences p;
            check(!p4ui::save_preferences("zh-CN;reboot", p) && !p.durable, "shell-like locale accepted");
            check(!p4ui::save_preferences_native("en-US", p) && mounts_rw == 0, "unknown native locale accepted");
        });
        test("installer-error-and-invalid-success", [] {
            p4ui::Preferences p;
            reply = "{\"version\":1,\"locale\":\"zh-CN\",\"language_selected\":false,\"durable\":false,"
                    "\"storage\":\"none\",\"error\":\"\u5b89\u88c5\u4efb\u52a1\u6b63\u5728\u8fd0\u884c\"}\n";
            reply_status = 1;
            check(!p4ui::save_preferences("zh-CN", p) && !p.durable && !p.error.empty(), "child failure accepted");
            reply_status = 0;
            check(!p4ui::save_preferences("zh-CN", p), "child false success accepted");
            reply = config;
            check(!p4ui::save_preferences("zh-CN", p), "missing response fields accepted");
            reply = "not-json\n";
            check(!p4ui::save_preferences("zh-CN", p), "invalid JSON accepted");
        });
        std::cout << count << " tests passed; synthetic files only\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\nfixture: " << fixture << '\n';
        return 1;
    }
}
