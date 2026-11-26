// SPDX-License-Identifier: Unlicense
#include "network.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <sstream>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

namespace p4ui {
namespace {
constexpr auto net_env = "/run/overpressure/net.env";
constexpr auto gadget_udc = "/sys/kernel/config/usb_gadget/overpressure/UDC";

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool component(std::string_view name, std::size_t limit) {
    return !name.empty() && name.size() <= limit && name != "." && name != ".." &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
            std::string_view::npos;
}

std::optional<std::string> read_file(const std::string& path, std::size_t limit) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return {};
    std::string content(limit + 1, '\0');
    std::size_t used = 0;
    bool okay = true;
    while (used < content.size()) {
        const auto count = read(fd, content.data() + used, content.size() - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { okay = false; break; }
        if (count == 0) break;
        used += static_cast<std::size_t>(count);
    }
    close(fd);
    if (!okay || used > limit) return {};
    content.resize(used);
    return content;
}

std::optional<std::string> bounded_read(const NetworkReader& reader, const std::string& path,
                                        std::size_t limit) {
    auto content = reader(path, limit);
    if (!content || content->size() > limit || content->find('\0') != std::string::npos) return {};
    return content;
}

std::optional<std::string> interface_selection(const std::optional<std::string>& env) {
    if (!env) return {};
    std::istringstream lines(*env);
    std::string line;
    std::optional<std::string> selected;
    while (std::getline(lines, line)) {
        const auto row = trim(line);
        if (!row.starts_with("IFACE=")) continue;
        if (selected) return {};
        const auto name = row.substr(6);
        if (!name.empty() && !valid_ncm_interface(name)) return {};
        selected = std::string(name);
    }
    return selected;
}

template<class T> std::optional<T> number(const std::optional<std::string>& content, int base = 10) {
    if (!content) return {};
    auto value = trim(*content);
    if (base == 16 && (value.starts_with("0x") || value.starts_with("0X"))) value.remove_prefix(2);
    if (value.empty()) return {};
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
    return result;
}

std::optional<bool> ipv4_address(const std::string& name) {
    struct ifaddrs* entries = nullptr;
    if (getifaddrs(&entries) != 0) return {};
    bool found = false;
    for (auto* entry = entries; entry; entry = entry->ifa_next) {
        if (!entry->ifa_name || name != entry->ifa_name || !entry->ifa_addr ||
            entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* address = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
        if (valid_ncm_ipv4(ntohl(address->sin_addr.s_addr))) { found = true; break; }
    }
    freeifaddrs(entries);
    return found;
}
}

bool valid_ncm_interface(std::string_view name) noexcept {
    return component(name, IFNAMSIZ - 1);
}

bool valid_ncm_ipv4(std::uint32_t address_host_order) noexcept {
    const auto first = address_host_order >> 24;
    return first != 0 && first != 127 && first < 224;
}

NcmState classify_ncm(const NetworkObservation& observation) noexcept {
    if (!observation.interface_name) return NcmState::unknown;
    if (observation.interface_name->empty()) return NcmState::disabled;
    if (!valid_ncm_interface(*observation.interface_name)) return NcmState::unknown;
    if (observation.udc && observation.udc->empty()) return NcmState::disabled;
    if (observation.flags && (*observation.flags & IFF_LOOPBACK)) return NcmState::unknown;
    if (observation.flags && !(*observation.flags & IFF_UP)) return NcmState::down;
    if (!observation.udc || !component(*observation.udc, 128) || !observation.udc_state)
        return NcmState::unknown;
    const auto& state = *observation.udc_state;
    if (state == "suspended") return NcmState::suspended;
    if (state == "not attached") return NcmState::disconnected;
    if (state == "attached" || state == "powered" || state == "default" || state == "addressed" ||
        state == "reconnecting" || state == "unauthenticated") return NcmState::connecting;
    if (state != "configured" || !observation.ifindex || !*observation.ifindex || !observation.flags)
        return NcmState::unknown;
    if (observation.carrier == false) return NcmState::disconnected;
    if (observation.operstate == "down" || observation.operstate == "lowerlayerdown" ||
        observation.operstate == "notpresent") return NcmState::disconnected;
    if (observation.operstate == "dormant" || observation.operstate == "testing")
        return NcmState::connecting;
    if (!observation.carrier || !observation.operstate || !observation.has_ipv4 ||
        (*observation.operstate != "up" && *observation.operstate != "unknown")) return NcmState::unknown;
    // This is local IPv4 readiness, not a claim about the host or Internet reachability.
    return *observation.has_ipv4 ? NcmState::connected : NcmState::no_address;
}

NetworkObservation collect_network(const NetworkReader& reader, const NetworkAddressProbe& addresses) {
    NetworkObservation observation;
    observation.interface_name = interface_selection(bounded_read(reader, net_env, 4096));
    if (const auto content = bounded_read(reader, gadget_udc, 256)) {
        const auto name = trim(*content);
        if (name.empty() || component(name, 128)) observation.udc = std::string(name);
    }
    if (observation.udc && !observation.udc->empty()) {
        if (const auto content = bounded_read(reader, "/sys/class/udc/" + *observation.udc + "/state", 128)) {
            const auto state = trim(*content);
            if (!state.empty()) observation.udc_state = std::string(state);
        }
    }
    if (!observation.interface_name || observation.interface_name->empty()) return observation;
    const auto& name = *observation.interface_name;
    const auto root = "/sys/class/net/" + name + "/";
    observation.ifindex = number<std::uint32_t>(bounded_read(reader, root + "ifindex", 64));
    if (observation.ifindex == 0) observation.ifindex.reset();
    observation.flags = number<std::uint32_t>(bounded_read(reader, root + "flags", 64), 16);
    const auto carrier = number<unsigned>(bounded_read(reader, root + "carrier", 64));
    if (carrier && *carrier <= 1) observation.carrier = *carrier == 1;
    if (const auto content = bounded_read(reader, root + "operstate", 64)) {
        const auto state = trim(*content);
        if (!state.empty()) observation.operstate = std::string(state);
    }
    observation.has_ipv4 = addresses ? addresses(name) : ipv4_address(name);
    observation.rx_bytes = number<std::uint64_t>(bounded_read(reader, root + "statistics/rx_bytes", 64));
    observation.tx_bytes = number<std::uint64_t>(bounded_read(reader, root + "statistics/tx_bytes", 64));
    // Re-plug can replace the netdev between reads; never pair its old/new counters.
    const auto final_index = number<std::uint32_t>(bounded_read(reader, root + "ifindex", 64));
    if (!observation.ifindex || final_index != observation.ifindex) {
        observation.ifindex.reset();
        observation.flags.reset();
        observation.carrier.reset();
        observation.operstate.reset();
        observation.has_ipv4.reset();
        observation.rx_bytes.reset();
        observation.tx_bytes.reset();
    }
    return observation;
}

NetworkObservation collect_network() {
    return collect_network(read_file);
}

NetworkRates NetworkRateHistory::sample(const NetworkObservation& observation, double monotonic) {
    if (!std::isfinite(monotonic) || monotonic < 0 || !observation.interface_name ||
        !valid_ncm_interface(*observation.interface_name) || !observation.ifindex || !*observation.ifindex ||
        !observation.rx_bytes || !observation.tx_bytes) {
        baseline_.reset();
        last_seen_.reset();
        rates_ = {};
        return rates_;
    }
    NetworkCounterSample current{*observation.interface_name, *observation.ifindex,
                                 *observation.rx_bytes, *observation.tx_bytes, monotonic};
    const bool discontinuity = !baseline_ || !last_seen_ || monotonic < *last_seen_ ||
        current.interface_name != baseline_->interface_name || current.ifindex != baseline_->ifindex ||
        current.rx_bytes < baseline_->rx_bytes || current.tx_bytes < baseline_->tx_bytes;
    last_seen_ = monotonic;
    if (discontinuity) {
        baseline_ = std::move(current);
        rates_ = {};
        return rates_;
    }
    const double elapsed = monotonic - baseline_->monotonic;
    if (elapsed < 1) return rates_;
    // Subtract integers before converting, preserving small deltas above 2^53.
    rates_.upload_bps = static_cast<double>(current.tx_bytes - baseline_->tx_bytes) / elapsed;
    rates_.download_bps = static_cast<double>(current.rx_bytes - baseline_->rx_bytes) / elapsed;
    baseline_ = std::move(current);
    return rates_;
}
}
