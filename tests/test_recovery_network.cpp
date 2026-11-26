// SPDX-License-Identifier: Unlicense
#include "../src/recovery-ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <net/if.h>

using namespace p4ui;
namespace {
int checks = 0;

void check(bool result, const std::string& message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}

void state(const NetworkObservation& observation, NcmState expected, const std::string& message) {
    check(classify_ncm(observation) == expected, message);
}

void unknown_rates(const NetworkRates& rates, const std::string& message) {
    check(!rates.upload_bps && !rates.download_bps, message);
}

void rates(const NetworkRates& value, double upload, double download, const std::string& message) {
    check(value.upload_bps && value.download_bps &&
        std::isfinite(*value.upload_bps) && std::isfinite(*value.download_bps) &&
        *value.upload_bps >= 0 && *value.download_bps >= 0 &&
        std::abs(*value.upload_bps - upload) < 1e-9 &&
        std::abs(*value.download_bps - download) < 1e-9, message);
}

NetworkObservation ready() {
    NetworkObservation observation;
    observation.interface_name = "usb0";
    observation.udc = "a600000.dwc3";
    observation.udc_state = "configured";
    observation.ifindex = 7;
    observation.flags = IFF_UP;
    observation.carrier = true;
    observation.operstate = "up";
    observation.has_ipv4 = true;
    observation.rx_bytes = 1000;
    observation.tx_bytes = 2000;
    return observation;
}

void classifier_tests() {
    state({}, NcmState::unknown, "missing observations are not disconnected or disabled");
    state(ready(), NcmState::connected, "configured carrier with a live IPv4 is connected locally");
    auto observation = ready();
    observation.udc = "a600000.usb";
    observation.ifindex = 3;
    observation.flags = 0x1003;
    state(observation, NcmState::connected, "actual PICO snapshot: usb0, a600000.usb, ifindex 3");
    observation = ready();
    observation.interface_name = "";
    state(observation, NcmState::disabled, "explicitly empty IFACE is disabled");
    observation = ready();
    observation.udc = "";
    state(observation, NcmState::disabled, "unbound gadget is disabled despite stale IPv4");
    observation = ready();
    observation.flags = 0;
    state(observation, NcmState::down, "administratively down interface");
    observation.udc_state = "suspended";
    state(observation, NcmState::down, "admin-down takes precedence over suspension");
    observation.flags = IFF_UP;
    observation.carrier = false;
    observation.operstate = "down";
    state(observation, NcmState::suspended, "suspension takes precedence over carrier drop");
    observation.udc_state = "not attached";
    state(observation, NcmState::disconnected, "not-attached UDC overrides stale address");
    for (const auto* usb : {"attached", "powered", "default", "addressed", "reconnecting", "unauthenticated"}) {
        observation.udc_state = usb;
        state(observation, NcmState::connecting, std::string("USB enumeration: ") + usb);
    }
    observation = ready();
    observation.udc_state = "attached";
    observation.ifindex.reset();
    observation.flags.reset();
    observation.carrier.reset();
    observation.operstate.reset();
    observation.has_ipv4.reset();
    state(observation, NcmState::connecting, "enumeration does not require an already-created netdev");
    observation = ready();
    observation.carrier = false;
    state(observation, NcmState::disconnected, "carrier-down overrides stale IPv4");
    for (const auto* oper : {"down", "lowerlayerdown", "notpresent"}) {
        observation = ready();
        observation.operstate = oper;
        state(observation, NcmState::disconnected, std::string("unusable operstate: ") + oper);
    }
    for (const auto* oper : {"dormant", "testing"}) {
        observation = ready();
        observation.operstate = oper;
        state(observation, NcmState::connecting, std::string("transitional operstate: ") + oper);
    }
    observation = ready();
    observation.has_ipv4 = false;
    state(observation, NcmState::no_address, "link readiness is not address readiness");
    observation.operstate = "unknown";
    state(observation, NcmState::no_address, "unknown operstate can rely on actual carrier and UDC");
    observation.has_ipv4 = true;
    state(observation, NcmState::connected, "carrier supports drivers without operstate reporting");
    for (int missing = 0; missing < 8; ++missing) {
        observation = ready();
        switch (missing) {
        case 0: observation.interface_name.reset(); break;
        case 1: observation.udc.reset(); break;
        case 2: observation.udc_state.reset(); break;
        case 3: observation.ifindex.reset(); break;
        case 4: observation.flags.reset(); break;
        case 5: observation.carrier.reset(); break;
        case 6: observation.operstate.reset(); break;
        case 7: observation.has_ipv4.reset(); break;
        }
        state(observation, NcmState::unknown, "missing required evidence " + std::to_string(missing));
    }
    observation = ready();
    observation.rx_bytes.reset();
    observation.tx_bytes.reset();
    state(observation, NcmState::connected, "counter availability is independent of link classification");
    observation = ready();
    observation.ifindex = 0;
    state(observation, NcmState::unknown, "zero ifindex is not a netdev identity");
    observation = ready();
    observation.flags = IFF_UP | IFF_LOOPBACK;
    state(observation, NcmState::unknown, "loopback cannot masquerade as NCM");
    observation = ready();
    observation.interface_name = "../usb0";
    state(observation, NcmState::unknown, "pure classifier rejects unvalidated interface paths");
    observation = ready();
    observation.udc = "../dwc3";
    state(observation, NcmState::unknown, "pure classifier rejects unvalidated controller paths");
    observation = ready();
    observation.udc_state = "unrecognized";
    state(observation, NcmState::unknown, "unknown UDC state never infers readiness from carrier");
    observation = ready();
    observation.operstate = "unrecognized";
    state(observation, NcmState::unknown, "unrecognized operstate never infers readiness from IPv4");
    const std::vector<std::pair<NcmState, std::string_view>> labels{
        {NcmState::disabled, "disabled"}, {NcmState::down, "down"},
        {NcmState::disconnected, "disconnected"}, {NcmState::connecting, "connecting"},
        {NcmState::no_address, "no-address"}, {NcmState::connected, "connected"},
        {NcmState::suspended, "suspended"}, {NcmState::unknown, "unknown"}};
    for (const auto& [value, label] : labels) check(ncm_state_string(value) == label, "stable state label");
    check(ncm_state_string(static_cast<NcmState>(999)) == "unknown", "invalid enum label is unknown");
    std::cout << "classifier: all eight states, precedence, and missing evidence passed\n";
}

void validation_tests() {
    for (const auto* name : {"usb0", "ncm0", "eth-0.1", "ABCDEFGHIJKLMNO", "_net"})
        check(valid_ncm_interface(name), std::string("valid interface name: ") + name);
    for (const auto* name : {"", ".", "..", "../usb0", "/usb0", "usb0/flags", "usb 0", "usb0:1",
                             "usb0;id", "usb0$(id)", "ABCDEFGHIJKLMNOP", "usb\t0", "usb\n0"})
        check(!valid_ncm_interface(name), std::string("unsafe or oversized interface rejected: ") + name);
    check(!valid_ncm_interface(std::string_view("usb\0x", 5)), "embedded NUL rejected");
    for (const std::uint32_t address : {0x01000001U, 0x0a000001U, 0xa9fe0001U, 0xac102a01U, 0xc0a80701U,
                                       0xdfFFFFFFU})
        check(valid_ncm_ipv4(address), "local unicast IPv4, including link-local, needs no Internet probe");
    for (const std::uint32_t address : {0U, 0x00000001U, 0x00FFFFFFU, 0x7f000001U, 0x7fFFFFFFU,
                                       0xe0000000U, 0xefffffffU, 0xf0000000U, 0xffffffffU})
        check(!valid_ncm_ipv4(address), "unspecified, loopback, multicast, reserved, or broadcast excluded");
    std::cout << "validation: interface bounds and local IPv4 boundaries passed\n";
}

struct Fixture {
    std::map<std::string, std::string> files{
        {"/run/overpressure/net.env", "IFACE=usb0\nIP=192.0.2.1\nUDC=wrong.env.controller\n"},
        {"/sys/kernel/config/usb_gadget/overpressure/UDC", "a600000.dwc3\n"},
        {"/sys/class/udc/a600000.dwc3/state", "configured\n"},
        {"/sys/class/udc/wrong.env.controller/state", "not attached\n"},
        {"/sys/class/net/usb0/ifindex", "7\n"},
        {"/sys/class/net/usb0/flags", "0x1003\n"},
        {"/sys/class/net/usb0/carrier", "1\n"},
        {"/sys/class/net/usb0/operstate", "up\n"},
        {"/sys/class/net/usb0/statistics/rx_bytes", "1000\n"},
        {"/sys/class/net/usb0/statistics/tx_bytes", "2000\n"}};
    std::vector<std::string> reads;
    std::optional<bool> address = true;
    int address_calls = 0, index_reads = 0;
    bool replace_interface = false;

