/*
 * Stands in for the game when testing the menu DLL: a D3D12 swap chain on a window that is never
 * shown, one cleared frame per Present, scripted key presses, and back buffer captures written
 * as raw files for tests/test_overlay.py to inspect.
 *
 * harness <overlay.dll> <rgba8|rgb10|fp16> <root dir> <frames> [<frame>:<vk>]... [ch:<frame>:<char code>]... [m:<frame>:<x>:<y>]... [cap:<frame>:<name>]...
 *
 * With AC8HARNESS_REAL=1 the window is shown and brought to the front, and the key presses go
 * through the real keyboard queue (SendInput), so the DLL's own focus and key checks are tested.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const UINT W = 1280, H = 720;

#define CHECK(expr) do { if (FAILED(expr)) { printf("FAILED: %s\n", #expr); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    std::string format_name = argv[2], root = argv[3];
    int frames = atoi(argv[4]);
    DXGI_FORMAT format = format_name == "fp16" ? DXGI_FORMAT_R16G16B16A16_FLOAT : format_name == "rgb10" ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT pixel_bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    struct Press { int frame, vk; bool character; };
    struct Click { int frame, x, y; }; /* the pointer moves there, then the left button goes down and up */
    std::vector<Click> clicks;
    struct Capture { int frame; std::string name; };
    std::vector<Press> presses;
    std::vector<Capture> captures;
    for (int i = 5; i < argc; i++) {
        if (!strncmp(argv[i], "cap:", 4)) {
            const char *colon = strchr(argv[i] + 4, ':');
            captures.push_back({ atoi(argv[i] + 4), colon + 1 });
        } else if (!strncmp(argv[i], "m:", 2)) {
            int frame = 0, x = 0, y = 0;
            sscanf(argv[i] + 2, "%d:%d:%d", &frame, &x, &y);
            clicks.push_back({ frame, x, y });
        } else if (!strncmp(argv[i], "ch:", 3)) presses.push_back({ atoi(argv[i] + 3), atoi(strchr(argv[i] + 3, ':') + 1), true });
        else presses.push_back({ atoi(argv[i]), atoi(strchr(argv[i], ':') + 1), false });
    }

    SetEnvironmentVariableA("AC8TWEAKS_OFFLINE", "1");
    SetEnvironmentVariableA("AC8TWEAKS_ROOT", root.c_str());
    char real_env[8] = "";
    bool real = GetEnvironmentVariableA("AC8HARNESS_REAL", real_env, sizeof real_env) && real_env[0] == '1';
    if (!real) SetEnvironmentVariableA("AC8OVERLAY_TEST", "1");

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AC8OverlayHarness";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"harness", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    IDXGIFactory2 *factory;
    IDXGISwapChain1 *sc1;
    IDXGISwapChain3 *sc;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC qd = {};
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W;
    sd.Height = H;
    sd.Format = format;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; /* contents survive Present, so they can be read back */
    CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1));
    CHECK(sc1->QueryInterface(IID_PPV_ARGS(&sc)));

    ID3D12CommandQueue *decoy; /* the DLL must pick the queue the swap chain presents on, not just any queue */
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&decoy)));

    ID3D12DescriptorHeap *rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *buffers[3];
    for (UINT i = 0; i < 3; i++) {
        CHECK(sc->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * rtv_size;
        device->CreateRenderTargetView(buffers[i], nullptr, h);
    }
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)));
    list->Close();
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;
    auto flush = [&]() {
        queue->Signal(fence, ++fence_value);
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, INFINITE);
    };

    UINT64 row_pitch = (UINT64)W * pixel_bytes;
    ID3D12Resource *readback;
    D3D12_HEAP_PROPERTIES rb_heap = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rb = {};
    rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = row_pitch * H;
    rb.Height = 1;
    rb.DepthOrArraySize = 1;
    rb.MipLevels = 1;
    rb.SampleDesc.Count = 1;
    rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { printf("FAILED: LoadLibrary %lu\n", GetLastError()); return 1; }
    auto test_hook = (void (*)(int, int))GetProcAddress(dll, "ac8overlay_test_key");
    if (!test_hook) { printf("FAILED: no test hook\n"); return 1; }
    auto test_key = [&](int vk, int down) {
        if (!real) { /* the polled key state for F10, and the window message ImGui reads */
            test_hook(vk, down);
            SendMessageW(hwnd, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, 0);
            return;
        }
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof in);
    };
    if (real) {
        ShowWindow(hwnd, SW_SHOW);
        keybd_event(VK_MENU, 0, 0, 0); /* a key press of our own lets this process take the foreground */
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(hwnd);
        MSG msg;
        for (int i = 0; i < 30; i++) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(10);
        }
        printf("in front: %s\n", GetForegroundWindow() == hwnd ? "yes" : "no");
    }
    Sleep(300); /* the DLL patches the vtable on its own thread */

    auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    };
    const float sky[4] = { 0.10f, 0.20f, 0.40f, 1.0f };
    for (int f = 0; f < frames; f++) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        for (const Click &c : clicks) {
            if (real) { /* the real pointer and the real button, through the system's input queue */
                INPUT in = {};
                in.type = INPUT_MOUSE;
                if (c.frame == f) {
                    POINT at = { c.x, c.y };
                    ClientToScreen(hwnd, &at);
                    SetCursorPos(at.x, at.y);
                }
                if (c.frame + 3 == f || c.frame + 6 == f) {
                    in.mi.dwFlags = c.frame + 3 == f ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    SendInput(1, &in, sizeof in);
                }
                continue;
            }
            LPARAM where = MAKELPARAM(c.x, c.y);
            /* The window is hidden, so Windows reports the pointer gone right after each move;
               every button message therefore comes with a move of its own. */
            if (c.frame == f) {
                SendMessageW(hwnd, WM_MOUSEMOVE, 0, where);
                SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, where);
            }
            if (c.frame + 2 == f) {
                SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, where);
                SendMessageW(hwnd, WM_LBUTTONUP, 0, where);
            }
        }
        for (const Press &p : presses) {
            if (p.character) {
                if (p.frame == f) SendMessageW(hwnd, WM_CHAR, (WPARAM)p.vk, 0);
                continue;
            }
            if (p.frame == f) test_key(p.vk, 1);
            if (p.frame + 2 == f) test_key(p.vk, 0);
        }
        UINT index = sc->GetCurrentBackBufferIndex();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += index * rtv_size;
        allocator->Reset();
        list->Reset(allocator, nullptr);
        barrier(buffers[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(rtv, sky, 0, nullptr);
        barrier(buffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->Close();
        ID3D12CommandList *lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        HRESULT hr = sc->Present(0, 0);
        if (FAILED(hr)) { printf("FAILED: Present 0x%08lx at frame %d\n", (unsigned long)hr, f); return 1; }
        flush();
        for (const Capture &c : captures) {
            if (c.frame != f) continue;
            allocator->Reset();
            list->Reset(allocator, nullptr);
            barrier(buffers[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
            src.pResource = buffers[index];
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { format, W, H, 1, (UINT)row_pitch };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(buffers[index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            list->Close();
            queue->ExecuteCommandLists(1, lists);
            flush();
            void *data;
            CHECK(readback->Map(0, nullptr, &data));
            FILE *out = fopen((root + "\\" + c.name + ".raw").c_str(), "wb");
            fwrite(data, 1, (size_t)(row_pitch * H), out);
            fclose(out);
            readback->Unmap(0, nullptr);
        }
        Sleep(15);
    }
    printf("done %ux%u %s\n", W, H, format_name.c_str());
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0); /* the DLL is pinned and its worker never ends */
    return 0;
}
