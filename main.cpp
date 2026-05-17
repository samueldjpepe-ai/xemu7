#define SDL_MAIN_HANDLED
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <map>
#include <conio.h>
#include <SDL.h>
#include "ViGEm/Client.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")

// --- ESTRUCTURAS UNIFICADAS ---

struct Mapeo {
    int botones[10]; // A, B, X, Y, LB, RB, START, BACK, L3, R3
    int dpad_tipo;   // 0: Botones, 1: Hat
    int dpad_ids[4]; 
    int ejes[4];     
    int signos[4];   
    int deadzoneIZQ; 
    int deadzoneDER; 
    bool invertirX;  
    bool invertirY;  
};

struct ControllerPair {
    SDL_Joystick* physical;
    PVIGEM_TARGET virtualPad;
    std::string guid;
};

struct MandoInfo {
    std::wstring name;
    std::wstring hwID;
};

// --- VARIABLES GLOBALES ---
std::vector<std::wstring> g_bloqueados;
std::map<std::string, Mapeo> baseDeDatos;
std::vector<std::string> nombresBotones = {"A", "B", "X", "Y", "LB", "RB", "START", "BACK", "L3 (Stick Izq)", "R3 (Stick Der)"};

const std::string CONFIG_MANDOS_REG = "config_mando.txt";      // Archivo de HidGuardian (IDs bloqueadas)
const std::string CONFIG_MAPEOS_DAT = "controles_config.dat"; // Archivo de calibración (Mapeos)

PVIGEM_CLIENT client = nullptr;

// --- FUNCIONES AUXILIARES DE SISTEMA Y NOMBRE ---

std::wstring ObtenerNombrePropioExe() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring ruta(buffer);
    size_t pos = ruta.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? ruta : ruta.substr(pos + 1);
}

// --- PERSISTENCIA Y CARGA DE ARCHIVOS ---

void cargarMapeosSDL() {
    std::ifstream archivo(CONFIG_MAPEOS_DAT, std::ios::binary);
    if (!archivo) return;
    std::string guid;
    while (archivo >> guid) {
        archivo.ignore();
        Mapeo m;
        archivo.read((char*)&m, sizeof(Mapeo));
        baseDeDatos[guid] = m;
    }
    archivo.close();
}

void guardarMapeosSDL() {
    std::ofstream archivo(CONFIG_MAPEOS_DAT, std::ios::binary);
    for (auto const& [guid, mapeo] : baseDeDatos) {
        archivo << guid << " ";
        archivo.write((char*)&mapeo, sizeof(Mapeo));
    }
    archivo.close();
}

void guardarConfigHidGuardian() {
    std::wofstream archivo(CONFIG_MANDOS_REG);
    if (archivo.is_open()) {
        archivo << ObtenerNombrePropioExe() << L"\n";
        for (const auto& id : g_bloqueados) {
            archivo << id << L"\n";
        }
        archivo.close();
    }
}

bool cargarConfigHidGuardian() {
    std::wifstream archivo(CONFIG_MANDOS_REG);
    if (!archivo.is_open()) return false;

    g_bloqueados.clear();
    std::wstring exeGuardado;
    if (std::getline(archivo, exeGuardado)) {
        // Ignoramos verificación de PID externo, nos auto-agregamos dinámicamente
    }

    std::wstring lineaId;
    while (std::getline(archivo, lineaId)) {
        if (!lineaId.empty()) {
            g_bloqueados.push_back(lineaId);
        }
    }
    archivo.close();
    return true;
}

// --- LÓGICA HIDGUARDIAN (FILTRADO Y REPOSITORIO DE REGISTRO) ---

bool AplicarCambiosRegistro() {
    HKEY hKey;
    LPCWSTR path = L"SYSTEM\\CurrentControlSet\\Services\\HidGuardian\\Parameters";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS) return false;

    // Escribir la lista de dispositivos afectados (bloqueados)
    std::vector<wchar_t> multiSz;
    for (const auto& id : g_bloqueados) {
        for (wchar_t c : id) multiSz.push_back(c);
        multiSz.push_back(L'\0');
    }
    multiSz.push_back(L'\0'); 
    RegSetValueExW(hKey, L"AffectedDevices", 0, REG_MULTI_SZ, (BYTE*)multiSz.data(), (DWORD)(multiSz.size() * sizeof(wchar_t)));

    // AUTO-WHITELIST RADICAL: Agregamos nuestro propio PID en caliente
    DWORD miPID = GetCurrentProcessId();
    std::wstring p = L"Whitelist\\" + std::to_wstring(miPID);
    HKEY hSubKey;
    if (RegCreateKeyExW(hKey, p.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hSubKey, NULL) == ERROR_SUCCESS) {
        RegCloseKey(hSubKey);
    }

    RegCloseKey(hKey);
    return true;
}

