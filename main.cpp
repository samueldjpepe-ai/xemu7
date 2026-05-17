#define SDL_MAIN_HANDLED
#include <windows.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <map>
#include <string>
#include <conio.h>
#include <SDL.h>
#include "ViGEm/Client.h"

#pragma comment(lib, "setupapi.lib")

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

std::map<std::string, Mapeo> baseDeDatos;
std::vector<std::string> nombresBotones = {"A", "B", "X", "Y", "LB", "RB", "START", "BACK", "L3 (Stick Izq)", "R3 (Stick Der)"};

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
        resultado = -normalizado; // Xbox Up = Negativo
        if (invY) resultado = -resultado;
    } else {
        resultado = normalizado;  // Xbox Right = Positivo
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

    // 1. BOTONES (Incluyendo L3 y R3)
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

    // 2. DPAD (CRUCETA)
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

    // 3. ANALOGOS
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
    std::cout << "\n¡Configuracion guardada! ¿Quieres configurar otro mando? (0 para Jugar): ";
}

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_VIDEO);
    PVIGEM_CLIENT client = vigem_alloc();
    vigem_connect(client);
    cargarConfig();

    while (true) {
        SDL_JoystickUpdate();
        system("cls");
        int n = SDL_NumJoysticks();
        std::cout << "=== EMULADOR XINPUT MASTER V3 ===\n";
        std::vector<std::string> gList;
        for (int i = 0; i < n && i < 6; i++) {
            SDL_Joystick* j = SDL_JoystickOpen(i);
            char gs[33]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), gs, 33);
            gList.push_back(gs);
            std::cout << "[" << i + 1 << "] " << SDL_JoystickName(j) << (baseDeDatos.count(gs) ? " [LISTO]" : " [!] CONFIGURAR") << std::endl;
        }

        std::cout << "\n[0] JUGAR | [1-6] Configurar | [ESC] Salir: ";
        int op; std::cin >> op;

        if (op == 0) {
            std::vector<ControllerPair> emus;
            for (int i = 0; i < n && i < 6; i++) {
                if (baseDeDatos.count(gList[i])) {
                    emus.push_back({SDL_JoystickOpen(i), vigem_target_x360_alloc(), gList[i]});
                    vigem_target_add(client, emus.back().virtualPad);
                }
            }
            std::cout << "EMULANDO... Presiona 'M' para volver al menu.\n";
            while (true) {
                if (_kbhit() && tolower(_getch()) == 'm') break;
                SDL_JoystickUpdate();
                for (auto& e : emus) {
                    XUSB_REPORT r; XUSB_REPORT_INIT(&r);
                    Mapeo m = baseDeDatos[e.guid];

                    // Botones corregidos (A, B, X, Y, LB, RB, Start, Back, L3, R3)
                    if(SDL_JoystickGetButton(e.physical, m.botones[0])) r.wButtons |= XUSB_GAMEPAD_A;
                    if(SDL_JoystickGetButton(e.physical, m.botones[1])) r.wButtons |= XUSB_GAMEPAD_B;
                    if(SDL_JoystickGetButton(e.physical, m.botones[2])) r.wButtons |= XUSB_GAMEPAD_X;
                    if(SDL_JoystickGetButton(e.physical, m.botones[3])) r.wButtons |= XUSB_GAMEPAD_Y;
                    if(SDL_JoystickGetButton(e.physical, m.botones[4])) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
                    if(SDL_JoystickGetButton(e.physical, m.botones[5])) r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
                    if(SDL_JoystickGetButton(e.physical, m.botones[6])) r.wButtons |= XUSB_GAMEPAD_START;
                    if(SDL_JoystickGetButton(e.physical, m.botones[7])) r.wButtons |= XUSB_GAMEPAD_BACK;
                    if(SDL_JoystickGetButton(e.physical, m.botones[8])) r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB; // L3
                    if(SDL_JoystickGetButton(e.physical, m.botones[9])) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB; // R3

                    // DPAD
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

                    // Gatillos (L2/R2 suelen ser botones 6 y 7 en Twin USB)
                    if (SDL_JoystickGetButton(e.physical, 6)) r.bLeftTrigger = 255;
                    if (SDL_JoystickGetButton(e.physical, 7)) r.bRightTrigger = 255;

                    vigem_target_x360_update(client, e.virtualPad, r);
                }
                Sleep(8);
            }
            for(auto& e : emus){ vigem_target_remove(client, e.virtualPad); vigem_target_free(e.virtualPad); }
        } else if (op > 0 && op <= (int)gList.size()) {
            calibrarMando(SDL_JoystickOpen(op - 1), gList[op - 1]);
        }
    }
    return 0;
}
