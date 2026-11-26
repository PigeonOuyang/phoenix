// SPDX-License-Identifier: Unlicense
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace p4ui {
enum class NcmState { disabled, down, disconnected, connecting, no_address, connected, suspended, unknown };

constexpr std::string_view ncm_state_string(NcmState state) noexcept {
    switch (state) {
    case NcmState::disabled: return "disabled";
    case NcmState::down: return "down";
    case NcmState::disconnected: return "disconnected";
    case NcmState::connecting: return "connecting";
    case NcmState::no_address: return "no-address";
    case NcmState::connected: return "connected";
    case NcmState::suspended: return "suspended";
    case NcmState::unknown: return "unknown";
    }
    return "unknown";
}

struct NetworkObservation {
    // Empty selections mean disabled; missing or malformed selections mean unknown.
    std::optional<std::string> interface_name, udc, udc_state, operstate;
    std::optional<std::uint32_t> ifindex, flags;
    std::optional<bool> carrier, has_ipv4;
    std::optional<std::uint64_t> rx_bytes, tx_bytes;
};

bool valid_ncm_interface(std::string_view name) noexcept;
bool valid_ncm_ipv4(std::uint32_t address_host_order) noexcept;
NcmState classify_ncm(const NetworkObservation& observation) noexcept;

using NetworkReader = std::function<std::optional<std::string>(const std::string&, std::size_t)>;
using NetworkAddressProbe = std::function<std::optional<bool>(const std::string&)>;
NetworkObservation collect_network();
// An empty address probe uses live getifaddrs; injected readers never source net.env.
NetworkObservation collect_network(const NetworkReader& reader, const NetworkAddressProbe& addresses = {});

struct NetworkRates {
    // Bytes per second, relative to the headset: TX is upload, RX is download.
    std::optional<double> upload_bps, download_bps;
};

struct NetworkCounterSample {
    std::string interface_name;
    std::uint32_t ifindex = 0;
    std::uint64_t rx_bytes = 0, tx_bytes = 0;
    double monotonic = 0;
};

class NetworkRateHistory {
public:
    NetworkRates sample(const NetworkObservation& observation, double monotonic);
    const std::optional<NetworkCounterSample>& last_sample() const noexcept { return baseline_; }

private:
    std::optional<NetworkCounterSample> baseline_;
    std::optional<double> last_seen_;
    NetworkRates rates_;
};
}