void ReiniciarSubSistemaHID() {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA devData = { sizeof(SP_DEVINFO_DATA) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); i++) {
        SP_PROPCHANGE_PARAMS pcp = { {sizeof(SP_CLASSINSTALL_HEADER), DIF_PROPERTYCHANGE}, DICS_PROPCHANGE, DICS_FLAG_GLOBAL, 0 };
        if (SetupDiSetClassInstallParamsW(hDevInfo, &devData, &pcp.ClassInstallHeader, sizeof(pcp))) {
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, &devData);
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
}

// --- FILTRADO DE MANDOS MEDIANTE SDL2 ---

struct SDL_GamepadID {
    Uint16 vid;
    Uint16 pid;
};

std::vector<SDL_GamepadID> ObtenerControlesActivosPorSDL() {
    std::vector<SDL_GamepadID> listaControles;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        Uint16 vid = SDL_JoystickGetDeviceVendor(i);
        Uint16 pid = SDL_JoystickGetDeviceProduct(i);
        if (vid != 0 || pid != 0) {
            listaControles.push_back({ vid, pid });
        }
    }
    return listaControles;
}

bool ValidarMandoConSDL2(const std::wstring& hwID, const std::vector<SDL_GamepadID>& controlesSDL) {
    std::wstring idUpper = hwID;
    std::transform(idUpper.begin(), idUpper.end(), idUpper.begin(), ::towupper);

    for (const auto& ctrl : controlesSDL) {
        wchar_t vidStr[32];
        wchar_t pidStr[32];
        swprintf_s(vidStr, L"VID_%04X", ctrl.vid);
        swprintf_s(pidStr, L"PID_%04X", ctrl.pid);

        if (idUpper.find(vidStr) != std::wstring::npos && idUpper.find(pidStr) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::vector<MandoInfo> ListarMandosFiltrados() {
    std::vector<MandoInfo> lista;
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return lista;

    auto controlesSDL = ObtenerControlesActivosPorSDL();
    SP_DEVINFO_DATA devData = { sizeof(SP_DEVINFO_DATA) };

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); i++) {
        wchar_t buf[1024];
        MandoInfo m;
        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf, sizeof(buf), NULL) ||
            SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_DEVICEDESC, NULL, (PBYTE)buf, sizeof(buf), NULL)) {
            m.name = buf;
        } else {
            m.name = L"Dispositivo desconocido";
        }
        
        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_HARDWAREID, NULL, (PBYTE)buf, sizeof(buf), NULL)) {
            m.hwID = buf; 
            if (ValidarMandoConSDL2(m.hwID, controlesSDL)) {
                lista.push_back(m);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return lista;
}

// --- LÓGICA DE CALIBRACIÓN Y EMULACIÓN ---

short aplicarEjeFinal(int valorActual, int signoCalibrado, int deadzone, bool esEjeY, bool invX, bool invY) {
    int dz = deadzone * 100;
    if (abs(valorActual) < dz) return 0;

    float normalizado = (float)valorActual / 32767.0f;
    if (signoCalibrado < 0) normalizado = -normalizado;

    float resultado;
    if (esEjeY) {
        resultado = -normalizado; 
        if (invY) resultado = -resultado;
    } else {
        resultado = normalizado;  
        if (invX) resultado = -resultado;
    }

    int finalVal = (int)(resultado * 32767.0f);
    if (finalVal > 32767) return 32767;
    if (finalVal < -32768) return -32768;
    return (short)finalVal;
}

void calibrarMando(SDL_Joystick* joy, std::string guid) {
    Mapeo nuevo;
    system("cls");
    std::cout << "=== CONFIGURACION COMPLETA: " << SDL_JoystickName(joy) << " ===\n";
    
    std::cout << "Zona muerta IZQ (1-100): "; std::cin >> nuevo.deadzoneIZQ;
    std::cout << "Zona muerta DER (1-100): "; std::cin >> nuevo.deadzoneDER;
    
    char res;
    std::cout << "¿Invertir Eje X (Izquierda/Derecha)? (s/n): "; std::cin >> res;
    nuevo.invertirX = (res == 's' || res == 'S');
    std::cout << "¿Invertir Eje Y (Arriba/Abajo)? (s/n): "; std::cin >> res;
    nuevo.invertirY = (res == 's' || res == 'S');

    for (int i = 0; i < 10; i++) {
        std::cout << "Presiona [" << nombresBotones[i] << "]: " << std::flush;
        bool cap = false;
        while (!cap) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.which == SDL_JoystickInstanceID(joy)) {
                    nuevo.botones[i] = ev.jbutton.button;
                    std::cout << "OK (" << ev.jbutton.button << ")\n";
                    cap = true; Sleep(400);
                }
            }
        }
    }

    std::cout << "\n--- CONFIGURAR CRUCETA ---\n";
    for (int i = 0; i < 4; i++) {
        std::cout << "Presiona " << (i==0?"ARRIBA":i==1?"ABAJO":i==2?"IZQUIERDA":"DERECHA") << ": " << std::flush;
        bool cap = false;
        while (!cap) {
            SDL_JoystickUpdate();
            for (int b=0; b<SDL_JoystickNumButtons(joy); b++) {
                if(SDL_JoystickGetButton(joy, b)) {
                    nuevo.dpad_tipo = 0; nuevo.dpad_ids[i] = b;
                    std::cout << "OK (Boton " << b << ")\n"; cap = true; Sleep(400); break;
                }
            }
            if(cap) break;
            for (int h=0; h<SDL_JoystickNumHats(joy); h++) {
                Uint8 v = SDL_JoystickGetHat(joy, h);
                if(v != SDL_HAT_CENTERED) {
                    nuevo.dpad_tipo = 1; nuevo.dpad_ids[i] = v;
                    std::cout << "OK (Hat " << (int)v << ")\n"; cap = true; Sleep(400); break;
                }
            }
        }
    }

    std::vector<std::string> nEjes = {"IZQUIERDO hacia ARRIBA", "IZQUIERDO hacia DERECHA", "DERECHO hacia ARRIBA", "DERECHO hacia DERECHA"};
    for (int i = 0; i < 4; i++) {
        std::cout << "Mueve " << nEjes[i] << " y MANTEN: " << std::flush;
        bool cap = false;
        while (!cap) {
            SDL_JoystickUpdate();
            for (int a = 0; a < SDL_JoystickNumAxes(joy); a++) {
                int val = SDL_JoystickGetAxis(joy, a);
                if (abs(val) > 22000) { 
                    nuevo.ejes[i] = a;
                    nuevo.signos[i] = (val > 0) ? 1 : -1; 
                    std::cout << "OK\n";
                    cap = true; Sleep(800); break;
                }
            }
        }
    }

    baseDeDatos[guid] = nuevo;
    guardarMapeosSDL();
}

