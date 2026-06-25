#include "platform/windows/D3D11Application.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "resources/resource.h"
#include "ui/MainMenu.hpp"
#include "ui/Theme.hpp"

#include <cstdint>
#include <filesystem>
#include <shellscalingapi.h>
#include <vector>
#include <wincodec.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {
constexpr wchar_t kWindowClassName[] = L"STOWindowClass";
constexpr wchar_t kWindowTitle[] = L"STO - Sistema de Transcricao de Oitivas";
D3D11Application* g_application = nullptr;

std::filesystem::path executable_directory() {
    wchar_t executable_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
    return std::filesystem::path(executable_path).parent_path();
}
}

D3D11Application::~D3D11Application() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyContext();
    }

    destroy_device();

    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
    if (window_class_.hInstance != nullptr) {
        UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);
    }
    g_application = nullptr;
}

bool D3D11Application::initialize(HINSTANCE instance, int show_command) {
    g_application = this;
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    const auto load_icon = [&](int cx, int cy) -> HICON {
        return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_STO_APP), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    };
    const int cx_big = GetSystemMetrics(SM_CXICON);
    const int cy_big = GetSystemMetrics(SM_CYICON);
    const int cx_sm  = GetSystemMetrics(SM_CXSMICON);
    const int cy_sm  = GetSystemMetrics(SM_CYSMICON);

    window_class_ = {
        sizeof(WNDCLASSEXW),
        CS_CLASSDC,
        window_procedure,
        0L,
        0L,
        instance,
        load_icon(cx_big, cy_big),
        nullptr,
        nullptr,
        nullptr,
        kWindowClassName,
        load_icon(cx_sm, cy_sm),
    };

    RegisterClassExW(&window_class_);
    window_ = CreateWindowW(
        window_class_.lpszClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        1440,
        900,
        nullptr,
        nullptr,
        window_class_.hInstance,
        nullptr
    );

    if (window_ == nullptr || !create_device()) {
        return false;
    }
    SendMessageW(window_, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(load_icon(cx_big, cy_big)));
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(load_icon(cx_sm,  cy_sm)));

    ShowWindow(window_, show_command);
    UpdateWindow(window_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    sto::ui::initialize_fonts(io, executable_directory() / "assets");
    sto::ui::apply_theme();
    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX11_Init(device_, device_context_);
    load_brand_logo();
    return true;
}

int D3D11Application::run() {
    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running = false;
            }
        }

        if (!running) {
            break;
        }

        if (swap_chain_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        swap_chain_occluded_ = false;

        render_frame();
    }

    return 0;
}

bool D3D11Application::create_device() {
    DXGI_SWAP_CHAIN_DESC swap_chain_description{};
    swap_chain_description.BufferCount = 2;
    swap_chain_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_description.OutputWindow = window_;
    swap_chain_description.SampleDesc.Count = 1;
    swap_chain_description.Windowed = TRUE;
    swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL feature_level{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        feature_levels,
        2,
        D3D11_SDK_VERSION,
        &swap_chain_description,
        &swap_chain_,
        &device_,
        &feature_level,
        &device_context_
    );

    if (FAILED(result)) {
        return false;
    }

    create_render_target();
    return true;
}

void D3D11Application::create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_);
    back_buffer->Release();
}

void D3D11Application::destroy_render_target() {
    if (render_target_view_ != nullptr) {
        render_target_view_->Release();
        render_target_view_ = nullptr;
    }
}

