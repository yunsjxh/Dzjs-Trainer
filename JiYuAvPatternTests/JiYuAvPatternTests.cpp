#define NOMINMAX
#include <Windows.h>

#include <iostream>
#include <vector>

#include "..\JiYuAvShared\PatternMatcher.h"
#include "..\JiYuAvShared\SharedPatternBuilder.h"

namespace {

bool Expect(bool condition, const char* name)
{
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        return false;
    }
    std::cout << "PASS " << name << "\n";
    return true;
}

bool FindPattern(
    const unsigned char* data,
    unsigned long dataLength,
    const unsigned char* pattern,
    const unsigned char* mask,
    unsigned long patternLength,
    unsigned long* offset)
{
    if (patternLength == 0UL || dataLength < patternLength) return false;
    for (unsigned long index = 0; index <= dataLength - patternLength; ++index) {
        if (JiYuAvPatternMatches(data + index, pattern, mask, patternLength)) {
            *offset = index;
            return true;
        }
    }
    return false;
}

bool ReadWholeFile(const wchar_t* path, std::vector<unsigned char>* bytes)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 512LL * 1024LL * 1024LL) {
        CloseHandle(file);
        return false;
    }
    bytes->resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const DWORD request = static_cast<DWORD>((std::min<std::size_t>)(
            bytes->size() - offset, 4UL * 1024UL * 1024UL));
        DWORD read = 0;
        if (!ReadFile(file, bytes->data() + offset, request, &read, nullptr) || read == 0) {
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool LoadExecutableCode(
    const wchar_t* path,
    std::vector<unsigned char>* code,
    std::vector<unsigned char>* stableMask,
    std::size_t* entryOffset)
{
    std::vector<unsigned char> file;
    if (!ReadWholeFile(path, &file) || file.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    if (ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > file.size() ||
        *reinterpret_cast<const DWORD*>(file.data() + ntOffset) != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_FILE_HEADER* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(
        file.data() + ntOffset + sizeof(DWORD));
    const std::size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (optionalOffset + fileHeader->SizeOfOptionalHeader > file.size()) return false;
    const WORD magic = *reinterpret_cast<const WORD*>(file.data() + optionalOffset);
    DWORD entryRva = 0;
    IMAGE_DATA_DIRECTORY relocationDirectory = {};
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        const IMAGE_OPTIONAL_HEADER32* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(file.data() + optionalOffset);
        entryRva = optional->AddressOfEntryPoint;
        if (optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
            relocationDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    }
    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        const IMAGE_OPTIONAL_HEADER64* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(file.data() + optionalOffset);
        entryRva = optional->AddressOfEntryPoint;
        if (optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
            relocationDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    }
    else return false;

    const std::size_t sectionOffset = optionalOffset + fileHeader->SizeOfOptionalHeader;
    if (sectionOffset + static_cast<std::size_t>(fileHeader->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > file.size()) return false;
    const IMAGE_SECTION_HEADER* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file.data() + sectionOffset);
    const IMAGE_SECTION_HEADER* selected = nullptr;
    for (WORD index = 0; index < fileHeader->NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.SizeOfRawData == 0) continue;
        const DWORD virtualSize = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (entryRva >= section.VirtualAddress && entryRva < section.VirtualAddress + virtualSize) {
            selected = &section;
            break;
        }
        if (!selected && (section.Characteristics & IMAGE_SCN_CNT_CODE) != 0) selected = &section;
    }
    if (!selected || selected->PointerToRawData >= file.size()) return false;
    const std::size_t rawSize = (std::min<std::size_t>)(
        selected->SizeOfRawData, file.size() - selected->PointerToRawData);
    if (rawSize < 48U) return false;
    code->assign(file.begin() + selected->PointerToRawData,
        file.begin() + selected->PointerToRawData + rawSize);
    stableMask->assign(rawSize, 0xFFU);
    auto rvaToRawOffset = [&](DWORD rva, std::size_t* rawOffset) -> bool {
        for (WORD index = 0; index < fileHeader->NumberOfSections; ++index) {
            const IMAGE_SECTION_HEADER& section = sections[index];
            const DWORD span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva < section.VirtualAddress || rva >= section.VirtualAddress + span) continue;
            const DWORD delta = rva - section.VirtualAddress;
            if (delta >= section.SizeOfRawData || section.PointerToRawData + delta >= file.size()) return false;
            *rawOffset = static_cast<std::size_t>(section.PointerToRawData) + delta;
            return true;
        }
        return false;
    };
    if (relocationDirectory.VirtualAddress != 0 && relocationDirectory.Size >= sizeof(IMAGE_BASE_RELOCATION)) {
        std::size_t relocationRawOffset = 0;
        if (rvaToRawOffset(relocationDirectory.VirtualAddress, &relocationRawOffset)) {
            const std::size_t available = (std::min<std::size_t>)(
                relocationDirectory.Size, file.size() - relocationRawOffset);
            std::size_t cursor = 0;
            while (cursor + sizeof(IMAGE_BASE_RELOCATION) <= available) {
                const IMAGE_BASE_RELOCATION* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                    file.data() + relocationRawOffset + cursor);
                if (block->SizeOfBlock < sizeof(*block) || block->SizeOfBlock > available - cursor) break;
                const WORD* entries = reinterpret_cast<const WORD*>(block + 1);
                const std::size_t entryCount = (block->SizeOfBlock - sizeof(*block)) / sizeof(WORD);
                for (std::size_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
                    const WORD entry = entries[entryIndex];
                    const WORD type = entry >> 12;
                    std::size_t width = 0;
                    if (type == IMAGE_REL_BASED_HIGHLOW) width = 4;
                    else if (type == IMAGE_REL_BASED_DIR64) width = 8;
                    else if (type == IMAGE_REL_BASED_HIGH || type == IMAGE_REL_BASED_LOW || type == IMAGE_REL_BASED_HIGHADJ) width = 2;
                    if (type == IMAGE_REL_BASED_HIGHADJ && entryIndex + 1 < entryCount) ++entryIndex;
                    if (width == 0) continue;
                    const DWORD targetRva = block->VirtualAddress + (entry & 0x0FFFU);
                    if (targetRva < selected->VirtualAddress) continue;
                    const std::size_t sectionByte = static_cast<std::size_t>(targetRva - selected->VirtualAddress);
                    for (std::size_t byteIndex = 0; byteIndex < width && sectionByte + byteIndex < stableMask->size(); ++byteIndex)
                        (*stableMask)[sectionByte + byteIndex] = 0U;
                }
                cursor += block->SizeOfBlock;
            }
        }
    }
    *entryOffset = entryRva >= selected->VirtualAddress
        ? static_cast<std::size_t>(entryRva - selected->VirtualAddress)
        : 0U;
    if (*entryOffset >= code->size()) *entryOffset = 0U;
    return true;
}

