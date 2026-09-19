#include <windows.h>
#include <vfw.h>
#include <vector>
#include <string>
#include <stdio.h>

#pragma comment(lib, "vfw32.lib")

static const int W = 640;
static const int H = 360;
static const int FPS = 12;
static const int FRAMES = 36;

struct Frame
{
    std::vector<BYTE> pixels;
    Frame() : pixels(W * H * 3, 0) {}
    BYTE* Pixel(int x, int y) { return &pixels[(y * W + x) * 3]; }
    void Fill(int x0, int y0, int x1, int y1, BYTE r, BYTE g, BYTE b)
    {
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W; if (y1 > H) y1 = H;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            BYTE* p = Pixel(x, y); p[0] = b; p[1] = g; p[2] = r;
        }
    }
};

typedef void (*DrawFunction)(Frame&, int);

static void Bars(Frame& frame, int frameNumber)
{
    const BYTE colors[8][3] = {
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255}, {0,0,255}, {255,0,0}, {0,0,0}
    };
    for (int i = 0; i < 8; ++i) frame.Fill(i * W / 8, 0, (i + 1) * W / 8, 250, colors[i][0], colors[i][1], colors[i][2]);
    frame.Fill(0, 250, W, H, 28, 28, 28);
    int x = (frameNumber * 20) % W;
    frame.Fill(x - 3, 0, x + 4, H, 255, 255, 255);
}

static void Moving(Frame& frame, int frameNumber)
{
    frame.Fill(0, 0, W, H, 14, 26, 54);
    int offset = (frameNumber * 18) % 96;
    for (int x = -96 + offset; x < W + 96; x += 96) frame.Fill(x, 0, x + 46, H, 32, 180, 220);
    frame.Fill(10, 10, W - 10, 16, 250, 220, 80);
    frame.Fill(10, H - 16, W - 10, H - 10, 250, 220, 80);
    frame.Fill(10, 10, 16, H - 10, 250, 220, 80);
    frame.Fill(W - 16, 10, W - 10, H - 10, 250, 220, 80);
}

static void Checker(Frame& frame, int frameNumber)
{
    frame.Fill(0, 0, W, H, 0, 0, 0);
    const int size = 45;
    int shift = frameNumber & 1;
    for (int y = 0; y < H; y += size) for (int x = 0; x < W; x += size)
        if (((x / size) + (y / size) + shift) & 1) frame.Fill(x, y, x + size, y + size, 244, 86, 86);
    frame.Fill(145, 132, 495, 228, 30, 30, 30);
}

static bool WriteAvi(const std::wstring& path, DrawFunction draw)
{
    AVIFileInit();
    PAVIFILE file = nullptr;
    HRESULT hr = AVIFileOpenW(&file, path.c_str(), OF_WRITE | OF_CREATE, nullptr);
    if (FAILED(hr)) { AVIFileExit(); return false; }

    AVISTREAMINFO streamInfo = {};
    streamInfo.fccType = streamtypeVIDEO;
    streamInfo.fccHandler = mmioFOURCC('D', 'I', 'B', ' ');
    streamInfo.dwScale = 1;
    streamInfo.dwRate = FPS;
    streamInfo.dwLength = FRAMES;
    streamInfo.dwSuggestedBufferSize = W * H * 3;
    streamInfo.rcFrame.right = W;
    streamInfo.rcFrame.bottom = H;
    PAVISTREAM stream = nullptr;
    hr = AVIFileCreateStream(file, &stream, &streamInfo);
    if (SUCCEEDED(hr)) {
        BITMAPINFOHEADER format = {};
        format.biSize = sizeof(format);
        format.biWidth = W;
        format.biHeight = H;
        format.biPlanes = 1;
        format.biBitCount = 24;
        format.biCompression = BI_RGB;
        format.biSizeImage = W * H * 3;
        hr = AVIStreamSetFormat(stream, 0, &format, sizeof(format));
    }
    for (int frameNumber = 0; SUCCEEDED(hr) && frameNumber < FRAMES; ++frameNumber) {
        Frame frame;
        draw(frame, frameNumber);
        std::vector<BYTE> bottomUp(frame.pixels.size());
        const int rowBytes = W * 3;
        for (int y = 0; y < H; ++y)
            memcpy(&bottomUp[y * rowBytes], &frame.pixels[(H - 1 - y) * rowBytes], rowBytes);
        hr = AVIStreamWrite(stream, frameNumber, 1, bottomUp.data(), static_cast<LONG>(bottomUp.size()), AVIIF_KEYFRAME, nullptr, nullptr);
    }
    if (stream) AVIStreamRelease(stream);
    AVIFileRelease(file);
    AVIFileExit();
    return SUCCEEDED(hr);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 2;
    std::wstring dir = argv[1];
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    bool ok = WriteAvi(dir + L"screen-video-bars.avi", Bars);
    ok = WriteAvi(dir + L"screen-video-moving.avi", Moving) && ok;
    ok = WriteAvi(dir + L"screen-video-checker.avi", Checker) && ok;
    wprintf(L"AVI generation %s\n", ok ? L"PASS" : L"FAIL");
    return ok ? 0 : 1;
}
