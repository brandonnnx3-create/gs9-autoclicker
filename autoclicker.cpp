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
// REGLA DE CUÁNDO CLICKEA (lo importante):
//   1. La ventana en primer plano tiene que ser Minecraft (javaw.exe/java.exe).
//   2. Si el CURSOR ESTÁ OCULTO (estás jugando) -> clickea.
//   3. Si el CURSOR ESTÁ VISIBLE (inventario, menú de pausa, chat) -> NO clickea,
//      SALVO que mantengas SHIFT apretado. Eso permite bajar pociones rápido
//      con shift+click en el inventario, sin spamear el menú de pausa.
//
// Compilación: ver instrucciones que te paso aparte (Visual Studio recomendado).
// ============================================================================

// Los proyectos de Visual Studio definen UNICODE por defecto, pero compilar con
// "cl" a secas no. Sin esto, macros como IDC_ARROW se expanden a la version ANSI
// y no compilan contra LoadCursorW. Definirlo aca hace que el archivo compile
// igual desde el IDE que desde la linea de comandos.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Windows Vista o superior: lo necesita PROCESS_QUERY_LIMITED_INFORMATION.
// Visual Studio ya lo define asi por defecto, pero MinGW/g++ no siempre.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM (maneja coordenadas negativas bien)
#include <commctrl.h>
#include <mmsystem.h>   // timeBeginPeriod
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <string>
#include <cwctype>
#include <cmath>

// user32.lib no se linkea solo desde "cl" a secas (el template de Visual
// Studio lo agrega via "Additional Dependencies", pero eso no existe fuera
// del proyecto). Sin esto, TODAS las funciones de User32 -CreateWindowExW,
// RegisterClassW, GetMessageW, SendInput, los hooks, etc.- quedan sin resolver
// al linkear (LNK2019), aunque el .cpp compile bien.
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
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
std::atomic<long long> g_movimientos{ 0 };      // movimientos REALES del mouse
std::atomic<bool> g_juegoWarpeaCursor{ false }; // ver DetectarWarpDelCursor()
std::atomic<long> g_distCentro{ -1 };           // px del cursor al centro (diagnostico)
std::atomic<long long> g_msDesdeCentro{ -1 };   // ms desde el ultimo paso por el centro
std::atomic<bool> g_senalCursorSirve{ false };  // GetCursorInfo demostro servir aca
std::atomic<long> g_muestrasOculto{ 0 };        // cuantas veces vimos el cursor oculto

const DWORD TECLA_EMERGENCIA = VK_F9;

// Para poder cortar el hilo de clicks al instante al cerrar, sin esperar a que
// termine el sleep (con CPS bajísimos el sleep puede durar minutos).
std::mutex g_mtxSalida;
std::condition_variable g_cvSalida;

HHOOK g_hMouseHook = NULL;
HHOOK g_hKeyboardHook = NULL;
std::thread g_hiloClicks;

HWND hMain, hLabelEstado, hLabelHotkey, hLabelEmergencia, hBtnHotkey;
HWND hLabelCps, hSliderCps, hLabelAyuda, hLabelDeteccion, hLabelContador;
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

// Evita el parpadeo de los STATIC: solo reescribe si el texto cambió.
void SetTextoSiCambio(HWND h, const std::wstring& nuevo) {
    wchar_t actual[256] = { 0 };
    GetWindowTextW(h, actual, 256);
    if (nuevo != actual) SetWindowTextW(h, nuevo.c_str());
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

// ---------------- DETECCIÓN DE CONTEXTO ----------------

// Chequeo de seguridad: solo clickeamos si la ventana en primer plano pertenece
// a Minecraft (javaw.exe / java.exe). Si alt-tabeás a Discord, el navegador, o
// el escritorio con el sistema armado, no clickea ahí.
//
// Cacheado por HWND: abrir el proceso en cada iteración (hasta 50 veces por
// segundo) es caro y además el proceso de una ventana nunca cambia.
bool VentanaActivaEsMinecraft() {
    HWND hFront = GetForegroundWindow();
    if (!hFront) return false;

    static std::atomic<HWND> ultimaVentana{ nullptr };
    static std::atomic<bool> ultimoResultado{ false };
    if (hFront == ultimaVentana.load()) return ultimoResultado.load();

    bool resultado = false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hFront, &pid);
    if (pid != 0) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t ruta[MAX_PATH];
            DWORD tam = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, ruta, &tam)) {
                std::wstring rutaStr(ruta);
                size_t pos = rutaStr.find_last_of(L"\\/");
                std::wstring nombre = (pos == std::wstring::npos) ? rutaStr : rutaStr.substr(pos + 1);
                for (auto& c : nombre) c = towlower(c);
                resultado = (nombre == L"javaw.exe" || nombre == L"java.exe");
            }
            CloseHandle(hProc);
        }
    }

    ultimaVentana = hFront;
    ultimoResultado = resultado;
    return resultado;
}