void D3D11Application::destroy_device() {
    destroy_render_target();
    if (brand_logo_texture_ != nullptr) {
        brand_logo_texture_->Release();
        brand_logo_texture_ = nullptr;
    }
    if (doc_logo_texture_ != nullptr) {
        doc_logo_texture_->Release();
        doc_logo_texture_ = nullptr;
    }
    if (swap_chain_ != nullptr) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
    if (device_context_ != nullptr) {
        device_context_->Release();
        device_context_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

void D3D11Application::load_brand_logo() {
    if (brand_logo_texture_ != nullptr || device_ == nullptr) {
        return;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        return;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    const auto logo_path = executable_directory() / "assets" / "branding" / "logo.png";
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(logo_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) {
        result = converter->GetSize(&width, &height);
    }

    std::vector<unsigned char> pixels;
    if (SUCCEEDED(result) && width > 0 && height > 0) {
        pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
        result = converter->CopyPixels(nullptr, width * 4U, static_cast<UINT>(pixels.size()), pixels.data());
    }

    if (SUCCEEDED(result) && !pixels.empty()) {
        D3D11_TEXTURE2D_DESC texture_description{};
        texture_description.Width = width;
        texture_description.Height = height;
        texture_description.MipLevels = 1;
        texture_description.ArraySize = 1;
        texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_description.SampleDesc.Count = 1;
        texture_description.Usage = D3D11_USAGE_DEFAULT;
        texture_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subresource{};
        subresource.pSysMem = pixels.data();
        subresource.SysMemPitch = width * 4U;

        ID3D11Texture2D* texture = nullptr;
        result = device_->CreateTexture2D(&texture_description, &subresource, &texture);
        if (SUCCEEDED(result)) {
            result = device_->CreateShaderResourceView(texture, nullptr, &brand_logo_texture_);
            texture->Release();
        }
        if (SUCCEEDED(result)) {
            sto::ui::set_brand_logo(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(brand_logo_texture_)), static_cast<int>(width), static_cast<int>(height));
        }
    }

    if (converter != nullptr) converter->Release();
    if (frame != nullptr) frame->Release();
    if (decoder != nullptr) decoder->Release();
    if (factory != nullptr) factory->Release();
}

void D3D11Application::load_doc_logo(const std::filesystem::path& path) {
    if (device_ == nullptr) return;

    IWICImagingFactory* factory  = nullptr;
    IWICBitmapDecoder*  decoder  = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv    = nullptr;

    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(result)) result = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);

    UINT w = 0, h = 0;
    if (SUCCEEDED(result)) result = conv->GetSize(&w, &h);

    std::vector<unsigned char> pixels;
    if (SUCCEEDED(result) && w > 0 && h > 0) {
        pixels.resize(static_cast<std::size_t>(w) * h * 4U);
        result = conv->CopyPixels(nullptr, w * 4U, static_cast<UINT>(pixels.size()), pixels.data());
    }

    if (SUCCEEDED(result) && !pixels.empty()) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{}; sub.pSysMem = pixels.data(); sub.SysMemPitch = w * 4U;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(device_->CreateTexture2D(&desc, &sub, &tex))) {
            if (doc_logo_texture_ != nullptr) { doc_logo_texture_->Release(); doc_logo_texture_ = nullptr; }
            if (SUCCEEDED(device_->CreateShaderResourceView(tex, nullptr, &doc_logo_texture_))) {
                sto::ui::set_doc_logo(
                    static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(doc_logo_texture_)),
                    static_cast<int>(w), static_cast<int>(h));
            }
            tex->Release();
        }
    }

    if (conv)    conv->Release();
    if (frame)   frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
}

void D3D11Application::render_frame() {
    // Load pending document logo requested by the settings UI
    const auto pending_logo = sto::ui::pending_doc_logo_path();
    if (!pending_logo.empty()) {
        sto::ui::clear_pending_doc_logo();
        load_doc_logo(pending_logo);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    sto::ui::render_main_menu();

    ImGui::Render();
    constexpr float clear_color[4] = {0.035F, 0.039F, 0.043F, 1.0F};
    device_context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
    device_context_->ClearRenderTargetView(render_target_view_, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const HRESULT result = swap_chain_->Present(1, 0);
    swap_chain_occluded_ = result == DXGI_STATUS_OCCLUDED;
}

LRESULT WINAPI D3D11Application::window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return true;
    }

    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* sizing = reinterpret_cast<MINMAXINFO*>(lparam);
        sizing->ptMinTrackSize = {900, 640};
        return 0;
    }
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED || g_application == nullptr || g_application->device_ == nullptr) {
            return 0;
        }
        g_application->destroy_render_target();
        g_application->swap_chain_->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
        g_application->create_render_target();
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}
