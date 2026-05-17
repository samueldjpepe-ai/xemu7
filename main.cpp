#define SDL_MAIN_HANDLED
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <map>
#include <string>
#include <conio.h>
#include <algorithm>
#include <sstream>
#include <SDL.h>
#include "ViGEm/Client.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")

// --- ESTRUCTURAS DEL EMULADOR ---
struct Mapeo {
    int botones[10]; 
    int dpad_tipo;   
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

// Estructura para emparejar lo que ve SDL2 con lo que necesita HidGuardian
struct MandoFiltrado {
    int indexSDL;
    std::string nameSDL;
    std::string guidSDL;
    std::wstring hwID;
};

std::map<std::string, Mapeo> baseDeDatos;
std::vector<std::string> nombresBotones = {"A", "B", "X", "Y", "LB", "RB", "START", "BACK", "L3 (Stick Izq)", "R3 (Stick Der)"};
std::vector<std::wstring> g_bloqueados;

// --- CONTROL DE PRIVILEGIOS ---
bool EsAdministrador() {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            fRet = elevation.TokenIsElevated;
        }
    }
    if (hToken) CloseHandle(hToken);
    return fRet;
}

// --- FUNCIÓN INTELIGENTE DE RASTREO DE HWID ---
std::wstring BuscarHardwareIDPorNombre(const std::string& nombreBuscado) {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return L"";

    // Convertir el nombre de SDL a minusculas para comparar mejor
    std::string nombreSDL = nombreBuscado;
    std::transform(nombreSDL.begin(), nombreSDL.end(), nombreSDL.begin(), ::tolower);

    SP_DEVINFO_DATA devData = { sizeof(SP_DEVINFO_DATA) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); i++) {
        wchar_t buf[1024];
        std::string nombreWindows = "";

        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf, sizeof(buf), NULL) ||
            SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_DEVICEDESC, NULL, (PBYTE)buf, sizeof(buf), NULL)) {
            
            // Convertir wchar_t de Windows a std::string para comparar
            std::wstring ws(buf);
            nombreWindows = std::string(ws.begin(), ws.end());
            std::transform(nombreWindows.begin(), nombreWindows.end(), nombreWindows.begin(), ::tolower);
        }

        // Si el nombre del Administrador de Dispositivos coincide o contiene el nombre que nos dio SDL
        if (!nombreWindows.empty() && (nombreWindows.find(nombreSDL) != std::string::npos || nombreSDL.find(nombreWindows) != std::string::npos)) {
            if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_HARDWAREID, NULL, (PBYTE)buf, sizeof(buf), NULL)) {
                std::wstring hwID = buf;
                if (hwID.find(L"VID_") != std::wstring::npos) {
                    SetupDiDestroyDeviceInfoList(hDevInfo);
                    return hwID; // Encontrado
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return L""; 
}

// --- FUNCIONES HIDGUARDIAN ---
bool AplicarCambiosHidGuardian(bool bloquear) {
    HKEY hKey;
    LPCWSTR path = L"SYSTEM\\CurrentControlSet\\Services\\HidGuardian\\Parameters";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS) return false;

    std::vector<wchar_t> multiSz;
    if (bloquear) {
        for (const auto& id : g_bloqueados) {
            for (wchar_t c : id) multiSz.push_back(c);
            multiSz.push_back(L'\0');
        }
    }
    multiSz.push_back(L'\0'); 
    RegSetValueExW(hKey, L"AffectedDevices", 0, REG_MULTI_SZ, (BYTE*)multiSz.data(), (DWORD)(multiSz.size() * sizeof(wchar_t)));

    std::wstring p = L"Whitelist\\" + std::to_wstring(GetCurrentProcessId());
    HKEY hSubKey;
    if (RegCreateKeyExW(hKey, p.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hSubKey, NULL) == ERROR_SUCCESS) {
        RegCloseKey(hSubKey);
    }
    RegCloseKey(hKey);
    return true;
}