// SEÑAL PRINCIPAL: ¿el cursor está oculto?
// Cuando estás jugando, Minecraft esconde el puntero. Cuando abrís el
// inventario, el menú de pausa o el chat, lo vuelve a mostrar.
// A diferencia de mirar el teclado, esto no depende de CÓMO saliste del menú
// (con Escape o clickeando "Back to Game"), y a diferencia de GetClipCursor,
// sigue funcionando en pantalla completa.
bool CursorEstaOculto() {
    CURSORINFO ci = { 0 };
    ci.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&ci)) return false;  // ante la duda, asumimos "visible"
    // flags: 0 = oculto, CURSOR_SHOWING = visible, CURSOR_SUPPRESSED = táctil.
    // hCursor NULL también significa que nadie está dibujando puntero.
    return (ci.flags & CURSOR_SHOWING) == 0 || ci.hCursor == NULL;
}

// SEÑAL SECUNDARIA (red de respaldo): los juegos en primera persona confinan el
// mouse a la ventana mientras jugás y lo liberan al abrir un menú.
// Ojo: en pantalla completa con un solo monitor, el área confinada coincide con
// toda la pantalla y esta señal deja de distinguir nada. Por eso es secundaria.
bool RatonEstaConfinado() {
    RECT clip;
    if (!GetClipCursor(&clip)) return false;

    RECT pantalla;
    pantalla.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    pantalla.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    pantalla.right = pantalla.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    pantalla.bottom = pantalla.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    return (clip.left > pantalla.left || clip.top > pantalla.top ||
            clip.right < pantalla.right || clip.bottom < pantalla.bottom);
}

// ¿La ventana en foco ocupa todo su monitor? Importa porque decide cuál de las
// señales es confiable: en modo ventana, el confinamiento del mouse distingue
// perfecto entre jugar y estar en un menú; en pantalla completa no distingue
// nada, porque el área confinada pasa a ser la pantalla entera.
// ¿La ventana en foco cubre TODO el escritorio (todos los monitores juntos)?
// Esta es la pregunta que de verdad importa para el confinamiento del mouse: si
// la ventana no cubre todo, entonces "el mouse no esta confinado" significa que
// nadie lo esta reteniendo, o sea que hay un menu abierto. Solo cuando la
// ventana cubre el escritorio entero las dos cosas se vuelven indistinguibles.
bool VentanaCubreTodoElEscritorio() {
    HWND h = GetForegroundWindow();
    if (!h) return false;

    RECT rv;
    if (!GetWindowRect(h, &rv)) return false;

    RECT esc;
    esc.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    esc.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    esc.right = esc.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    esc.bottom = esc.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    return rv.left <= esc.left && rv.top <= esc.top &&
           rv.right >= esc.right && rv.bottom >= esc.bottom;
}

bool VentanaEnPantallaCompleta() {
    HWND h = GetForegroundWindow();
    if (!h) return false;

    RECT rv;
    if (!GetWindowRect(h, &rv)) return false;

    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    return rv.left <= mi.rcMonitor.left && rv.top <= mi.rcMonitor.top &&
           rv.right >= mi.rcMonitor.right && rv.bottom >= mi.rcMonitor.bottom;
}