int TestExecutableSamples(int argumentCount, wchar_t** arguments)
{
    std::vector<std::vector<unsigned char>> code(static_cast<std::size_t>(argumentCount - 1));
    std::vector<std::vector<unsigned char>> stableMasks(code.size());
    std::vector<std::size_t> entryOffsets(code.size(), 0U);
    std::vector<JIYU_AV_PATTERN_SAMPLE_VIEW> samples(code.size());
    for (int index = 1; index < argumentCount; ++index) {
        const std::size_t sampleIndex = static_cast<std::size_t>(index - 1);
        if (!LoadExecutableCode(arguments[index], &code[sampleIndex], &stableMasks[sampleIndex], &entryOffsets[sampleIndex])) {
            std::wcerr << L"FAIL exe-load " << arguments[index] << L"\n";
            return 1;
        }
        samples[sampleIndex] = {
            code[sampleIndex].data(), code[sampleIndex].size(), entryOffsets[sampleIndex], stableMasks[sampleIndex].data()
        };
    }
    unsigned char pattern[64] = {};
    unsigned char mask[64] = {};
    std::size_t patternLength = 0;
    std::size_t exactBytes = 0;
    if (!JiYuAvBuildSharedPattern(samples.data(), samples.size(), pattern, mask,
        sizeof(pattern), &patternLength, &exactBytes)) {
        std::cerr << "FAIL exe-shared-pattern\n";
        return 1;
    }
    for (const JIYU_AV_PATTERN_SAMPLE_VIEW& sample : samples) {
        if (!JiYuAvPatternMatchesBuffer(sample, pattern, mask, patternLength)) {
            std::cerr << "FAIL exe-sample-validation\n";
            return 1;
        }
    }
    std::cout << "PASS exe-shared-pattern samples=" << samples.size()
        << " bytes=" << patternLength
        << " exact=" << exactBytes
        << " wildcards=" << (patternLength - exactBytes) << "\n";
    std::cout << "PATTERN ";
    static constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < patternLength; ++index) {
        if (index != 0) std::cout << ' ';
        if (mask[index] == 0xFFU) {
            std::cout << digits[pattern[index] >> 4] << digits[pattern[index] & 0x0FU];
        }
        else std::cout << "??";
    }
    std::cout << "\nRESULT exe samples passed\n";
    return 0;
}

} // namespace

