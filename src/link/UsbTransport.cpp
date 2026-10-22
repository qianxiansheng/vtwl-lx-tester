#include "Transport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <initguid.h>
#include <usbiodef.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <thread>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace Lx::Link {

namespace {

// ---------------------------------------------------------------- 工具

std::string toUtf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring toWide(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::wstring upper(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
	return s;
}

std::string errorText(DWORD err)
{
	wchar_t* buf = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
	std::wstring msg = buf ? buf : L"";
	if (buf) LocalFree(buf);
	while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L'.' || msg.back() == L'。'))
		msg.pop_back();
	return toUtf8(msg) + "（" + std::to_string(err) + "）";
}

std::vector<std::wstring> splitMultiSz(const wchar_t* p)
{
	std::vector<std::wstring> out;
	while (p && *p) {
		out.emplace_back(p);
		p += out.back().size() + 1;
	}
	return out;
}

std::wstring regProperty(HDEVINFO h, SP_DEVINFO_DATA& d, DWORD prop)
{
	DWORD type = 0, size = 0;
	SetupDiGetDeviceRegistryPropertyW(h, &d, prop, &type, nullptr, 0, &size);
	if (size == 0) return {};
	std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
	if (!SetupDiGetDeviceRegistryPropertyW(h, &d, prop, &type, reinterpret_cast<PBYTE>(buf.data()), size, nullptr))
		return {};
	return buf.data();
}

// WinUSB 设备（INF 安装或 MS OS 描述符自动安装）在设备注册表键中登记接口 GUID
std::vector<GUID> interfaceGuids(HDEVINFO h, SP_DEVINFO_DATA& d)
{
	std::vector<GUID> out;
	HKEY key = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
	if (key == INVALID_HANDLE_VALUE) return out;
	for (const wchar_t* name : { L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID" }) {
		DWORD type = 0, size = 0;
		if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size == 0) continue;
		std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
		if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS) continue;
		for (const auto& s : splitMultiSz(buf.data())) {
			GUID g;
			if (SUCCEEDED(CLSIDFromString(s.c_str(), &g))) out.push_back(g);
		}
	}
	RegCloseKey(key);
	return out;
}