// TERCERA SEÑAL (la única que sirve en pantalla completa con un solo monitor):
// mientras jugás, Minecraft no deja que el cursor se mueva: lo vuelve a poner en
// el centro del área de la ventana en cada cuadro. O sea, movés el mouse y la
// posición del cursor NO cambia. En un menú eso es imposible: el cursor se mueve
// libremente con la mano.
//
// Esto no depende ni de que el cursor esté oculto ni de que el mouse esté
// confinado, así que sobrevive a la pantalla completa.
// Distancia en pixeles del cursor al centro del area de la ventana en foco.
// Devuelve -1 si no se pudo calcular.
long DistanciaAlCentro() {
    HWND h = GetForegroundWindow();
    if (!h) return -1;

    RECT rc;
    if (!GetClientRect(h, &rc)) return -1;
    POINT centro = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    if (!ClientToScreen(h, &centro)) return -1;

    POINT p;
    if (!GetCursorPos(&p)) return -1;

    long dx = p.x - centro.x;
    long dy = p.y - centro.y;
    double d = sqrt((double)(dx * dx + dy * dy));
    return (long)(d + 0.5);
}

// Se llama desde el timer de la ventana, muy seguido (cada 15 ms).
//
// La clave NO es que el cursor esté en el centro en este instante: mientras
// movés la mano, el juego lo devuelve al centro una vez por cuadro, así que
// entre cuadro y cuadro está a mitad de camino. La clave es que VUELVA al
// centro una y otra vez. En un menú el cursor queda donde lo dejaste y nunca
// vuelve solo.
//
// Por eso se mide "cuánto hace que no pasa por el centro":
//   - Jugando (te muevas o no): pasa por el centro cada cuadro -> siempre poco.
//   - En un menú, apenas movés el mouse: nunca más pasa -> crece sin freno.
// Se definen mas abajo, pero el muestreador las necesita.
bool EstaJugando();
bool ShiftApretado();

// Snapshot de todas las señales, para el panel. Se guarda uno "viejo" (medio
// segundo atrás) porque el panel solo se puede leer alt-tabeando, y Minecraft
// restaura el cursor JUSTO al perder el foco: congelar el último valor medía el
// instante en que la señal ya había cambiado, no lo que pasaba jugando.
struct Muestra {
    bool valida = false;
    bool oculto = false, clip = false, warp = false, full = false, shift = false;
    bool permite = false;
    long dist = -1;
    long long msCentro = -1;
};
Muestra g_muestraReciente, g_muestraVieja;