// --- SUB-MENÚ DE GESTIÓN DE HIDGUARDIAN (PASO ADICIONAL OPCIONAL) ---

void MenuDispositivosHid() {
    std::wstring input;
    while (true) {
        system("cls");
        std::wcout << L"=== PASO 2: GESTION DE BLOQUEO DE DISPOSITIVOS ===\n";
        std::wcout << L"Este programa ya se encuentra en la Whitelist automáticamente.\n\n";
        
        auto mandos = ListarMandosFiltrados();
        std::wcout << L"ID | ESTADO   | CONTROL REAL DETECTADO (SDL2)\n";
        std::wcout << L"---|----------|------------------------------------\n";
        for (size_t i = 0; i < mandos.size(); i++) {
            bool b = std::find(g_bloqueados.begin(), g_bloqueados.end(), mandos[i].hwID) != g_bloqueados.end();
            std::wcout << i + 1 << L". [" << (b ? L"BLOQUEADO" : L" LIBRE   ") << L"] " << mandos[i].name << L"\n";
            std::wcout << L"   ID: " << mandos[i].hwID << L"\n\n";
        }

        if(mandos.empty()) std::wcout << L" No se detectan controles por hardware vinculados a SDL2.\n\n";

        std::wcout << L"OPCIONES:\n";
        std::wcout << L"- Ingrese numeros separados por coma (ej: 1,2) para alternar el bloqueo.\n";
        std::wcout << L"- [0] Finalizar gestión de bloqueos y Continuar.\n";
        std::wcout << L"Seleccion: ";
        std::wcin >> input;

        if (input == L"0") break;

        std::wstringstream ss(input);
        std::wstring item;
        while (std::getline(ss, item, L',')) {
            try {
                int idx = std::stoi(item);
                if (idx > 0 && idx <= (int)mandos.size()) {
                    auto it = std::find(g_bloqueados.begin(), g_bloqueados.end(), mandos[idx - 1].hwID);
                    if (it != g_bloqueados.end()) g_bloqueados.erase(it);
                    else g_bloqueados.push_back(mandos[idx - 1].hwID);
                }
            } catch (...) {}
        }

        if (!AplicarCambiosRegistro()) {
            std::wcout << L"\n[!] ERROR: No se pudo escribir el registro. Revisa privilegios Admin.\n";
            Sleep(2000);
        } else {
            guardarConfigHidGuardian();
        }
        ReiniciarSubSistemaHID();
        Sleep(600);
    }
}