    std::optional<std::string> read(const std::string& path, std::size_t) {
        reads.push_back(path);
        if (path == "/sys/class/net/usb0/ifindex" && ++index_reads > 1 && replace_interface) return "8\n";
        const auto entry = files.find(path);
        return entry == files.end() ? std::optional<std::string>{} : entry->second;
    }
    NetworkObservation collect() {
        return collect_network([&](const auto& path, auto limit) { return read(path, limit); },
            [&](const auto& name) {
                check(name == "usb0", "address query uses exactly the selected, validated IFACE");
                ++address_calls;
                return address;
            });
    }
};

void collector_tests() {
    Fixture fixture;
    auto observation = fixture.collect();
    state(observation, NcmState::connected, "collector observes all live readiness inputs");
    check(observation.udc == "a600000.dwc3", "controller selection comes only from configfs UDC");
    check(observation.ifindex == 7 && observation.flags == 0x1003 && observation.carrier == true,
          "sysfs identity, hexadecimal flags, and carrier parsed");
    check(observation.rx_bytes == 1000 && observation.tx_bytes == 2000, "RX/TX byte counters preserved");
    check(std::find(fixture.reads.begin(), fixture.reads.end(),
                    "/sys/class/udc/wrong.env.controller/state") == fixture.reads.end(),
          "never read a stale controller from net.env or enumerate other UDCs");
    fixture.address = false;
    state(fixture.collect(), NcmState::no_address, "configured IP= is not a live address");
    fixture.address.reset();
    state(fixture.collect(), NcmState::unknown, "getifaddrs failure is not address absence");
    for (const auto* env : {"IFACE=\n", "  IFACE=\r\n"}) {
        Fixture blank;
        blank.files["/run/overpressure/net.env"] = env;
        state(blank.collect(), NcmState::disabled, "explicit empty selection");
        check(blank.address_calls == 0, "disabled selection does not query addresses");
    }
    for (const auto& env : std::vector<std::string>{"", "IP=192.0.2.1\n", "IFACE=../usb0\n",
             "IFACE=usb0/flags\n", "IFACE=usb0;id\n", "IFACE='usb0'\n", "IFACE=usb0\nIFACE=usb0\n",
             "IFACE=ABCDEFGHIJKLMNOP\n", std::string("IFACE=usb0\0\n", 12), std::string(4097, 'x')}) {
        Fixture invalid;
        invalid.files["/run/overpressure/net.env"] = env;
        state(invalid.collect(), NcmState::unknown, "malformed/missing/duplicate IFACE rejected");
        check(invalid.address_calls == 0, "unvalidated IFACE never reaches address lookup or sysfs");
        check(std::none_of(invalid.reads.begin(), invalid.reads.end(), [](const auto& path) {
            return path.starts_with("/sys/class/net/");
        }), "invalid IFACE never forms a netdev path");
    }
    fixture = Fixture{};
    fixture.files.erase("/run/overpressure/net.env");
    state(fixture.collect(), NcmState::unknown, "missing net.env is unknown, not disabled");
    fixture = Fixture{};
    fixture.files["/sys/kernel/config/usb_gadget/overpressure/UDC"] = "\n";
    state(fixture.collect(), NcmState::disabled, "empty actual binding overrides stale env and interface");
    for (const auto& binding : {std::string("../controller\n"), std::string("controller/name\n"),
                                std::string(129, 'a'), std::string(257, 'a')}) {
        Fixture invalid;
        invalid.files["/sys/kernel/config/usb_gadget/overpressure/UDC"] = binding;
        state(invalid.collect(), NcmState::unknown, "malformed or oversized UDC binding rejected");
        check(std::none_of(invalid.reads.begin(), invalid.reads.end(), [](const auto& path) {
            return path.starts_with("/sys/class/udc/");
        }), "invalid binding never forms a controller path");
    }
    fixture = Fixture{};
    fixture.files.erase("/sys/kernel/config/usb_gadget/overpressure/UDC");
    state(fixture.collect(), NcmState::unknown, "unreadable binding is unknown, not unbound");
    fixture = Fixture{};
    fixture.files.erase("/sys/class/udc/a600000.dwc3/state");
    state(fixture.collect(), NcmState::unknown, "unreadable selected controller is unknown");
    for (const auto* raw : {"", "2\n", "-1\n", "1oops\n"}) {
        Fixture invalid;
        invalid.files["/sys/class/net/usb0/carrier"] = raw;
        observation = invalid.collect();
        check(!observation.carrier, "invalid carrier is not false or true");
        state(observation, NcmState::unknown, "invalid carrier cannot prove link readiness");
    }
    for (const auto* raw : {"0\n", "-7\n", "7oops\n", "4294967296\n", ""}) {
        Fixture invalid;
        invalid.files["/sys/class/net/usb0/ifindex"] = raw;
        observation = invalid.collect();
        check(!observation.ifindex && !observation.rx_bytes && !observation.tx_bytes,
              "invalid ifindex discards counter and identity evidence");
        state(observation, NcmState::unknown, "invalid identity cannot prove readiness");
    }
    for (const auto* raw : {"", "0x\n", "-1\n", "0x3garbage\n", "0x100000000\n"}) {
        Fixture invalid;
        invalid.files["/sys/class/net/usb0/flags"] = raw;
        observation = invalid.collect();
        check(!observation.flags, "strict unsigned hexadecimal flags");
        state(observation, NcmState::unknown, "missing flags never mean administratively down");
    }
    for (const auto* raw : {"", "-1\n", "10oops\n", "18446744073709551616\n", "0x12\n"}) {
        Fixture invalid;
        invalid.files["/sys/class/net/usb0/statistics/rx_bytes"] = raw;
        observation = invalid.collect();
        check(!observation.rx_bytes, "strict full-width unsigned decimal counter");
        state(observation, NcmState::connected, "bad counters do not alter address/link classification");
    }
    fixture = Fixture{};
    fixture.files["/sys/class/net/usb0/statistics/rx_bytes"] = "18446744073709551615\n";
    fixture.files["/sys/class/net/usb0/statistics/tx_bytes"] = "0\n";
    observation = fixture.collect();
    check(observation.rx_bytes == std::numeric_limits<std::uint64_t>::max() && observation.tx_bytes == 0,
          "zero and UINT64_MAX counters are representable");
    fixture = Fixture{};
    fixture.replace_interface = true;
    observation = fixture.collect();
    check(!observation.ifindex && !observation.flags && !observation.carrier && !observation.operstate &&
          !observation.has_ipv4 && !observation.rx_bytes && !observation.tx_bytes,
          "replacement during a single collection discards inconsistent netdev evidence");
    state(observation, NcmState::unknown, "mid-collection recreation is unknown");
    fixture = Fixture{};
    fixture.files["/run/overpressure/net.env"] = "IFACE=lo\n";
    fixture.files["/sys/class/net/lo/ifindex"] = "1\n";
    fixture.files["/sys/class/net/lo/flags"] = "0x49\n";
    observation = collect_network([&](const auto& path, auto limit) { return fixture.read(path, limit); });
    check(observation.has_ipv4 == false, "live getifaddrs ignores loopback IPv4");
    state(observation, NcmState::unknown, "loopback cannot be reported as an NCM connection");
    std::cout << "collector: selected UDC, strict input, live address, and recreation checks passed\n";
}

void rate_tests() {
    NetworkRateHistory history;
    auto observation = ready();
    unknown_rates(history.sample(observation, 0), "first counters are a baseline, not lifetime traffic");
    check(history.last_sample() && history.last_sample()->monotonic == 0, "zero monotonic origin is valid");
    observation.rx_bytes = 1100;
    observation.tx_bytes = 2400;
    unknown_rates(history.sample(observation, 0.999), "subsecond action refresh produces no first rate");
    check(history.last_sample()->rx_bytes == 1000 && history.last_sample()->monotonic == 0,
          "subsecond refresh does not advance counter baseline");
    rates(history.sample(observation, 1), 400, 100, "one-second threshold, TX upload and RX download");
    observation.rx_bytes = 9000;
    observation.tx_bytes = 10000;
    rates(history.sample(observation, 1.001), 400, 100, "duplicate/action refresh retains rates without spikes");
    rates(history.sample(observation, 1.001), 400, 100, "duplicate timestamp is not a zero-duration division");
    check(history.last_sample()->monotonic == 1 && history.last_sample()->rx_bytes == 1100,
          "extra actions leave diagnostics paired with accepted rate sample");
    observation.rx_bytes = 1350;
    observation.tx_bytes = 3150;
    rates(history.sample(observation, 3.5), 300, 100, "delayed sampling uses actual elapsed 2.5 seconds");
    rates(history.sample(observation, 4.5), 0, 0, "idle counters are known zero rates");
    observation.tx_bytes = 10;
    unknown_rates(history.sample(observation, 4.6), "TX rollback clears both rates even on an extra refresh");
    check(history.last_sample()->tx_bytes == 10 && history.last_sample()->monotonic == 4.6,
          "rollback immediately establishes a new baseline");
    observation.rx_bytes = 1450;
    observation.tx_bytes = 110;
    rates(history.sample(observation, 5.6), 100, 100, "new sample after rollback has no unsigned underflow");
    observation.rx_bytes = 1;
    unknown_rates(history.sample(observation, 6.6), "RX rollback clears both directions");
    observation.rx_bytes = 51;
    observation.tx_bytes = 210;
    rates(history.sample(observation, 7.6), 100, 50, "RX rollback rebaseline recovers");

    for (int missing = 0; missing < 6; ++missing) {
        NetworkRateHistory gap;
        const auto valid = ready();
        gap.sample(valid, 10);
        rates(gap.sample(valid, 11), 0, 0, "pre-gap known rates");
        auto invalid = valid;
        switch (missing) {
        case 0: invalid.interface_name.reset(); break;
        case 1: invalid.interface_name = ""; break;
        case 2: invalid.ifindex.reset(); break;
        case 3: invalid.ifindex = 0; break;
        case 4: invalid.rx_bytes.reset(); break;
        case 5: invalid.tx_bytes.reset(); break;
        }
        unknown_rates(gap.sample(invalid, 11.1), "missing identity/counter clears stale rates immediately");
        check(!gap.last_sample(), "missing sample removes diagnostics baseline");
        unknown_rates(gap.sample(valid, 12), "first sample after a gap does not bridge unknown traffic");
        rates(gap.sample(valid, 13), 0, 0, "gap recovery requires two complete samples");
    }
    observation = ready();
    history.sample(observation, 20);
    history.sample(observation, 21);
    observation.interface_name = "usb1";
    unknown_rates(history.sample(observation, 21.1), "interface name change rebaselines before cadence gate");
    observation.rx_bytes = 1200;
    observation.tx_bytes = 2300;
    rates(history.sample(observation, 22.1), 300, 200, "renamed interface recovers after a full interval");
    observation.ifindex = 8;
    observation.rx_bytes = 50000;
    observation.tx_bytes = 100000;
    unknown_rates(history.sample(observation, 22.2), "same name with a new ifindex never spikes upward");
    observation.rx_bytes = 50200;
    observation.tx_bytes = 100400;
    rates(history.sample(observation, 23.2), 400, 200, "recreated identity measures only new counters");
    unknown_rates(history.sample(observation, 23.1), "backward clock cannot retain stale rates");
    rates(history.sample(observation, 24.1), 0, 0, "clock rollback rebaseline recovers");
    history.sample(observation, 24.8);
    unknown_rates(history.sample(observation, 24.7), "clock rollback detected relative to extra refresh too");
    for (const double invalid : {-1., std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        unknown_rates(history.sample(observation, invalid), "invalid monotonic time clears history");
        check(!history.last_sample(), "invalid timestamp cannot appear in sample diagnostics");
        unknown_rates(history.sample(observation, 30), "valid clock after invalid time starts unknown");
        rates(history.sample(observation, 31), 0, 0, "valid time recovers normally");
    }
    NetworkRateHistory wide;
    observation = ready();
    observation.rx_bytes = std::numeric_limits<std::uint64_t>::max() - 10;
    observation.tx_bytes = (std::uint64_t{1} << 53) + 1;
    wide.sample(observation, 100);
    observation.rx_bytes = std::numeric_limits<std::uint64_t>::max();
    *observation.tx_bytes += 3;
    rates(wide.sample(observation, 101.5), 2, 10 / 1.5, "integer subtraction preserves small full-width deltas");
    observation.rx_bytes = 0;
    unknown_rates(wide.sample(observation, 102.5), "UINT64_MAX wrap/reset is not interpreted as throughput");
    *observation.rx_bytes = 100;
    *observation.tx_bytes += 200;
    rates(wide.sample(observation, 103.5), 200, 100, "full-width wrap recovery has no spike");
    NetworkRateHistory long_gap;
    observation = ready();
    long_gap.sample(observation, 1);
    *observation.rx_bytes += 3600;
    *observation.tx_bytes += 7200;
    rates(long_gap.sample(observation, 3601), 2, 1, "long positive elapsed interval is not treated as one second");
    std::cout << "rate estimator: cadence, elapsed time, missing/reset/identity/clock, and uint64 bounds passed\n";
}

void backend_history_tests() {
    static_assert(std::is_same_v<decltype(Telemetry::ncm_state), std::string>);
    static_assert(std::is_same_v<decltype(Telemetry::ncm_interface), std::string>);
    static_assert(std::is_same_v<decltype(Telemetry::upload_bps), std::optional<double>>);
    static_assert(std::is_same_v<decltype(Telemetry::download_bps), std::optional<double>>);
    Backend first, second;
    check(first.telemetry.ncm_state == "unknown" && first.telemetry.ncm_interface.empty() &&
          !first.telemetry.upload_bps && !first.telemetry.download_bps &&
          !first.telemetry.network_rx_bytes && !first.telemetry.network_tx_bytes &&
          !first.telemetry.network_sample_seconds, "telemetry defaults explicitly unknown");
    auto observation = ready();
    first.network_rates.sample(observation, 1);
    *observation.rx_bytes += 100;
    *observation.tx_bytes += 200;
    rates(first.network_rates.sample(observation, 2), 200, 100, "first Backend owns established rates");
    unknown_rates(second.network_rates.sample(observation, 2), "another Backend has independent first baseline");
    first.telemetry = Telemetry{};
    rates(first.network_rates.sample(observation, 3), 0, 0, "telemetry reset does not reset Backend history");
    std::cout << "backend ownership: independent history survives telemetry replacement\n";
}
}

int main() {
    try {
        classifier_tests();
        validation_tests();
        collector_tests();
        rate_tests();
        backend_history_tests();
        std::cout << "RECOVERY_NETWORK_OK " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "RECOVERY_NETWORK_FAILED after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