// Se llama desde el timer de la ventana cada 15 ms.
void MuestrearSeniales() {
    // --- parámetros de la heurística del cursor secuestrado ---
    const long TOLERANCIA_CENTRO = 10;   // qué tan cerca del centro cuenta como "volvió"
    const long CORTE_INMEDIATO   = 200;  // tan lejos del centro que no puede estar jugando
    const ULONGLONG VENTANA_APAGADO = 120;   // ms sin volver al centro -> menú
    const ULONGLONG EXPIRA_SIN_MOVER = 2000; // ver comentario abajo

    // Cuántas muestras seguidas hay que ver el cursor oculto antes de confiar en
    // GetCursorInfo. Con una sola alcanzaría para que un instante raro desactive
    // para siempre la heurística que sí funciona, así que se pide evidencia.
    const long MUESTRAS_PARA_CONFIAR = 30;   // ~450 ms de juego

    static long long ultimosMovimientos = 0;
    static ULONGLONG ultimoPasoPorCentro = 0;
    static ULONGLONG ultimoMovimiento = 0;
    static ULONGLONG ultimaRotacion = 0;
    static bool confirmado = false;

    long long movs = g_movimientos.load();
    bool huboMovimiento = (movs != ultimosMovimientos);
    ultimosMovimientos = movs;

    if (!VentanaActivaEsMinecraft()) return;   // fuera de Minecraft no tocamos nada

    ULONGLONG ahora = GetTickCount64();
    if (huboMovimiento) ultimoMovimiento = ahora;

    // --- calibración de la señal de cursor oculto ---
    bool oculto = CursorEstaOculto();
    if (oculto) {
        long n = g_muestrasOculto.load() + 1;
        g_muestrasOculto = n;
        if (n >= MUESTRAS_PARA_CONFIAR) g_senalCursorSirve = true;
    }

    // --- heurística del cursor secuestrado ---
    long d = DistanciaAlCentro();
    g_distCentro = d;

    if (d >= 0 && d <= TOLERANCIA_CENTRO) {
        ultimoPasoPorCentro = ahora;
        // Que VUELVA al centro mientras movés la mano es la prueba de que algo lo
        // está devolviendo ahí. Estar quieto en el centro no prueba nada: es
        // exactamente lo que pasa también con el inventario recién abierto.
        if (huboMovimiento) confirmado = true;
    }
    g_msDesdeCentro = ultimoPasoPorCentro ? (long long)(ahora - ultimoPasoPorCentro) : -1;

    if (d > CORTE_INMEDIATO) confirmado = false;
    if (!ultimoPasoPorCentro || (ahora - ultimoPasoPorCentro) > VENTANA_APAGADO) confirmado = false;

    // Punto ciego irreducible de esta heurística: con el mouse totalmente quieto
    // y el cursor en el centro, jugar y tener el inventario abierto son
    // indistinguibles (el juego deja el cursor en el centro en los dos casos).
    // Ante esa duda se elige NO clickear: un click de más en el inventario tira
    // objetos, y para que vuelva a clickear alcanza con mover el mouse.
    // Solo aplica a esta heurística; las otras dos capas no tienen este problema.
    if (ultimoMovimiento && (ahora - ultimoMovimiento) > EXPIRA_SIN_MOVER) confirmado = false;

    g_juegoWarpeaCursor = confirmado;

    // --- snapshot para el panel ---
    Muestra m;
    m.valida = true;
    m.oculto = oculto;
    m.clip = RatonEstaConfinado();
    m.warp = confirmado;
    m.full = VentanaEnPantallaCompleta();
    m.shift = ShiftApretado();
    m.dist = d;
    m.msCentro = g_msDesdeCentro.load();
    m.permite = EstaJugando() || m.shift;
    g_muestraReciente = m;

    if (ahora - ultimaRotacion >= 500) {   // el panel muestra esto, medio segundo atrás
        g_muestraVieja = g_muestraReciente;
        ultimaRotacion = ahora;
    }
}

// Las tres señales no son equivalentes: cada una es exacta en un contexto y
// ciega en otro. En vez de mezclarlas con un OR (que deja que la peor de las
// tres mantenga el click prendido), se usa la mejor disponible en cada caso.
//
//   1. Si en esta máquina GetCursorInfo demostró detectar que el juego oculta el
//      puntero, esa es la respuesta exacta. Se calibra sola. (Con Minecraft no
//      funciona, pero la capa queda por si sirve en otro juego o versión.)
//   2. Mouse confinado a menos que el escritorio = algo lo está reteniendo, y
//      eso solo pasa jugando. Es evidencia positiva y directa.
//   3. Mouse NO confinado, pero la ventana tampoco cubre todo el escritorio:
//      si estuvieras jugando, el juego lo estaría confinando. No lo hace ->
//      hay un menú abierto. Exacto también.
//      (Este es el caso de pantalla completa con varios monitores: la ventana
//      ocupa un monitor, no el escritorio entero, así que sigue siendo exacto.)
//   4. Recién si la ventana cubre TODO el escritorio, "no confinado" y
//      "confinado a todo" son lo mismo y no se puede distinguir: ahí, y solo
//      ahí, se cae a la heurística del cursor secuestrado, con su punto ciego.
bool EstaJugando() {
    if (g_senalCursorSirve.load()) return CursorEstaOculto();
    if (RatonEstaConfinado()) return true;
    if (!VentanaCubreTodoElEscritorio()) return false;
    return g_juegoWarpeaCursor.load();
}