void ReiniciarSistemaHID() {
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

// --- FUNCIONES DEL EMULADOR ---
void cargarConfig() {
    std::ifstream archivo("controles_config.dat", std::ios::binary);
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

void guardarConfig() {
    std::ofstream archivo("controles_config.dat", std::ios::binary);
    for (auto const& [guid, mapeo] : baseDeDatos) {
        archivo << guid << " ";
        archivo.write((char*)&mapeo, sizeof(Mapeo));
    }
    archivo.close();
}

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
    guardarConfig();
    std::cout << "\n¡Configuracion guardada!\n";
    Sleep(1000);
}

// --- MAIN ---
int main(int argc, char* argv[]) {
    if (!EsAdministrador()) {
        wchar_t szPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, szPath, MAX_PATH)) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"runas"; 
            sei.lpFile = szPath;
            sei.hwnd = NULL;
            sei.nShow = SW_NORMAL;
            if (ShellExecuteExW(&sei)) return 0;
        }
        std::cout << "ERROR: Requiere privilegios de Administrador.\n";
        Sleep(3000);
        return 1;
    }

    SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_VIDEO);
    cargarConfig();

    // ==========================================
    // PASO 1: DETECCIÓN INTELIGENTE FILTRADA POR SDL2
    // ==========================================
    std::cout << "=== PASO 1: DETECCION DE MANDOS REALES ===\n\n";
    SDL_JoystickUpdate();
    int numJoysticks = SDL_NumJoysticks();
    std::vector<MandoFiltrado> mandosValidos;

    if (numJoysticks == 0) {
        std::cout << "No se detectaron mandos de juego conectados mediante SDL2.\n";
    } else {
        for (int i = 0; i < numJoysticks; i++) {
            SDL_Joystick* j = SDL_JoystickOpen(i);
            if (j) {
                std::string nombreMando = SDL_JoystickName(j);
                char guidStr[33];
                SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guidStr, 33);

                // El programa busca de forma invisible el Hardware ID en Windows usando el nombre
                std::wstring hwIDEncontrado = BuscarHardwareIDPorNombre(nombreMando);

                MandoFiltrado mf = { i, nombreMando, std::string(guidStr), hwIDEncontrado };
                mandosValidos.push_back(mf);

                std::cout << "[" << mandosValidos.size() << "] Mando: " << mf.nameSDL << "\n";
                if (!mf.hwID.empty()) {
                    std::wcout << L"    Hardware ID (Auto-Detectado): " << mf.hwID << L"\n";
                    g_bloqueados.push_back(mf.hwID); // Se agrega a la lista de bloqueo
                } else {
                    std::cout << "    Hardware ID: [No se pudo recuperar el ID de registro]\n";
                }
                std::cout << "\n";
                SDL_JoystickClose(j);
            }
        }
    }

    // ==========================================
    // PASO 2: OCULTAMIENTO (HidGuardian)
    // ==========================================
    std::cout << "=== PASO 2: OCULTAMIENTO (HidGuardian) ===\n";
    std::cout << "[\242Quieres bloquear/ocultar los mandos f\244sicos listados arriba?]\n";
    std::cout << "[1] SI, bloquear originales.\n";
    std::cout << "[0] NO, mantenerlos libres.\n";
    std::cout << "Selecci\242n (0/1): ";
    int opcBloqueo = 0;
    std::cin >> opcBloqueo;

    if (opcBloqueo == 1 && !g_bloqueados.empty()) {
        if (AplicarCambiosHidGuardian(true)) {
            ReiniciarSistemaHID();
            std::cout << "[OK] Mandos ocultados con éxito.\n\n";
        } else {
            std::cout << "[!] ERROR: Fall\242 al escribir en HidGuardian.\n\n";
        }
    } else {
        AplicarCambiosHidGuardian(false); 
        ReiniciarSistemaHID();
        std::cout << "[INFO] Mandos libres y visibles.\n\n";
    }
    Sleep(1000);

    // ==========================================
    // PASO 3: MENÚ DE EMULACIÓN Y CONFIGURACIÓN
    // ==========================================
    PVIGEM_CLIENT client = vigem_alloc();
    vigem_connect(client);

    while (true) {
        SDL_JoystickUpdate();
        system("cls");
        std::cout << "=== PASO 3: CONTROL DE EMULACION V3 (FILTRADO) ===\n\n";
        
        // Volvemos a listar solo los mandos detectados por el filtro
        for (size_t i = 0; i < mandosValidos.size(); i++) {
            std::cout << "Mando [" << i + 1 << "]: " << mandosValidos[i].nameSDL 
                      << (baseDeDatos.count(mandosValidos[i].guidSDL) ? " [LISTO]" : " [!] CONFIGURAR REQUERIDA") << std::endl;
        }

        std::cout << "\nOPCIONES DISPONIBLES:\n";
        std::cout << "[1] INICIAR EMULACION DE MANDOS VIRTUALES (Jugar)\n";
        std::cout << "[0] IR A CONFIGURAR / CALIBRAR MANDOS\n";
        std::cout << "[ESC] Salir del programa\n\n";
        std::cout << "Selecci\242n: ";
        
        std::string inputMenu;
        std::cin >> inputMenu;

        if (inputMenu == "1") {
            std::vector<ControllerPair> emus;
            for (size_t i = 0; i < mandosValidos.size(); i++) {
                if (baseDeDatos.count(mandosValidos[i].guidSDL)) {
                    emus.push_back({SDL_JoystickOpen(mandosValidos[i].indexSDL), vigem_target_x360_alloc(), mandosValidos[i].guidSDL});
                    vigem_target_add(client, emus.back().virtualPad);
                }
            }

            if(emus.empty()) {
                std::cout << "\n[!] Configura tus mandos primero con la opci\242n [0].\n";
                Sleep(2500);
                continue;
            }

            std::cout << "\n>>> EMULANDO MANDOS XBOX 360 ACTIVOS <<<\n";
            std::cout << "Presiona la tecla 'M' para detener la emulaci\242n y volver.\n";
            
            while (true) {
                if (_kbhit() && tolower(_getch()) == 'm') break;
                SDL_JoystickUpdate();
                for (auto& e : emus) {
                    XUSB_REPORT r; XUSB_REPORT_INIT(&r);
                    Mapeo m = baseDeDatos[e.guid];

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

                    r.sThumbLY = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[0]), m.signos[0], m.deadzoneIZQ, true, m.invertirX, m.invertirY);
                    r.sThumbLX = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[1]), m.signos[1], m.deadzoneIZQ, false, m.invertirX, m.invertirY);
                    r.sThumbRY = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[2]), m.signos[2], m.deadzoneDER, true, m.invertirX, m.invertirY);
                    r.sThumbRX = aplicarEjeFinal(SDL_JoystickGetAxis(e.physical, m.ejes[3]), m.signos[3], false, m.deadzoneDER, m.invertirX, m.invertirY);

                    if (SDL_JoystickGetButton(e.physical, 6)) r.bLeftTrigger = 255;
                    if (SDL_JoystickGetButton(e.physical, 7)) r.bRightTrigger = 255;

                    vigem_target_x360_update(client, e.virtualPad, r);
                }
                Sleep(8);
            }

            for(auto& e : emus){ vigem_target_remove(client, e.virtualPad); vigem_target_free(e.virtualPad); }

        } else if (inputMenu == "0") {
            std::cout << "\nIngrese el n\243mero de mando a calibrar (1-" << mandosValidos.size() << "): ";
            int indexMando;
            std::cin >> indexMando;
            if (indexMando > 0 && indexMando <= (int)mandosValidos.size()) {
                calibrarMando(SDL_JoystickOpen(mandosValidos[indexMando - 1].indexSDL), mandosValidos[indexMando - 1].guidSDL);
            }
        } else {
            break; 
        }
    }

    AplicarCambiosHidGuardian(false); 
    ReiniciarSistemaHID();
    vigem_disconnect(client);
    vigem_free(client);
    return 0;
}