int wmain(int argumentCount, wchar_t** arguments)
{
    if (argumentCount > 1) {
        if (argumentCount < 3) {
            std::cerr << "usage: JiYuAvPatternTests.exe <version1.exe> <version2.exe> [...]\n";
            return 2;
        }
        return TestExecutableSamples(argumentCount, arguments);
    }
    const unsigned char data[] = {
        0x90U, 0x48U, 0x8BU, 0x51U, 0x2AU, 0xE8U, 0x7FU, 0x00U, 0xCCU
    };
    unsigned long offset = 0UL;
    bool passed = true;

    {
        const unsigned char pattern[] = { 0x48U, 0x8BU, 0x51U, 0x2AU };
        const unsigned char mask[] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
        passed &= Expect(
            FindPattern(data, sizeof(data), pattern, mask, sizeof(pattern), &offset) &&
                offset == 1UL,
            "exact-match-offset");
    }
    {
        const unsigned char pattern[] = { 0x48U, 0x00U, 0x50U, 0x0AU, 0xE8U };
        const unsigned char mask[] = { 0xFFU, 0x00U, 0xF0U, 0x0FU, 0xFFU };
        passed &= Expect(
            FindPattern(data, sizeof(data), pattern, mask, sizeof(pattern), &offset) &&
                offset == 1UL,
            "byte-and-nibble-wildcards");
    }
    {
        const unsigned char pattern[] = { 0x48U, 0x8BU, 0x60U, 0x2AU };
        const unsigned char mask[] = { 0xFFU, 0xFFU, 0xF0U, 0xFFU };
        passed &= Expect(
            !FindPattern(data, sizeof(data), pattern, mask, sizeof(pattern), &offset),
            "nibble-mismatch");
    }
    {
        const unsigned char pattern[] = { 0x00U, 0x8BU, 0x00U, 0x2AU };
        const unsigned char mask[] = { 0x00U, 0xFFU, 0x00U, 0xFFU };
        passed &= Expect(
            FindPattern(data, sizeof(data), pattern, mask, sizeof(pattern), &offset) &&
                offset == 1UL,
            "full-byte-wildcards");
    }
    {
        const unsigned char pattern[] = { 0x90U, 0x48U, 0x8BU, 0x51U, 0x2AU, 0xE8U, 0x7FU, 0x00U, 0xCCU, 0x00U };
        const unsigned char mask[] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU };
        passed &= Expect(
            !FindPattern(data, sizeof(data), pattern, mask, sizeof(pattern), &offset),
            "pattern-longer-than-input");
    }

    {
        std::vector<unsigned char> versionA(128);
        for (std::size_t index = 0; index < versionA.size(); ++index) {
            versionA[index] = static_cast<unsigned char>((index * 37U + 11U) & 0xFFU);
        }
        std::vector<unsigned char> versionB(137, 0xCCU);
        std::copy(versionA.begin(), versionA.end(), versionB.begin() + 9);
        for (std::size_t index : { 31U, 34U, 39U, 55U, 62U, 70U, 76U, 83U }) {
            versionB[index + 9] ^= static_cast<unsigned char>(0x41U + index);
        }

        const JIYU_AV_PATTERN_SAMPLE_VIEW samples[] = {
            { versionA.data(), versionA.size(), 24U },
            { versionB.data(), versionB.size(), 33U }
        };
        unsigned char sharedPattern[64] = {};
        unsigned char sharedMask[64] = {};
        std::size_t sharedLength = 0;
        std::size_t exactBytes = 0;
        const bool built = JiYuAvBuildSharedPattern(
            samples, 2, sharedPattern, sharedMask, sizeof(sharedPattern),
            &sharedLength, &exactBytes);
        passed &= Expect(
            built && sharedLength == 48U && exactBytes >= 16U && exactBytes < sharedLength &&
                JiYuAvPatternMatchesBuffer(samples[0], sharedPattern, sharedMask, sharedLength) &&
                JiYuAvPatternMatchesBuffer(samples[1], sharedPattern, sharedMask, sharedLength),
            "multi-version-shared-pattern");
    }

    {
        std::vector<unsigned char> sample(80);
        for (std::size_t index = 0; index < sample.size(); ++index) {
            sample[index] = static_cast<unsigned char>((index * 19U + 3U) & 0xFFU);
        }
        const JIYU_AV_PATTERN_SAMPLE_VIEW view = { sample.data(), sample.size(), 20U };
        unsigned char sharedPattern[64] = {};
        unsigned char sharedMask[64] = {};
        std::size_t sharedLength = 0;
        std::size_t exactBytes = 0;
        const bool built = JiYuAvBuildSharedPattern(
            &view, 1, sharedPattern, sharedMask, sizeof(sharedPattern),
            &sharedLength, &exactBytes);
        passed &= Expect(
            built && sharedLength == 48U && exactBytes == 48U &&
                JiYuAvPatternMatchesBuffer(view, sharedPattern, sharedMask, sharedLength),
            "single-version-exact-pattern");
    }

    std::cout << (passed ? "RESULT 7/7 passed\n" : "RESULT failed\n");
    return passed ? 0 : 1;
}
