import re

with open('main.cpp', 'r') as f:
    content = f.read()

render_match = re.search(r'(static void RenderUI\(\) \{.*?\n\})', content, re.DOTALL)

with open('src/ui/Menu.h', 'w') as f:
    f.write('''#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

class Menu {
public:
    static void Init(HWND hwnd);
    static void Render();
    static void Shutdown();
    static void Toggle();
    static bool IsOpen();
private:
    static bool s_isOpen;
};
''')

with open('src/ui/Menu.cpp', 'w') as f:
    f.write('#include "Menu.h"\n')
    f.write('#include "../core/CaptureDXGI.h"\n')
    f.write('#include "../config/Config.h"\n')
    f.write('#include "../core/Scanner.h"\n')
    f.write('#include "imgui.h"\n')
    f.write('#include "imgui_impl_win32.h"\n')
    f.write('#include "imgui_impl_dx11.h"\n\n')
    f.write('bool Menu::s_isOpen = true;\n\n')
    f.write('void Menu::Init(HWND hwnd) {\n')
    f.write('    IMGUI_CHECKVERSION();\n')
    f.write('    ImGui::CreateContext();\n')
    f.write('    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;\n')
    f.write('    ImGui::StyleColorsDark();\n')
    f.write('    ImGui_ImplWin32_Init(hwnd);\n')
    f.write('    ImGui_ImplDX11_Init(CaptureDXGI::GetDevice(), CaptureDXGI::GetContext());\n')
    f.write('}\n\n')
    f.write('void Menu::Shutdown() {\n')
    f.write('    ImGui_ImplDX11_Shutdown();\n')
    f.write('    ImGui_ImplWin32_Shutdown();\n')
    f.write('    ImGui::DestroyContext();\n')
    f.write('}\n\n')
    f.write('void Menu::Toggle() { s_isOpen = !s_isOpen; }\n')
    f.write('bool Menu::IsOpen() { return s_isOpen; }\n\n')

    s = render_match.group(1).replace('static void RenderUI()', 'void Menu::Render()')
    s = s.replace('g_menuOpen', 's_isOpen')
    s = s.replace('g_hasAVX2', 'Scanner::HasAVX2()')
    s = s.replace('g_context->ClearRenderTargetView(g_rtv.Get(),', 'auto rtv = CaptureDXGI::GetRTV();\n    CaptureDXGI::GetContext()->ClearRenderTargetView(rtv,')
    s = s.replace('g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);', 'CaptureDXGI::GetContext()->OMSetRenderTargets(1, &rtv, nullptr);')
    s = s.replace('g_swapChain->Present(0, 0);', 'CaptureDXGI::Present();')
    
    f.write(s + '\n')
