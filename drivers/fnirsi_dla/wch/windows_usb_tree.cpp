/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/windows_usb_drivers.h"

#if defined(_WIN32)

#include "wch/wch_device_system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <usbioctl.h>

#include <cwchar>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

constexpr GUID kWchInterface = {0x5e7f6bdf, 0x1ce5, 0x4d78, {0xbb, 0xcf, 0xd2, 0x0c, 0x44, 0x32, 0x9f, 0x7d}};
constexpr ULONG kUsbDeviceFilter = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;

struct Placement {
  std::vector<uint32_t> addresses;
  std::string rootHub;
};

std::string narrow(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                       nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::optional<std::wstring> instanceIdOf(DEVINST node) {
  ULONG length = 0;
  if (CM_Get_Device_ID_Size(&length, node, 0) != CR_SUCCESS) return std::nullopt;
  std::wstring id(static_cast<size_t>(length) + 1, L'\0');
  if (CM_Get_Device_IDW(node, id.data(), static_cast<ULONG>(id.size()), 0) != CR_SUCCESS) return std::nullopt;
  id.resize(std::wcslen(id.c_str()));
  return id;
}

std::optional<uint32_t> uint32Property(DEVINST node, const DEVPROPKEY& key) {
  DEVPROPTYPE type = 0;
  ULONG value = 0;
  ULONG size = sizeof value;
  if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(&value), &size, 0) != CR_SUCCESS) {
    return std::nullopt;
  }
  if (type != DEVPROP_TYPE_UINT32) return std::nullopt;
  return value;
}

std::string stringProperty(DEVINST node, const DEVPROPKEY& key) {
  DEVPROPTYPE type = 0;
  ULONG size = 0;
  if (CM_Get_DevNode_PropertyW(node, &key, &type, nullptr, &size, 0) != CR_BUFFER_SMALL) return {};
  if (type != DEVPROP_TYPE_STRING || size < sizeof(wchar_t)) return {};
  std::wstring text(size / sizeof(wchar_t), L'\0');
  if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(text.data()), &size, 0) != CR_SUCCESS) {
    return {};
  }
  text.resize(std::wcslen(text.c_str()));
  return narrow(text);
}

std::optional<Placement> placementOf(DEVINST device) {
  Placement placement;
  DEVINST node = device;
  for (size_t tier = 0; tier < kMaxWindowsUsbPortDepth; ++tier) {
    const auto address = uint32Property(node, DEVPKEY_Device_Address);
    if (!address) return std::nullopt;
    placement.addresses.push_back(*address);
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return std::nullopt;
    const auto parentId = instanceIdOf(parent);
    if (!parentId) return std::nullopt;
    std::string narrowed = narrow(*parentId);
    if (isUsbRootHubInstanceId(narrowed)) {
      placement.rootHub = std::move(narrowed);
      return placement;
    }
    node = parent;
  }
  return std::nullopt;
}

std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::wstring firstInterfacePath(const GUID& interfaceClass, std::wstring instanceId) {
  GUID guid = interfaceClass;
  constexpr ULONG kPresent = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
  ULONG length = 0;
  if (CM_Get_Device_Interface_List_SizeW(&length, &guid, instanceId.data(), kPresent) != CR_SUCCESS) return {};
  if (length <= 1) return {};
  std::wstring list(length, L'\0');
  if (CM_Get_Device_Interface_ListW(&guid, instanceId.data(), list.data(), length, kPresent) != CR_SUCCESS) return {};
  return std::wstring(list.c_str());
}

class HubHandle {
 public:
  explicit HubHandle(const std::wstring& path)
      : handle_(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)) {}
  HubHandle(const HubHandle&) = delete;
  HubHandle& operator=(const HubHandle&) = delete;
  ~HubHandle() {
    if (valid()) CloseHandle(handle_);
  }
  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_;
};

std::string wchInterfacePath(std::wstring instanceId) {
  return narrow(firstInterfacePath(kWchInterface, std::move(instanceId)));
}

}

