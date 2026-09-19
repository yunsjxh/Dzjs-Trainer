#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <stdio.h>

static void ReleaseUnknown(IUnknown* value)
{
    if (value) value->Release();
}

static int ValidateVideo(const wchar_t* path)
{
    IMFAttributes* attributes = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* outputType = nullptr;
    IMFMediaType* currentType = nullptr;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path, attributes, &reader);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outputType);
    if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType);
    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType);
    if (SUCCEEDED(hr)) hr = MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &width, &height);
    IMFSample* sample = nullptr;
    DWORD actualStream = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    int samples = 0;
    if (SUCCEEDED(hr)) {
        for (;;) {
            sample = nullptr;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actualStream, &flags, &timestamp, &sample);
            ReleaseUnknown(sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (sample) ++samples;
        }
    }
    wprintf(L"%s: hr=0x%08X size=%ux%u flags=0x%08X samples=%d\n", path, static_cast<unsigned>(hr), width, height, flags, samples);
    ReleaseUnknown(currentType);
    ReleaseUnknown(outputType);
    ReleaseUnknown(reader);
    ReleaseUnknown(attributes);
    return SUCCEEDED(hr) && width > 0 && height > 0 && samples > 0 ? 0 : 1;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 2;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) return 3;
    int result = 0;
    for (int i = 1; i < argc; ++i) if (ValidateVideo(argv[i]) != 0) result = 1;
    MFShutdown();
    return result;
}
