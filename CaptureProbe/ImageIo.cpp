#include "ImageIo.h"
#include <wincodec.h>

namespace {

IWICImagingFactory* g_wic = nullptr;
bool g_comHoldsRef = false;

void ReleaseAll(IWICStream* stream, IWICBitmapEncoder* encoder,
	IWICBitmapFrameEncode* frame, IPropertyBag2* props)
{
	if (props) props->Release();
	if (frame) frame->Release();
	if (encoder) encoder->Release();
	if (stream) stream->Release();
}

void ReleaseAll(IWICBitmapDecoder* decoder, IWICBitmapFrameDecode* src,
	IWICFormatConverter* conv)
{
	if (conv) conv->Release();
	if (src) src->Release();
	if (decoder) decoder->Release();
}

} // namespace

bool ImageIoInit()
{
	if (g_wic) return true;

	const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	g_comHoldsRef = SUCCEEDED(comHr);
	if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
		return false;

	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
		CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
		reinterpret_cast<void**>(&g_wic));
	if (FAILED(hr))
	{
		g_wic = nullptr;
		return false;
	}
	return true;
}

void ImageIoShutdown()
{
	if (g_wic)
	{
		g_wic->Release();
		g_wic = nullptr;
	}
	if (g_comHoldsRef)
	{
		CoUninitialize();
		g_comHoldsRef = false;
	}
}

bool ImageSavePng(const wchar_t* path, const Image32& img)
{
	if (!g_wic || !path || !*path || !img.Valid())
		return false;

	IWICStream* stream = nullptr;
	IWICBitmapEncoder* encoder = nullptr;
	IWICBitmapFrameEncode* frame = nullptr;
	IPropertyBag2* props = nullptr;
	bool ok = false;

	HRESULT hr = g_wic->CreateStream(&stream);
	if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
	if (SUCCEEDED(hr)) hr = g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
	if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
	if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
	if (SUCCEEDED(hr)) hr = frame->Initialize(props);
	if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)img.width, (UINT)img.height);

	WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
	if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);

	// PNG 编码器对 32bppBGRA 是原生支持的；万一它换成别的格式，
	// 说明我们的缓冲布局与它要求的不一致，宁可失败也不要写出一张错图。
	// WritePixels 的形参是 BYTE* 而不是 const BYTE*，但它只读，去掉 const 安全。
	if (SUCCEEDED(hr) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
		hr = frame->WritePixels((UINT)img.height, (UINT)img.stride,
			(UINT)((size_t)img.stride * (size_t)img.height),
			const_cast<BYTE*>(img.pixels.data()));
	else if (SUCCEEDED(hr))
		hr = E_FAIL;

	if (SUCCEEDED(hr)) hr = frame->Commit();
	if (SUCCEEDED(hr)) hr = encoder->Commit();
	ok = SUCCEEDED(hr);

	ReleaseAll(stream, encoder, frame, props);
	return ok;
}

bool ImageLoadFile(const wchar_t* path, Image32& out)
{
	out.Clear();
	if (!g_wic || !path || !*path)
		return false;

	IWICBitmapDecoder* decoder = nullptr;
	IWICBitmapFrameDecode* src = nullptr;
	IWICFormatConverter* conv = nullptr;
	bool ok = false;

	HRESULT hr = g_wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnDemand, &decoder);
	if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &src);
	if (SUCCEEDED(hr)) hr = g_wic->CreateFormatConverter(&conv);
	if (SUCCEEDED(hr)) hr = conv->Initialize(src, GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);

	UINT w = 0, h = 0;
	if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
	if (SUCCEEDED(hr) && w > 0 && h > 0)
	{
		out.Alloc((int)w, (int)h);
		hr = conv->CopyPixels(nullptr, (UINT)out.stride,
			(UINT)((size_t)out.stride * (size_t)out.height), out.pixels.data());
		if (SUCCEEDED(hr)) ok = true;
	}

	ReleaseAll(decoder, src, conv);
	if (!ok) out.Clear();
	return ok;
}
