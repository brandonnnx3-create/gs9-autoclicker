// ============================================================================
// Autoclicker en C++ (Win32 nativo)
// ----------------------------------------------------------------------------
// Ventajas sobre la versión en Python: al ser código compilado y no interpretado,
// el hook de mouse procesa cada evento en microsegundos, así que no debería
// notarse NINGÚN cambio de sensibilidad en juegos.
//
// Funciones:
// - Tecla configurable para armar/desarmar el sistema (click en "Cambiar tecla").
// - F9 = tecla de emergencia FIJA, siempre desarma todo al instante.
// - Mientras está armado, clickea solo mientras mantenés el click IZQUIERDO.
// - Slider de CPS con decimales (hasta 3 decimales, ej: 17.654).
// - Shift + click en el slider = escribir el número a mano.
// - Contador de clicks generados, visible en la ventana.
//
// Compilación: ver instrucciones que te paso aparte (Visual Studio recomendado).
// ============================================================================

#include <windows.h>
#include <commctrl.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <string>
#include <cwctype>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------- IDs DE CONTROLES ----------------
#define IDC_BTN_HOTKEY   101
#define IDC_SLIDER_CPS   102
#define ID_TIMER_CONTADOR 1
#define IDC_EDIT_CPS     201
#define IDC_BTN_OK       202
#define IDC_BTN_CANCEL   203

// ---------------- ESTADO GLOBAL ----------------
std::atomic<bool> g_running{ true };
std::atomic<bool> g_sistemaActivo{ false };
std::atomic<bool> g_mouseApretado{ false };
std::atomic<bool> g_esperandoNuevaTecla{ false };
std::atomic<double> g_cps{ 10.0 };
std::atomic<DWORD> g_hotkeyVK{ VK_F6 };
std::atomic<long long> g_contador{ 0 };

const DWORD TECLA_EMERGENCIA = VK_F9;

HHOOK g_hMouseHook = NULL;
HHOOK g_hKeyboardHook = NULL;
std::thread g_hiloClicks;

HWND hMain, hLabelEstado, hLabelHotkey, hLabelEmergencia, hBtnHotkey;
HWND hLabelCps, hSliderCps, hLabelAyuda;
WNDPROC g_oldSliderProc = nullptr;

// ---------------- UTILIDADES ----------------
std::wstring NombreTecla(DWORD vk) {
    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lParamKey = (LONG)(scanCode << 16);
    wchar_t buffer[64] = { 0 };
    if (GetKeyNameTextW(lParamKey, buffer, 64)) {
        return std::wstring(buffer);
    }
    return L"Desconocida";
}

void ActualizarLabelEstado() {
    if (g_sistemaActivo)
        SetWindowTextW(hLabelEstado, L"Estado: ACTIVADO (manten\u00e9 click izq. para clickear)");
    else
        SetWindowTextW(hLabelEstado, L"Estado: PAUSADO");
}

void ActualizarLabelHotkey() {
    std::wstring texto = L"Tecla actual: " + NombreTecla(g_hotkeyVK.load());
    SetWindowTextW(hLabelHotkey, texto.c_str());
}

void ActualizarLabelCps(double valor) {
    std::wostringstream ss;
    ss << L"CPS: " << std::fixed << std::setprecision(3) << valor;
    SetWindowTextW(hLabelCps, ss.str().c_str());
}

// ---------------- ENVÍO DEL CLICK (SendInput) ----------------
void EnviarClick() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
    // SendInput marca automáticamente estos eventos como "inyectados",
    // así el hook de mouse los distingue solo de los tuyos reales.
}

// ---------------- HILO QUE HACE LOS CLICKS ----------------

// Chequeo extra de seguridad: solo permitimos clickear si la ventana en
// primer plano pertenece a Minecraft (javaw.exe / java.exe). Así, si alt-tabeás
// a Discord, el navegador, o el escritorio con el sistema armado, no clickea ahí.
bool VentanaActivaEsMinecraft() {
    HWND hFront = GetForegroundWindow();
    if (!hFront) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hFront, &pid);
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t ruta[MAX_PATH];
    DWORD tam = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProc, 0, ruta, &tam);
    CloseHandle(hProc);
    if (!ok) return false;

    std::wstring rutaStr(ruta);
    size_t pos = rutaStr.find_last_of(L"\\/");
    std::wstring nombre = (pos == std::wstring::npos) ? rutaStr : rutaStr.substr(pos + 1);
    for (auto& c : nombre) c = towlower(c);

    return (nombre == L"javaw.exe" || nombre == L"java.exe");
}