std::expected<std::vector<WindowsUsbDevice>, std::string> listWindowsUsbDevices() {
  ULONG length = 0;
  if (const CONFIGRET result = CM_Get_Device_ID_List_SizeW(&length, L"USB", kUsbDeviceFilter); result != CR_SUCCESS) {
    return std::unexpected(std::format("Windows refused USB device list size (error {:#x})", result));
  }
  std::wstring ids(length, L'\0');
  if (const CONFIGRET result = CM_Get_Device_ID_ListW(L"USB", ids.data(), length, kUsbDeviceFilter);
      result != CR_SUCCESS) {
    return std::unexpected(std::format("Windows refused USB device list (error {:#x})", result));
  }

  std::vector<WindowsUsbDevice> devices;
  const wchar_t* const end = ids.c_str() + ids.size();
  for (wchar_t* id = ids.data(); id < end && *id != L'\0'; id += std::wcslen(id) + 1) {
    std::string narrowed = narrow(id);
    const auto identity = parseUsbInstanceId(narrowed);
    if (!identity) continue;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
    const auto placement = placementOf(node);
    if (!placement) continue;
    auto ports = portPathOfAddresses(placement->addresses);
    if (!ports) continue;

    WindowsUsbDevice device;
    device.instanceId = std::move(narrowed);
    device.vendorId = identity->vendorId;
    device.productId = identity->productId;
    device.ports = std::move(*ports);
    device.rootHub = placement->rootHub;
    device.service = stringProperty(node, DEVPKEY_Device_Service);
    if (usbPathKindOfService(device.service) == UsbPathKind::wch) device.wchDevicePath = wchInterfacePath(id);
    devices.push_back(std::move(device));
  }
  return devices;
}

UsbLinkSpeed windowsUsbLinkSpeed(const WindowsUsbDevice& device) {
  if (device.ports.empty()) return UsbLinkSpeed::unknown;
  std::wstring id = widen(device.instanceId);
  DEVINST node = 0;
  if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return UsbLinkSpeed::unknown;
  DEVINST parent = 0;
  if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return UsbLinkSpeed::unknown;
  const auto parentId = instanceIdOf(parent);
  if (!parentId) return UsbLinkSpeed::unknown;
  const std::wstring hubPath = firstInterfacePath(GUID_DEVINTERFACE_USB_HUB, *parentId);
  if (hubPath.empty()) return UsbLinkSpeed::unknown;
  const HubHandle hub(hubPath);
  if (!hub.valid()) return UsbLinkSpeed::unknown;
  const ULONG port = device.ports.back();

  USB_NODE_CONNECTION_INFORMATION_EX_V2 modern{};
  modern.ConnectionIndex = port;
  modern.Length = sizeof modern;
  modern.SupportedUsbProtocols.Usb300 = 1;
  DWORD returned = 0;
  bool superSpeedPlus = false;
  bool superSpeed = false;
  if (DeviceIoControl(hub.get(), IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, &modern, sizeof modern, &modern,
                      sizeof modern, &returned, nullptr)) {
    superSpeedPlus = modern.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher != 0;
    superSpeed = modern.Flags.DeviceIsOperatingAtSuperSpeedOrHigher != 0;
  }

  constexpr size_t kPipeRoom = 32;
  std::vector<uint8_t> legacyBuffer(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + kPipeRoom * sizeof(USB_PIPE_INFO));
  auto* legacy = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(legacyBuffer.data());
  legacy->ConnectionIndex = port;
  std::optional<int> legacySpeed;
  const auto legacySize = static_cast<DWORD>(legacyBuffer.size());
  if (DeviceIoControl(hub.get(), IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, legacy, legacySize, legacy, legacySize,
                      &returned, nullptr)) {
    legacySpeed = static_cast<int>(legacy->Speed);
  }
  return usbLinkSpeedOfHub(superSpeedPlus, superSpeed, legacySpeed);
}

}

#else

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

std::expected<std::vector<WindowsUsbDevice>, std::string> listWindowsUsbDevices() {
  return std::vector<WindowsUsbDevice>{};
}

UsbLinkSpeed windowsUsbLinkSpeed([[maybe_unused]] const WindowsUsbDevice& device) { return UsbLinkSpeed::unknown; }

}

#endif
