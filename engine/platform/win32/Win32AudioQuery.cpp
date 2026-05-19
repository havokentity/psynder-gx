// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/win32/Win32AudioQuery.cpp
//
// Lane 23 — WASAPI default-endpoint device-name + sample-rate query.
//
// Exposes psynder::platform::win32::default_audio_device_name() and
// default_audio_sample_rate() to mirror the lane-25 CoreAudio equivalents
// (PublicPlatformMacos.h) so lane 14 can open its host backend without
// per-OS ifdefs.
//
// NEEDS PC VALIDATION — the orchestrator runs macOS; runtime correctness must
// be confirmed on a Windows build.

#include "PublicPlatformWin32.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Common.h"

#include <propvarutil.h>   // PropVariantInit / PropVariantClear
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <array>
#include <cstring>

// ole32 is already linked by CMakeLists.txt (Win32 core libs).
// Propsys is needed for PropVariantInit / PropVariantClear.
#pragma comment(lib, "Propsys.lib")

namespace psynder::platform::win32 {

namespace {

// Shared WASAPI initialisation: create enumerator + open default render endpoint.
// Returns nullptr on any failure.  Caller owns the ref count.
ComPtr<IMMDevice> open_default_render_device() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (!psy_hr_ok(hr, "CoCreateInstance(MMDeviceEnumerator) [query]")) {
        return nullptr;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
    if (!psy_hr_ok(hr, "GetDefaultAudioEndpoint [query]")) {
        return nullptr;
    }
    return device;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// default_audio_device_name
// ─────────────────────────────────────────────────────────────────────────────
const char* default_audio_device_name() {
    // Static buffer: valid until the next call — same contract as lane 25.
    static std::array<char, 512> s_name_buf{};
    s_name_buf[0] = '\0';

    ComScope com_scope;  // Idempotent; COINIT_MULTITHREADED.

    ComPtr<IMMDevice> device = open_default_render_device();
    if (!device) return s_name_buf.data();

    ComPtr<IPropertyStore> props;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    if (!psy_hr_ok(hr, "IMMDevice::OpenPropertyStore")) return s_name_buf.data();

    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    hr = props->GetValue(PKEY_Device_FriendlyName, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        // Convert the UTF-16 friendly name to UTF-8 for the static buffer.
        const int needed = ::WideCharToMultiByte(
            CP_UTF8, 0, pv.pwszVal, -1,
            s_name_buf.data(), static_cast<int>(s_name_buf.size() - 1u),
            nullptr, nullptr);
        if (needed > 0) {
            s_name_buf[static_cast<std::size_t>(needed)] = '\0';
        }
    }
    ::PropVariantClear(&pv);

    return s_name_buf.data();
}

// ─────────────────────────────────────────────────────────────────────────────
// default_audio_sample_rate
// ─────────────────────────────────────────────────────────────────────────────
std::uint32_t default_audio_sample_rate() {
    ComScope com_scope;

    ComPtr<IMMDevice> device = open_default_render_device();
    if (!device) return 0u;

    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf()));
    if (!psy_hr_ok(hr, "IMMDevice::Activate(IAudioClient) [query]")) return 0u;

    WAVEFORMATEX* mix_fmt = nullptr;
    hr = client->GetMixFormat(&mix_fmt);
    if (!psy_hr_ok(hr, "IAudioClient::GetMixFormat [query]")) return 0u;

    // Guard: Windows allocates mix_fmt via CoTaskMem; free on exit.
    struct FmtGuard {
        WAVEFORMATEX* p;
        ~FmtGuard() { if (p) ::CoTaskMemFree(p); }
    } guard{mix_fmt};

    return static_cast<std::uint32_t>(mix_fmt->nSamplesPerSec);
}

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