std::vector<std::wstring> interfacePaths(const GUID& g, const std::wstring& instanceId)
{
	GUID guid = g;
	ULONG len = 0;
	if (CM_Get_Device_Interface_List_SizeW(&len, &guid, const_cast<DEVINSTID_W>(instanceId.c_str()),
			CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len <= 1)
		return {};
	std::vector<wchar_t> buf(len, L'\0');
	if (CM_Get_Device_Interface_ListW(&guid, const_cast<DEVINSTID_W>(instanceId.c_str()), buf.data(), len,
			CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
		return {};
	return splitMultiSz(buf.data());
}

// 一个设备节点的信息；path 为空表示找不到可打开的接口
struct DeviceNode {
	std::wstring instanceId;
	std::wstring service;      // 驱动服务名（WinUSB / usbccgp / usbser ...）
	std::wstring name;
	std::wstring location;
	std::wstring path;
};

DeviceNode describe(HDEVINFO h, SP_DEVINFO_DATA& d)
{
	DeviceNode n;
	wchar_t id[MAX_DEVICE_ID_LEN] = {};
	if (SetupDiGetDeviceInstanceIdW(h, &d, id, MAX_DEVICE_ID_LEN, nullptr)) n.instanceId = id;
	n.service = regProperty(h, d, SPDRP_SERVICE);
	n.name = regProperty(h, d, SPDRP_FRIENDLYNAME);
	if (n.name.empty()) n.name = regProperty(h, d, SPDRP_DEVICEDESC);
	n.location = regProperty(h, d, SPDRP_LOCATION_INFORMATION);

	auto guids = interfaceGuids(h, d);
	if (upper(n.service) == L"WINUSB") guids.push_back(GUID_DEVINTERFACE_USB_DEVICE);   // 兜底
	for (const auto& g : guids) {
		auto paths = interfacePaths(g, n.instanceId);
		if (!paths.empty()) { n.path = paths.front(); break; }
	}
	return n;
}

bool isWinUsb(const DeviceNode& n) { return upper(n.service) == L"WINUSB"; }

// ---------------------------------------------------------------- 传输

class UsbTransport : public ITransport {
public:
	explicit UsbTransport(std::string instanceId) : m_name(std::move(instanceId)) {}
	~UsbTransport() override { close(); }

	bool open(DataHandler onData, ClosedHandler onClosed, std::string& err) override
	{
		close();

		// 1. 按实例 ID 找到设备节点和 WinUSB 接口路径
		DeviceNode node;
		{
			HDEVINFO h = SetupDiCreateDeviceInfoList(nullptr, nullptr);
			if (h == INVALID_HANDLE_VALUE) { err = errorText(GetLastError()); return false; }
			SP_DEVINFO_DATA d{ sizeof(SP_DEVINFO_DATA) };
			const std::wstring id = toWide(m_name);
			const bool found = SetupDiOpenDeviceInfoW(h, id.c_str(), nullptr, 0, &d) != FALSE;
			if (found) node = describe(h, d);
			SetupDiDestroyDeviceInfoList(h);
			if (!found) { err = "设备不在位（未插入或实例 ID 已变化），请刷新端口"; return false; }
		}
		if (!isWinUsb(node)) {
			err = "设备未绑定 WinUSB 驱动（当前驱动：" + (node.service.empty() ? std::string("无") : toUtf8(node.service)) + "）";
			return false;
		}
		if (node.path.empty()) { err = "找不到设备的 WinUSB 接口"; return false; }

		// 2. 打开设备并初始化 WinUSB（句柄必须带 FILE_FLAG_OVERLAPPED）
		m_file = CreateFileW(node.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
		if (m_file == INVALID_HANDLE_VALUE) {
			const DWORD e = GetLastError();
			err = e == ERROR_ACCESS_DENIED ? "设备已被其他程序占用" : "打开设备失败：" + errorText(e);
			return false;
		}
		if (!WinUsb_Initialize(m_file, &m_usb)) {
			err = "WinUSB 初始化失败：" + errorText(GetLastError());
			releaseHandles();
			return false;
		}

		// 3. 找 IN / OUT 端点：优先批量，其次中断
		USB_INTERFACE_DESCRIPTOR ifd{};
		if (!WinUsb_QueryInterfaceSettings(m_usb, 0, &ifd)) {
			err = "读取接口描述符失败：" + errorText(GetLastError());
			releaseHandles();
			return false;
		}
		int inScore = 0, outScore = 0;
		for (UCHAR i = 0; i < ifd.bNumEndpoints; ++i) {
			WINUSB_PIPE_INFORMATION pipe{};
			if (!WinUsb_QueryPipe(m_usb, 0, i, &pipe)) continue;
			const int score = pipe.PipeType == UsbdPipeTypeBulk ? 2 : pipe.PipeType == UsbdPipeTypeInterrupt ? 1 : 0;
			if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
				if (score > inScore) { inScore = score; m_inPipe = pipe.PipeId; m_inPacket = pipe.MaximumPacketSize; }
			}
			else if (score > outScore) {
				outScore = score; m_outPipe = pipe.PipeId;
			}
		}
		if (!inScore || !outScore) {
			err = "设备接口缺少批量/中断 IN、OUT 端点";
			releaseHandles();
			return false;
		}
		ULONG writeTimeout = 1000;
		WinUsb_SetPipePolicy(m_usb, m_outPipe, PIPE_TRANSFER_TIMEOUT, sizeof(writeTimeout), &writeTimeout);
		WinUsb_FlushPipe(m_usb, m_inPipe);

		m_onData = std::move(onData);
		m_onClosed = std::move(onClosed);
		m_running = true;
		m_readerDone = false;
		m_reader = std::thread(&UsbTransport::readLoop, this);
		return true;
	}

	void close() override
	{
		m_running = false;
		if (m_reader.joinable()) {
			// 读线程可能刚好在检查 m_running 之后才发起读，单次 Abort 可能落空，反复取消直到线程退出
			while (!m_readerDone) {
				WinUsb_AbortPipe(m_usb, m_inPipe);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			m_reader.join();
		}
		std::lock_guard<std::mutex> lk(m_writeMutex);
		releaseHandles();
	}

	bool send(const std::vector<uint8_t>& data) override
	{
		std::lock_guard<std::mutex> lk(m_writeMutex);
		if (!m_usb || !m_running) return false;
		ULONG written = 0;
		return WinUsb_WritePipe(m_usb, m_outPipe, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
			&written, nullptr) && written == data.size();   // 失败由读线程检测断开
	}

	const std::string& name() const override { return m_name; }

private:
	void readLoop()
	{
		std::vector<uint8_t> buf(std::max<size_t>(512, m_inPacket));
		OVERLAPPED ov{};
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

		while (m_running) {
			ResetEvent(ov.hEvent);
			ULONG n = 0;
			BOOL ok = WinUsb_ReadPipe(m_usb, m_inPipe, buf.data(), static_cast<ULONG>(buf.size()), &n, &ov);
			DWORD e = ok ? ERROR_SUCCESS : GetLastError();
			if (!ok && e == ERROR_IO_PENDING) {
				ok = WinUsb_GetOverlappedResult(m_usb, &ov, &n, TRUE);
				e = ok ? ERROR_SUCCESS : GetLastError();
			}
			if (!m_running) break;
			if (ok) {
				if (n > 0 && m_onData) m_onData(std::vector<uint8_t>(buf.begin(), buf.begin() + n));
				continue;
			}
			// 拔出 / 设备复位 / 其它错误：视为断开
			if (m_running.exchange(false) && m_onClosed) m_onClosed(errorText(e));
			break;
		}
		CloseHandle(ov.hEvent);
		m_readerDone = true;
	}

	void releaseHandles()
	{
		if (m_usb) { WinUsb_Free(m_usb); m_usb = nullptr; }
		if (m_file != INVALID_HANDLE_VALUE) { CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE; }
	}

	std::string m_name;
	HANDLE      m_file = INVALID_HANDLE_VALUE;
	WINUSB_INTERFACE_HANDLE m_usb = nullptr;
	UCHAR       m_inPipe = 0, m_outPipe = 0;
	USHORT      m_inPacket = 64;
	std::mutex  m_writeMutex;
	std::thread m_reader;
	std::atomic<bool> m_running{ false };
	std::atomic<bool> m_readerDone{ true };
	DataHandler   m_onData;
	ClosedHandler m_onClosed;
};

} // namespace

std::string usbShortName(const std::string& instanceId)
{
	const auto pos = instanceId.find_last_of('\\');
	return pos == std::string::npos ? instanceId : instanceId.substr(pos + 1);
}

std::vector<UsbDeviceInfo> enumerateUsbDevices(uint16_t vid, uint16_t pid)
{
	std::vector<UsbDeviceInfo> out;
	wchar_t match[32];
	swprintf_s(match, L"VID_%04X&PID_%04X", vid, pid);

	HDEVINFO h = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
	if (h == INVALID_HANDLE_VALUE) return out;

	SP_DEVINFO_DATA d{ sizeof(SP_DEVINFO_DATA) };
	for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); ++i) {
		wchar_t id[MAX_DEVICE_ID_LEN] = {};
		if (!SetupDiGetDeviceInstanceIdW(h, &d, id, MAX_DEVICE_ID_LEN, nullptr)) continue;
		if (upper(id).find(match) == std::wstring::npos) continue;

		const DeviceNode n = describe(h, d);
		if (upper(n.service) == L"USBCCGP") continue;   // 复合设备父节点：只列它的各个接口

		UsbDeviceInfo info;
		info.instanceId = toUtf8(n.instanceId);
		info.label = usbShortName(info.instanceId);
		info.winusb = isWinUsb(n) && !n.path.empty();
		info.description = toUtf8(n.name);
		if (!n.location.empty()) info.description += " · " + toUtf8(n.location);
		if (!info.winusb)
			info.description += "（驱动：" + (n.service.empty() ? std::string("无") : toUtf8(n.service)) + "，非 WinUSB 无法使用）";
		out.push_back(std::move(info));
	}
	SetupDiDestroyDeviceInfoList(h);

	std::sort(out.begin(), out.end(), [](const UsbDeviceInfo& a, const UsbDeviceInfo& b) { return a.instanceId < b.instanceId; });
	return out;
}

std::unique_ptr<ITransport> makeUsbTransport(const std::string& instanceId)
{
	return std::make_unique<UsbTransport>(instanceId);
}

} // namespace Lx::Link