// La mayoría de los juegos en primera persona "confiscan" el mouse (lo confinan
// a la ventana) mientras estás jugando activamente, y lo liberan al abrir
// cualquier menú. Es una señal más confiable que las teclas, porque no importa
// CÓMO volviste a jugar (teclado o mouse).
bool RatonEstaCapturado() {
    RECT clip;
    if (!GetClipCursor(&clip)) return false;

    RECT pantalla;
    pantalla.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    pantalla.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    pantalla.right = pantalla.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    pantalla.bottom = pantalla.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Si el área permitida para el mouse es MENOR que toda la pantalla, algo
    // (probablemente el juego) lo está reteniendo para el modo de juego.
    return (clip.left > pantalla.left || clip.top > pantalla.top ||
            clip.right < pantalla.right || clip.bottom < pantalla.bottom);
}

bool PermiteClickPorMenu() {
    if (!VentanaActivaEsMinecraft()) return false;
    return RatonEstaCapturado();
}

void HiloClicks() {
    while (g_running) {
        if (g_sistemaActivo && g_mouseApretado && g_cps.load() > 0.0 && PermiteClickPorMenu()) {
            EnviarClick();
            g_contador++;
            double intervalo = 1.0 / g_cps.load();
            std::this_thread::sleep_for(std::chrono::duration<double>(intervalo));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

// ---------------- HOOK DE MOUSE ----------------
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP) {
            MSLLHOOKSTRUCT* info = (MSLLHOOKSTRUCT*)lParam;
            if (!(info->flags & LLMHF_INJECTED)) {
                g_mouseApretado = (wParam == WM_LBUTTONDOWN);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------- HOOK DE TECLADO ----------------
LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* info = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vk = info->vkCode;

        if (vk == TECLA_EMERGENCIA) {
            g_sistemaActivo = false;
            ActualizarLabelEstado();
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        if (g_esperandoNuevaTecla) {
            g_hotkeyVK = vk;
            g_esperandoNuevaTecla = false;
            ActualizarLabelHotkey();
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        if (vk == g_hotkeyVK.load()) {
            static ULONGLONG ultimoToggle = 0;
            ULONGLONG ahora = GetTickCount64();
            if (ahora - ultimoToggle >= 300) {  // debounce, 300ms
                g_sistemaActivo = !g_sistemaActivo;
                ultimoToggle = ahora;
                ActualizarLabelEstado();
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------- VENTANA EMERGENTE PARA ESCRIBIR EL CPS A MANO ----------------
double g_valorPopup = 10.0;
bool g_popupOk = false;

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit;
    switch (msg) {
    case WM_CREATE: {
        wchar_t inicial[32];
        swprintf(inicial, 32, L"%.3f", g_valorPopup);
        hEdit = CreateWindowExW(0, L"EDIT", inicial,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 10, 200, 24, hwnd, (HMENU)IDC_EDIT_CPS, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE, 40, 45, 60, 25, hwnd, (HMENU)IDC_BTN_OK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancelar",
            WS_CHILD | WS_VISIBLE, 110, 45, 80, 25, hwnd, (HMENU)IDC_BTN_CANCEL, GetModuleHandleW(NULL), NULL);
        SetFocus(hEdit);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_OK) {
            wchar_t buf[32];
            GetWindowTextW(hEdit, buf, 32);
            double v = wcstod(buf, NULL);
            if (v < 0.001) v = 0.001;
            if (v > 50.0) v = 50.0;
            g_valorPopup = v;
            g_popupOk = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wParam) == IDC_BTN_CANCEL) {
            g_popupOk = false;
            DestroyWindow(hwnd);
        }
        break;
    case WM_CLOSE:
        g_popupOk = false;
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Abre una ventanita modal simple para escribir el CPS a mano.
// Devuelve true si el usuario confirmó con OK.
bool PedirCpsManual(HWND parent, double actual, double* resultado) {
    static bool claseRegistrada = false;
    const wchar_t* claseName = L"PopupCpsClass";

    if (!claseRegistrada) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = claseName;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        claseRegistrada = true;
    }

    g_valorPopup = actual;
    g_popupOk = false;
    EnableWindow(parent, FALSE);

    HWND hPopup = CreateWindowExW(WS_EX_DLGMODALFRAME, claseName, L"Configurar CPS",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 230, 120, parent, NULL, GetModuleHandleW(NULL), NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsWindow(hPopup)) break;
        if (!IsDialogMessageW(hPopup, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (g_popupOk) {
        *resultado = g_valorPopup;
        return true;
    }
    return false;
}

// ---------------- SUBCLASE DEL SLIDER (para detectar Shift+Click y click-directo) ----------------
bool g_arrastrandoSlider = false;

int PosDesdeX(HWND hwnd, LONG x) {
    RECT canal;
    SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&canal);
    double proporcion = (double)(x - canal.left) / (double)(canal.right - canal.left);
    if (proporcion < 0) proporcion = 0;
    if (proporcion > 1) proporcion = 1;
    return (int)(1 + proporcion * (50000 - 1));
}

LRESULT CALLBACK SliderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDOWN) {
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            double resultado;
            if (PedirCpsManual(hMain, g_cps.load(), &resultado)) {
                g_cps = resultado;
                int pos = (int)(resultado * 1000);
                SendMessageW(hSliderCps, TBM_SETPOS, TRUE, pos);
                ActualizarLabelCps(resultado);
            }
            return 0;
        } else {
            // Click directo en cualquier parte de la barra: saltamos ahí mismo,
            // en vez del incremento chiquito que hace el control por defecto.
            int nuevaPos = PosDesdeX(hwnd, LOWORD(lParam));
            SendMessageW(hwnd, TBM_SETPOS, TRUE, nuevaPos);
            g_cps = nuevaPos / 1000.0;
            ActualizarLabelCps(g_cps.load());
            g_arrastrandoSlider = true;
            SetCapture(hwnd);
            return 0;
        }
    }
    if (msg == WM_MOUSEMOVE && g_arrastrandoSlider) {
        int nuevaPos = PosDesdeX(hwnd, LOWORD(lParam));
        SendMessageW(hwnd, TBM_SETPOS, TRUE, nuevaPos);
        g_cps = nuevaPos / 1000.0;
        ActualizarLabelCps(g_cps.load());
        return 0;
    }
    if (msg == WM_LBUTTONUP && g_arrastrandoSlider) {
        g_arrastrandoSlider = false;
        ReleaseCapture();
        return 0;
    }
    return CallWindowProcW(g_oldSliderProc, hwnd, msg, wParam, lParam);
}

// ---------------- VENTANA PRINCIPAL ----------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hLabelEstado = CreateWindowExW(0, L"STATIC", L"Estado: PAUSADO",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 10, 260, 20, hwnd, NULL, NULL, NULL);

        hLabelHotkey = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 35, 260, 20, hwnd, NULL, NULL, NULL);

        hLabelEmergencia = CreateWindowExW(0, L"STATIC", L"Emergencia (siempre desarma): F9",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 58, 260, 18, hwnd, NULL, NULL, NULL);

        hBtnHotkey = CreateWindowExW(0, L"BUTTON", L"Cambiar tecla",
            WS_CHILD | WS_VISIBLE, 90, 82, 100, 26, hwnd, (HMENU)IDC_BTN_HOTKEY, NULL, NULL);

        hLabelCps = CreateWindowExW(0, L"STATIC", L"CPS: 10.000",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 118, 260, 20, hwnd, NULL, NULL, NULL);

        hSliderCps = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ, 10, 142, 260, 30, hwnd, (HMENU)IDC_SLIDER_CPS, NULL, NULL);
        SendMessageW(hSliderCps, TBM_SETRANGE, TRUE, MAKELPARAM(1, 50000));
        SendMessageW(hSliderCps, TBM_SETPOS, TRUE, 10000);

        // Subclase del slider para detectar shift+click
        g_oldSliderProc = (WNDPROC)SetWindowLongPtrW(hSliderCps, GWLP_WNDPROC, (LONG_PTR)SliderSubclassProc);

        hLabelAyuda = CreateWindowExW(0, L"STATIC", L"Shift + click en el slider para escribir el n\u00famero a mano",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 175, 260, 32, hwnd, NULL, NULL, NULL);

        ActualizarLabelHotkey();

        g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(NULL), 0);
        g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(NULL), 0);

        g_hiloClicks = std::thread(HiloClicks);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_HOTKEY) {
            g_esperandoNuevaTecla = true;
            SetWindowTextW(hLabelHotkey, L"Presion\u00e1 una tecla...");
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lParam == hSliderCps) {
            int pos = (int)SendMessageW(hSliderCps, TBM_GETPOS, 0, 0);
            double valor = pos / 1000.0;
            g_cps = valor;
            ActualizarLabelCps(valor);
        }
        break;
    case WM_DESTROY:
        g_running = false;
        if (g_hiloClicks.joinable()) g_hiloClicks.join();
        if (g_hMouseHook) UnhookWindowsHookEx(g_hMouseHook);
        if (g_hKeyboardHook) UnhookWindowsHookEx(g_hKeyboardHook);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------- PUNTO DE ENTRADA ----------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t* claseName = L"AutoclickerMainClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = claseName;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    hMain = CreateWindowExW(0, claseName, L"Autoclicker - C++",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 260,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hMain, nCmdShow);
    UpdateWindow(hMain);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
