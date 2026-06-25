#pragma once

#include <d3d11.h>
#include <filesystem>
#include <windows.h>

class D3D11Application {
public:
    D3D11Application() = default;
    ~D3D11Application();

    D3D11Application(const D3D11Application&) = delete;
    D3D11Application& operator=(const D3D11Application&) = delete;

    bool initialize(HINSTANCE instance, int show_command);
    int run();

private:
    static LRESULT WINAPI window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    bool create_device();
    void load_brand_logo();
    void load_doc_logo(const std::filesystem::path& path);
    void create_render_target();
    void destroy_device();
    void destroy_render_target();
    void render_frame();

    HWND window_ = nullptr;
    WNDCLASSEXW window_class_{};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* device_context_ = nullptr;
    ID3D11ShaderResourceView* brand_logo_texture_ = nullptr;
    ID3D11ShaderResourceView* doc_logo_texture_   = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* render_target_view_ = nullptr;
    bool com_initialized_ = false;
    bool swap_chain_occluded_ = false;
};
