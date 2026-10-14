// Read-only report of what the MFG unlock would find in real NVIDIA modules. Not part of CI: it needs
// the modules, which cannot be redistributed. Maps each one without running its DllMain
// (DONT_RESOLVE_DLL_REFERENCES) and writes nothing.
//
//   cl /nologo /EHsc /std:c++20 tests\mfg_real_module_check.cpp /Fe:x64\mfg_real_module_check.exe
//   x64\mfg_real_module_check.exe <nvngx_dlssg.dll or sl.dlss_g.dll> [...]
//
// Adapted from Susemi (3def828d) tests/mfg_real_module_check.cpp, without the PTX temporal fix, which is
// not part of this build.
#include "../OptiScaler/framegen/dlssg/MfgUnlockFlip.h"
#include "../OptiScaler/framegen/dlssg/MfgUnlockPlugin.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace MfgUnlock;

namespace
{
// Same pattern language as OptiScaler's scanner: hex bytes, '?' for any byte.
std::vector<int> ParsePattern(const char* text)
{
    std::vector<int> bytes;
    std::istringstream in(text);
    std::string token;

    while (in >> token)
        bytes.push_back(token == "?" ? -1 : static_cast<int>(std::stoul(token, nullptr, 16)));

    return bytes;
}

// Matches in executable sections, as scanner::GetAddress walks them.
size_t CountPattern(void* image, const char* text)
{
    const auto pattern = ParsePattern(text);
    size_t hits = 0;

    Provider::ForEachSection(image,
                             [&](uint8_t* data, size_t size, DWORD characteristics)
                             {
                                 if (!(characteristics & IMAGE_SCN_MEM_EXECUTE) || size < pattern.size())
                                     return;

                                 for (size_t off = 0; off + pattern.size() <= size; ++off)
                                 {
                                     size_t i = 0;
                                     while (i < pattern.size() && (pattern[i] < 0 || data[off + i] == pattern[i]))
                                         ++i;

                                     if (i == pattern.size())
                                         ++hits;
                                 }
                             });

    return hits;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: %s <module> [<module> ...]\n", argv[0]);
        return 2;
    }

    for (int a = 1; a < argc; ++a)
    {
        std::wstring path;

        for (const char* c = argv[a]; *c; ++c)
            path += static_cast<wchar_t>(static_cast<unsigned char>(*c));

        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        printf("\n== %s\n", argv[a]);

        if (module == nullptr)
        {
            printf("  could not be mapped (error %lu)\n", GetLastError());
            continue;
        }

        void* image = module;
        const bool ngx = GetProcAddress(module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") != nullptr ||
                         GetProcAddress(module, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") != nullptr;
        const bool directSr = GetProcAddress(module, "NVSDK_NGX_DirectSR_Create") != nullptr;

        printf("  provider path: %s; NGX entry point %s, DirectSR %s, marker %s\n",
               Provider::IsProviderPath(path) ? "yes" : "no", ngx ? "yes" : "no", directSr ? "yes" : "no",
               Provider::ImageContains(image, Provider::kMarker) ? "yes" : "no");

        printf("  frame-count gates: legacy advertise %zu, validate %zu; 310.9 advertise %zu, validate %zu (each must "
               "be exactly 1 in one shape)\n",
               CountPattern(image, "BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3"),
               CountPattern(image, "3D B0 01 00 00 7C ? 83 FB 03 76"),
               CountPattern(image, "81 FD B0 01 00 00 0F 8C ? ? ? ? BF 05 00 00 00"),
               CountPattern(image, "3D B0 01 00 00 0F 93 C0"));

        Plugin::CeilingSite site;
        const auto ceiling = Plugin::FindCeilingSite(image, site);
        printf("  plugin frame-count clamp: %s",
               ceiling == Plugin::FindResult::Found       ? "found"
               : ceiling == Plugin::FindResult::Ambiguous ? "AMBIGUOUS (refused)"
                                                          : "not found");
        if (ceiling == Plugin::FindResult::Found)
            printf(", compiled maximum %u generated frame(s) at +0x%llx", site.compiled,
                   static_cast<unsigned long long>(site.address - static_cast<uint8_t*>(image)));
        printf("\n");

        Flip::Plan plan;
        const auto flip = Flip::FindPlan(image, plan);
        printf("  plugin flip metering: %s", Flip::Describe(flip));
        if (flip == Flip::FindResult::Found)
        {
            printf(": context +0x%X pinned to %u at %zu site(s)\n", plan.field, plan.value, plan.sites.size());

            for (const auto& store : plan.sites)
            {
                printf("    +0x%llx %s: ", static_cast<unsigned long long>(store.address - static_cast<uint8_t*>(image)),
                       store.fromRegister ? "register store" : "immediate store");

                for (uint8_t b : store.original)
                    printf("%02X ", b);
                printf("-> ");
                for (uint8_t b : store.replacement)
                    printf("%02X ", b);
                printf("\n");
            }
        }
        else
        {
            printf("\n");
        }

        FreeLibrary(module);
    }

    return 0;
}
