/*
 * Copyright (C) 2021-2022 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu_platform_detail.h"

#include <multipass/file_ops.h>
#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/utils.h>

#include <shared/linux/backend_utils.h>

#include <QFile>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "qemu platform";
const QString multipass_bridge_name{"mpqemubr0"};

// An interface name can only be 15 characters, so this generates a hash of the
// VM instance name with a "tap-" prefix and then truncates it.
auto generate_tap_device_name(const std::string& vm_name)
{
    const auto name_hash = std::hash<std::string>{}(vm_name);
    auto tap_name = fmt::format("tap-{:x}", name_hash);
    tap_name.resize(15);
    return QString::fromStdString(tap_name);
}

void create_tap_device(const QString& tap_name, const QString& bridge_name)
{
    if (!MP_UTILS.run_cmd_for_status("ip", {"addr", "show", tap_name}))
    {
        MP_UTILS.run_cmd_for_status("ip", {"tuntap", "add", tap_name, "mode", "tap"});
        MP_UTILS.run_cmd_for_status("ip", {"link", "set", tap_name, "master", bridge_name});
        MP_UTILS.run_cmd_for_status("ip", {"link", "set", tap_name, "up"});
    }
}

void remove_tap_device(const QString& tap_device_name)
{
    if (MP_UTILS.run_cmd_for_status("ip", {"addr", "show", tap_device_name}))
    {
        MP_UTILS.run_cmd_for_status("ip", {"link", "delete", tap_device_name});
    }
}

void create_virtual_switch(const std::string& subnet, const QString& bridge_name)
{
    if (!MP_UTILS.run_cmd_for_status("ip", {"addr", "show", bridge_name}))
    {
        const auto mac_address = mp::utils::generate_mac_address();
        const auto cidr = fmt::format("{}.1/24", subnet);
        const auto broadcast = fmt::format("{}.255", subnet);

        MP_UTILS.run_cmd_for_status("ip",
                                    {"link", "add", bridge_name, "address", mac_address.c_str(), "type", "bridge"});
        MP_UTILS.run_cmd_for_status(
            "ip", {"address", "add", cidr.c_str(), "dev", bridge_name, "broadcast", broadcast.c_str()});
        MP_UTILS.run_cmd_for_status("ip", {"link", "set", bridge_name, "up"});
    }
}

void set_ip_forward()
{
    // Command line equivalent: "sysctl -w net.ipv4.ip_forward=1"
    QFile ip_forward("/proc/sys/net/ipv4/ip_forward");
    if (!MP_FILEOPS.open(ip_forward, QFile::ReadWrite))
    {
        mpl::log(mpl::Level::warning, category,
                 fmt::format("Unable to open {}", qUtf8Printable(ip_forward.fileName())));
        return;
    }

    if (MP_FILEOPS.write(ip_forward, "1") < 0)
    {
        mpl::log(mpl::Level::warning, category,
                 fmt::format("Failed to write to {}", qUtf8Printable(ip_forward.fileName())));
    }
}

mp::DNSMasqServer::UPtr init_nat_network(const mp::Path& network_dir, const QString& bridge_name,
                                         const std::string& subnet)
{
    create_virtual_switch(subnet, bridge_name);
    set_ip_forward();

    return MP_DNSMASQ_SERVER_FACTORY.make_dnsmasq_server(network_dir, bridge_name, subnet);
}

void delete_virtual_switch(const QString& bridge_name)
{
    if (MP_UTILS.run_cmd_for_status("ip", {"addr", "show", bridge_name}))
    {
        MP_UTILS.run_cmd_for_status("ip", {"link", "delete", bridge_name});
    }
}
} // namespace

mp::QemuPlatformDetail::QemuPlatformDetail(const mp::Path& data_dir)
    : bridge_name{multipass_bridge_name},
      network_dir{mp::utils::make_dir(QDir(data_dir), "network")},
      subnet{MP_BACKEND.get_subnet(network_dir, bridge_name)},
      dnsmasq_server{init_nat_network(network_dir, bridge_name, subnet)},
      firewall_config{MP_FIREWALL_CONFIG_FACTORY.make_firewall_config(bridge_name, subnet)}
{
}

mp::QemuPlatformDetail::~QemuPlatformDetail()
{
    for (const auto& it : name_to_net_device_map)
    {
        const auto& [tap_device_name, hw_addr] = it.second;
        remove_tap_device(tap_device_name);
    }

    delete_virtual_switch(bridge_name);
}

mp::optional<mp::IPAddress> mp::QemuPlatformDetail::get_ip_for(const std::string& hw_addr)
{
    return dnsmasq_server->get_ip_for(hw_addr);
}

void mp::QemuPlatformDetail::remove_resources_for(const std::string& name)
{
    auto it = name_to_net_device_map.find(name);
    if (it != name_to_net_device_map.end())
    {
        const auto& [tap_device_name, hw_addr] = it->second;
        dnsmasq_server->release_mac(hw_addr);
        remove_tap_device(tap_device_name);

        name_to_net_device_map.erase(name);
    }
}

void mp::QemuPlatformDetail::platform_health_check()
{
    MP_BACKEND.check_for_kvm_support();
    MP_BACKEND.check_if_kvm_is_in_use();

    dnsmasq_server->check_dnsmasq_running();
    firewall_config->verify_firewall_rules();
}

QStringList mp::QemuPlatformDetail::vm_platform_args(const VirtualMachineDescription& vm_desc)
{
    // Configure and generate the args for the default network interface
    auto tap_device_name = generate_tap_device_name(vm_desc.vm_name);
    create_tap_device(tap_device_name, bridge_name);

    name_to_net_device_map.emplace(vm_desc.vm_name, std::make_pair(tap_device_name, vm_desc.default_mac_address));

    return QStringList() << "--enable-kvm"
                         // Pass host CPU flags to VM
                         << "-cpu"
                         << "host"
                         // Set up the network related args
                         << "-nic"
                         << QString::fromStdString(
                                fmt::format("tap,ifname={},script=no,downscript=no,model=virtio-net-pci,mac={}",
                                            tap_device_name, vm_desc.default_mac_address));
}

mp::QemuPlatform::UPtr mp::QemuPlatformFactory::make_qemu_platform(const Path& data_dir) const
{
    return std::make_unique<mp::QemuPlatformDetail>(data_dir);
}