// --- ENTRADA PRINCIPAL ---

int main(int argc, char* argv[]) {
    _wsetlocale(LC_ALL, L"");
    
    // Inicializar subsistemas de video, eventos y joysticks de SDL2
    SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    
    // Inicializar cliente virtual de ViGEm Bus
    client = vigem_alloc();
    if (vigem_connect(client) != VIGEM_ERROR_NONE) {
        std::cout << "[!] ERROR: No se pudo conectar al Driver de ViGEm Bus. ¿Esta instalado?\n";
        SDL_Quit();
        return -1;
    }

    // Cargar ambas configuraciones persistentes
    cargarMapeosSDL();
    bool configPreviaHid = cargarConfigHidGuardian();

    bool saltarMenuHid = false;

    // Menú de arranque inicial
    if (configPreviaHid) {
        std::wcout << L"=== SE DETECTÓ CONFIGURACIÓN PREVIA DE HARDWARE ===\n";
        std::wcout << L"[0] Cargar bloqueos automáticos y avanzar a emulación\n";
        std::wcout << L"[1] Reconfigurar bloqueos de hardware (HidGuardian)\n";
        std::wcout << L"Selección: ";
        int opc;
        if (std::wcin >> opc && opc == 0) {
            if (AplicarCambiosRegistro()) {
                ReiniciarSubSistemaHID();
                saltarMenuHid = true;
                std::wcout << L"[OK] Bloqueos aplicados. Auto-Whitelist inyectada.\n";
                Sleep(1000);
            }
        }
    }

    if (!saltarMenuHid) {
        MenuDispositivosHid();
    }

    // Bucle Principal del Emulador XInput
    while (true) {
        SDL_JoystickUpdate();
        system("cls");
        int n = SDL_NumJoysticks();
        std::cout << "=== EMULADOR XINPUT MASTER V3 (HIDGUARDIAN ACTIVO) ===\n";
        
        std::vector<std::string> gList;
        for (int i = 0; i < n && i < 6; i++) {
            SDL_Joystick* j = SDL_JoystickOpen(i);
            char gs[33]; 
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), gs, 33);
            gList.push_back(gs);
            std::cout << "[" << i + 1 << "] " << SDL_JoystickName(j) 
                      << (baseDeDatos.count(gs) ? " [MAPEO LISTO]" : " [!] REQUIERE CONFIGURAR") << std::endl;
        }

        std::cout << "\n[0] INICIAR EMULACIÓN (JUGAR)\n";
        std::cout << "[G] Volver a Gestionar Bloqueos (HidGuardian)\n";
        std::cout << "[ESC] Salir del Software\n";
        std::cout << "Selección: ";
        
        std::string seleccion;
        std::cin >> seleccion;

        if (seleccion == "0") {
            std::vector<ControllerPair> emus;
            for (int i = 0; i < n && i < 6; i++) {
                if (baseDeDatos.count(gList[i])) {
                    emus.push_back({SDL_JoystickOpen(i), vigem_target_x360_alloc(), gList[i]});
                    vigem_target_add(client, emus.back().virtualPad);
                }
            }
            
            std::cout << "\n>>> EMULANDO MANDOS XBOX 360 EN SEGUNDO PLANO <<<\n";
            std::cout << "Presiona 'M' en el teclado para volver al menú.\n";
            
            while (true) {
                if (_kbhit() && tolower(_getch()) == 'm') break;
                
                SDL_JoystickUpdate();
                // Asegurar refresco dinámico de la Whitelist del software activo
                AplicarCambiosRegistro(); 

                for (auto& e : emus) {
                    XUSB_REPORT r; 
                    XUSB_REPORT_INIT(&r);
                    Mapeo m = baseDeDatos[e.guid];

                    // Botones Físicos -> Virtuales
                    if(SDL_JoystickGetButton(e.physical, m.botones[0])) r.wButtons |= XUSB_GAMEPAD_A;
                    if(SDL_JoystickGetButton(e.physical, m.botones[1])) r.wButtons |= XUSB_GAMEPAD_B;
                    if(SDL_JoystickGetButton(e.physical, m.botones[2])) r.wButtons |= XUSB_GAMEPAD_X;
                    if(SDL_JoystickGetButton(e.physical, m.botones[3])) r.wButtons |= XUSB_GAMEPAD_Y;
                    if(SDL_JoystickGetButton(e.physical, m.botones[4])) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
                    if(SDL_JoystickGetButton(e.physical, m.botones[5])) r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
                    if(SDL_JoystickGetButton(e.physical, m.botones[6])) r.wButtons |= XUSB_GAMEPAD_START;
                    if(SDL_JoystickGetButton(e.physical, m.botones[7])) r.wButtons |= XUSB_GAMEPAD_BACK;
                    if(SDL_JoystickGetButton(e.physical, m.botones[8])) r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB; 
                    if(SDL_JoystickGetButton(e.physical, m.botones[9])) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB; 

                    // Cruceta / DPAD
                    if (m.dpad_tipo == 0) {
                        if(SDL_JoystickGetButton(e.physical, m.dpad_ids[0])) r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
                        if(SDL_JoystickGetButton(e.physical, m.dpad_ids[1])) r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
                        if(SDL_JoystickGetButton(e.physical, m.dpad_ids[2])) r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
                        if(SDL_JoystickGetButton(e.physical, m.dpad_ids[3])) r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
                    } else {
                        Uint8 h = SDL_JoystickGetHat(e.physical, 0);
                        if(h & SDL_HAT_UP) r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
                        if(h & SDL_HAT_DOWN) r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
                        if(h & SDL_HAT_LEFT) r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
                        if(h & SDL_HAT_RIGHT) r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
                    }

                    // Sticks Analógicos de Entrada con Ajustes Modificados
                    r.sThumbLY = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[0]), m.signos[0], m.deadzoneIZQ, true, m.invertirX, m.invertirY);
                    r.sThumbLX = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[1]), m.signos[1], m.deadzoneIZQ, false, m.invertirX, m.invertirY);
                    r.sThumbRY = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[2]), m.signos[2], m.deadzoneDER, true, m.invertirX, m.invertirY);
                    r.sThumbRX = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[3]), m.signos[3], m.deadzoneDER, false, m.invertirX, m.invertirY);

                    // Triggers Genéricos asignados de forma estática
                    if (SDL_JoystickGetButton(e.physical, 6)) r.bLeftTrigger = 255;
                    if (SDL_JoystickGetButton(e.physical, 7)) r.bRightTrigger = 255;

                    vigem_target_x360_update(client, e.virtualPad, r);
                }
                Sleep(8);
            }

            // Desvincular mandos al regresar al menú principal
            for(auto& e : emus){ 
                vigem_target_remove(client, e.virtualPad); 
                vigem_target_free(e.virtualPad); 
            }
        } 
        else if (seleccion == "G" || seleccion == "g") {
            MenuDispositivosHid();
        } 
        else if (seleccion == "ESC" || seleccion == "esc") {
            break;
        } 
        else {
            try {
                int op = std::stoi(seleccion);
                if (op > 0 && op <= n) {
                    SDL_Joystick* jSel = SDL_JoystickOpen(op - 1);
                    char gs[33]; 
                    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(jSel), gs, 33);
                    calibrarMando(jSel, gs);
                }
            } catch(...) {}
        }
    }

    // Desconexión limpia del entorno de simulación
    if (client) {
        vigem_disconnect(client);
        vigem_free(client);
    }
    SDL_Quit();
    return 0;
}