bool ShiftApretado() {
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

// LA REGLA COMPLETA.
bool PermiteClick() {
    if (!VentanaActivaEsMinecraft()) return false;
    if (EstaJugando()) return true;   // cursor oculto -> jugando -> clickea
    return ShiftApretado();           // cursor visible -> solo con Shift
}

// ---------------- HILO QUE HACE LOS CLICKS ----------------

// Duerme como máximo `d`, pero se despierta enseguida si se pidió cerrar.
template <class Dur>
void DormirInterrumpible(Dur d) {
    std::unique_lock<std::mutex> lock(g_mtxSalida);
    g_cvSalida.wait_for(lock, d, [] { return !g_running.load(); });
}

void HiloClicks() {
    using reloj = std::chrono::steady_clock;
    auto proximoClick = reloj::now();
    bool enRafaga = false;

    while (g_running) {
        double cps = g_cps.load();

        // NOTA: acá hubo una "reconciliación" que leía GetAsyncKeyState(VK_LBUTTON)
        // para el caso de que el hook perdiera el evento de soltar. Se sacó: los
        // propios clicks que este programa manda con SendInput pisan ese mismo
        // estado global (el LEFTUP sintético queda registrado como "botón
        // suelto" para GetAsyncKeyState), así que cortaba el autoclicker después
        // del primer click SIEMPRE, no solo en el caso raro que se quería cubrir.
        // El hook de mouse (que sí filtra LLMHF_INJECTED) es la señal correcta.
        if (!(g_sistemaActivo && g_mouseApretado && cps > 0.0 && PermiteClick())) {
            enRafaga = false;
            DormirInterrumpible(std::chrono::milliseconds(5));
            continue;
        }

        auto ahora = reloj::now();
        if (!enRafaga) {          // primer click de la ráfaga: sale ya
            enRafaga = true;
            proximoClick = ahora;
        }

        if (ahora < proximoClick) {
            // Esperamos de a 15ms como mucho, para reaccionar rápido si soltás el
            // botón, movés el slider, o se abre un menú: la condición se vuelve a
            // evaluar recién al despertarse, así que este tope es demora directa.
            auto falta = proximoClick - ahora;
            auto tope = std::chrono::milliseconds(15);
            DormirInterrumpible(falta < tope ? falta : std::chrono::duration_cast<reloj::duration>(tope));
            continue;
        }

        EnviarClick();
        g_contador++;

        // Deadline absoluto en vez de "dormir 1/cps": así el tiempo que tarda
        // SendInput no se va acumulando como atraso click tras click.
        auto intervalo = std::chrono::duration_cast<reloj::duration>(
            std::chrono::duration<double>(1.0 / cps));
        proximoClick += intervalo;
        if (proximoClick < ahora) proximoClick = ahora;  // si nos atrasamos, reanclamos
    }
}

// ---------------- HOOK DE MOUSE ----------------
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* info = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP) {
            if (!(info->flags & LLMHF_INJECTED)) {
                g_mouseApretado = (wParam == WM_LBUTTONDOWN);
            }
        } else if (wParam == WM_MOUSEMOVE) {
            // Solo movimientos de la mano, no los que genere este programa.
            if (!(info->flags & LLMHF_INJECTED)) g_movimientos++;
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
bool g_popupCerrado = false;

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
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == IDC_BTN_OK || id == IDOK) {   // IDOK = tecla Enter
            wchar_t buf[32];
            GetWindowTextW(hEdit, buf, 32);
            double v = wcstod(buf, NULL);
            if (v < 0.001) v = 0.001;
            if (v > 50.0) v = 50.0;
            g_valorPopup = v;
            g_popupOk = true;
            DestroyWindow(hwnd);
        } else if (id == IDC_BTN_CANCEL || id == IDCANCEL) {  // IDCANCEL = Escape
            g_popupOk = false;
            DestroyWindow(hwnd);
        }
        break;
    }
    case WM_CLOSE:
        g_popupOk = false;
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        // OJO: acá NO va PostQuitMessage. El popup corre un loop de mensajes
        // anidado dentro del loop principal, así que un WM_QUIT le cerraría
        // la aplicación entera al salir del popup.
        g_popupCerrado = true;
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
    g_popupCerrado = false;
    EnableWindow(parent, FALSE);

    HWND hPopup = CreateWindowExW(WS_EX_DLGMODALFRAME, claseName, L"Configurar CPS",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 230, 120, parent, NULL, GetModuleHandleW(NULL), NULL);

    if (!hPopup) {          // si la ventana no se pudo crear, no entramos al loop
        EnableWindow(parent, TRUE);
        return false;
    }

    MSG msg;
    bool huboQuit = false;
    while (!g_popupCerrado) {
        if (!GetMessageW(&msg, NULL, 0, 0)) { huboQuit = true; break; }
        if (!IsDialogMessageW(hPopup, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    // Si mientras el popup estaba abierto se pidió cerrar el programa entero,
    // reenviamos el WM_QUIT para que el loop principal también termine.
    if (huboQuit) PostQuitMessage((int)msg.wParam);

    if (g_popupOk) {
        *resultado = g_valorPopup;
        return true;
    }
    return false;
}

// ---------------- SUBCLASE DEL SLIDER (para detectar Shift+Click y click-directo) ----------------
bool g_arrastrandoSlider = false;

int PosDesdeX(HWND hwnd, int x) {
    RECT canal;
    SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&canal);
    int minPos = (int)SendMessageW(hwnd, TBM_GETRANGEMIN, 0, 0);
    int maxPos = (int)SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0);
    LONG ancho = canal.right - canal.left;
    if (ancho <= 0) return minPos;

    double proporcion = (double)(x - canal.left) / (double)ancho;
    if (proporcion < 0) proporcion = 0;
    if (proporcion > 1) proporcion = 1;
    return minPos + (int)(proporcion * (maxPos - minPos));
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
            // GET_X_LPARAM y no LOWORD: con SetCapture las coordenadas pueden
            // ser negativas (arrastrás fuera del control por la izquierda) y
            // LOWORD las convertiría en un número gigante.
            int nuevaPos = PosDesdeX(hwnd, GET_X_LPARAM(lParam));
            SendMessageW(hwnd, TBM_SETPOS, TRUE, nuevaPos);
            g_cps = nuevaPos / 1000.0;
            ActualizarLabelCps(g_cps.load());
            g_arrastrandoSlider = true;
            SetCapture(hwnd);
            return 0;
        }
    }
    if (msg == WM_MOUSEMOVE && g_arrastrandoSlider) {
        int nuevaPos = PosDesdeX(hwnd, GET_X_LPARAM(lParam));
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

// ---------------- PANEL DE DIAGNÓSTICO ----------------
// Muestra en vivo qué está viendo el programa. Sirve para verificar en tu
// máquina que la detección del cursor funciona (sobre todo en pantalla
// completa, que es donde la señal vieja fallaba).
void ActualizarDiagnostico() {
    bool mc = VentanaActivaEsMinecraft();
    bool shift = ShiftApretado();
    bool jugando = EstaJugando();
    bool permite = mc && (jugando || shift);

    std::wstring capa;
    if (g_senalCursorSirve.load())              capa = L"cursor oculto";
    else if (!VentanaCubreTodoElEscritorio())   capa = L"confinamiento (exacta)";
    else                                        capa = L"warp (heuristica)";

    std::wstring t;
    const Muestra& m = g_muestraVieja;
    if (!m.valida) {
        t = L"Con MC en foco: (todavia sin datos)\r\n\r\n";
    } else {
        t =  L"MC 0.5s atras: Ocul:"; t += m.oculto ? L"1" : L"0";
        t += L" Clip:"; t += m.clip ? L"1" : L"0";
        t += L" Warp:"; t += m.warp ? L"1" : L"0";
        t += L" Full:"; t += m.full ? L"1" : L"0";
        t += L" Sh:"; t += m.shift ? L"1" : L"0";
        t += m.permite ? L" -> CLICKEABA" : L" -> bloqueado";
        t += L"\r\ndCtr:" + std::to_wstring(m.dist);
        t += L" msCtr:" + std::to_wstring(m.msCentro);
        t += L" mOcul:" + std::to_wstring(g_muestrasOculto.load());
        t += L"\r\n";
    }

    t += L"Capa: " + capa;
    t += L"  |  Ahora MC:";
    t += mc ? L"si" : L"no";
    t += jugando ? L" Jug:si" : L" Jug:no";
    t += permite ? L" -> CLICKEA" : L" -> bloqueado";

    SetTextoSiCambio(hLabelDeteccion, t);
    SetTextoSiCambio(hLabelContador, L"Clicks generados: " + std::to_wstring(g_contador.load()));
}

// ---------------- VENTANA PRINCIPAL ----------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hLabelEstado = CreateWindowExW(0, L"STATIC", L"Estado: PAUSADO",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 10, 444, 20, hwnd, NULL, NULL, NULL);

        hLabelHotkey = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 35, 444, 20, hwnd, NULL, NULL, NULL);

        hLabelEmergencia = CreateWindowExW(0, L"STATIC", L"Emergencia (siempre desarma): F9",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 58, 444, 18, hwnd, NULL, NULL, NULL);

        hBtnHotkey = CreateWindowExW(0, L"BUTTON", L"Cambiar tecla",
            WS_CHILD | WS_VISIBLE, 182, 82, 100, 26, hwnd, (HMENU)IDC_BTN_HOTKEY, NULL, NULL);

        hLabelCps = CreateWindowExW(0, L"STATIC", L"CPS: 10.000",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 118, 444, 20, hwnd, NULL, NULL, NULL);

        hSliderCps = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ, 10, 142, 444, 30, hwnd, (HMENU)IDC_SLIDER_CPS, NULL, NULL);
        SendMessageW(hSliderCps, TBM_SETRANGE, TRUE, MAKELPARAM(1, 50000));
        SendMessageW(hSliderCps, TBM_SETPOS, TRUE, 10000);

        // Subclase del slider para detectar shift+click
        g_oldSliderProc = (WNDPROC)SetWindowLongPtrW(hSliderCps, GWLP_WNDPROC, (LONG_PTR)SliderSubclassProc);

        hLabelAyuda = CreateWindowExW(0, L"STATIC",
            L"Shift + click en el slider = escribir el n\u00famero a mano.\r\n"
            L"En inventario/men\u00fa no clickea, salvo que mantengas SHIFT.",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 176, 444, 32, hwnd, NULL, NULL, NULL);

        hLabelDeteccion = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 214, 444, 72, hwnd, NULL, NULL, NULL);

        hLabelContador = CreateWindowExW(0, L"STATIC", L"Clicks generados: 0",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 290, 444, 18, hwnd, NULL, NULL, NULL);

        ActualizarLabelHotkey();

        g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(NULL), 0);
        g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(NULL), 0);

        SetTimer(hwnd, ID_TIMER_CONTADOR, 15, NULL);
        g_hiloClicks = std::thread(HiloClicks);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_HOTKEY) {
            g_esperandoNuevaTecla = true;
            SetWindowTextW(hLabelHotkey, L"Presion\u00e1 una tecla...");
        }
        break;
    case WM_TIMER:
        if (wParam == ID_TIMER_CONTADOR) {
            // El muestreo tiene que ser rapido: el juego devuelve el cursor al
            // centro una vez por cuadro (~16 ms), asi que a 100 ms nos perdiamos
            // casi todos esos regresos mientras el mouse estaba en movimiento.
            MuestrearSeniales();
            static int tick = 0;
            if (++tick >= 7) { tick = 0; ActualizarDiagnostico(); }
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
        KillTimer(hwnd, ID_TIMER_CONTADOR);
        g_running = false;
        g_cvSalida.notify_all();   // despierta el hilo aunque esté en un sleep largo
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

    // Sin esto, Windows despierta los sleeps cada ~15.6ms y los CPS reales
    // quedan muy por debajo de los configurados (pedir 20 CPS da ~16).
    timeBeginPeriod(1);

    // Comparamos posiciones del cursor contra el area de la ventana de Minecraft.
    // Si este proceso no es "DPI aware" y la pantalla tiene escalado (125%, 150%),
    // Windows nos entrega coordenadas virtualizadas y esa comparacion puede no
    // cerrar nunca. Esto nos pone en el mismo espacio de coordenadas que el juego.
    SetProcessDPIAware();

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
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 372,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hMain, nCmdShow);
    UpdateWindow(hMain);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    timeEndPeriod(1);
    return 0;
}
