#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <random>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <atomic> 
#include <unistd.h> 
#include <limits>   
#include <cstdlib>
#include <algorithm> 

using namespace std;

// ============================================================================
// CONSTANTES Y ENUMS
// ============================================================================
#define MAX_COCHES_SIMULTANEOS 3
#define MAX_COCHES_SEGUIDOS 5
#define TOTAL_COCHES_POR_LADO 8
#define TIEMPO_CRUCE_MS 2000

#define MAX_PESO_TON 20
#define MAX_ALTURA_M 4 

enum Direccion { IZQUIERDA = 0, DERECHA = 1, NINGUNO = 2 };
enum EstadoCoche { ESPERANDO, CRUZANDO, FINALIZADO, RECHAZADO }; 

// ============================================================================
// ESTRUCTURA DEL COCHE 
// ============================================================================
struct Coche {
    int id;
    Direccion direccion;
    EstadoCoche estado;
    chrono::time_point<chrono::system_clock> tiempo_llegada;
    chrono::time_point<chrono::system_clock> tiempo_inicio_cruce;
    chrono::time_point<chrono::system_clock> tiempo_salida;

    int peso_toneladas;         
    int altura_metros;          
    bool falla_mecanica_grave;  
};

// ============================================================================
// DECLARACIÓN ANTICIPADA Y AUXILIARES
// ============================================================================
string get_timestamp() {
    auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    tm tm_info;
    #ifdef _WIN32
        localtime_s(&tm_info, &now);
    #else
        localtime_r(&now, &tm_info);
    #endif
    stringstream ss;
    ss << put_time(&tm_info, "%H:%M:%S");
    return ss.str();
}

string direccion_str(Direccion dir) {
    switch(dir) {
        case IZQUIERDA: return "IZQUIERDA";
        case DERECHA: return "DERECHA";
        default: return "NINGUNO";
    }
}

void log_evento(const string& mensaje);

// ============================================================================
// CLASE MONITOR DEL PUENTE 
// ============================================================================
class MonitorPuente {
private:
    bool puente_bloqueado = false;      
    bool sensor_izq_ok = true;     
    bool sensor_der_ok = true;     
    bool barrera_izq_ok = true;    
    bool barrera_der_ok = true;    
    
    int coches_en_puente[2] = {0, 0};
    int coches_esperando[2] = {0, 0};
    int coches_seguidos[2] = {0, 0};
    Direccion turno = NINGUNO;
    
public:
    mutex mtx; 
    condition_variable cola_izquierda;
    condition_variable cola_derecha;
    condition_variable cv_apagado; // CV para despertar hilos que están durmiendo
    
    int total_cruzados = 0;
    int total_generados = 0;
    atomic<bool> sistema_activo;
    atomic<bool> sistema_en_pausa; 

    MonitorPuente() {
        sistema_activo = true;
        sistema_en_pausa = false;
        cerr << "[" << get_timestamp() << "] ✓ Monitor del puente inicializado correctamente" << endl;
    }

    void set_sensor_ok(Direccion dir, bool estado) {
        lock_guard<mutex> lock(mtx);
        if (dir == IZQUIERDA) {
            sensor_izq_ok = estado;
        } else {
            sensor_der_ok = estado;
        }
    }

    void set_barrera_ok(Direccion dir, bool estado) {
        lock_guard<mutex> lock(mtx);
        if (dir == IZQUIERDA) {
            barrera_izq_ok = estado;
        } else {
            barrera_der_ok = estado;
        }
    }

    void mostrar_estado() {
        lock_guard<mutex> lock(mtx);
        cout << "\n";
        cout << "╔════════════════════════════════════════════════════════════╗\n";
        cout << "║              ESTADO ACTUAL DEL PUENTE DUERO                ║\n";
        cout << "╠════════════════════════════════════════════════════════════╣\n";
        cout << "║ ESTADO GLOBAL:       " << (puente_bloqueado ? "🚨 BLOQUEADO/SUSPENDIDO (Intervención)" : "OPERATIVO") << " ║\n";
        cout << "║ Turno actual:        " << direccion_str(turno) << " ║\n";
        cout << "║ Componentes: IZQ(" << (sensor_izq_ok ? "Sensor OK" : "SENSOR FALLA") << " | " << (barrera_izq_ok ? "Barrera OK" : "BARRERA FALLA") << ") ║\n";
        cout << "║ Componentes: DER(" << (sensor_der_ok ? "Sensor OK" : "SENSOR FALLA") << " | " << (barrera_der_ok ? "Barrera OK" : "BARRERA FALLA") << ") ║\n";
        cout << "║ Coches en puente IZQ: " << coches_en_puente[IZQUIERDA] << " ║\n";
        cout << "║ Coches en puente DER: " << coches_en_puente[DERECHA] << " ║\n";
        cout << "║ Coches esperando IZQ: " << coches_esperando[IZQUIERDA] << " ║\n";
        cout << "║ Coches esperando DER: " << coches_esperando[DERECHA] << " ║\n";
        cout << "║ Total cruzados:       " << total_cruzados << " ║\n";
        cout << "╚════════════════════════════════════════════════════════════╝\n";
        cout << "\n";
    }

    void llega_cola(Coche* coche);
    void pasa_coche(Coche* coche);
    void sale_coche(Coche* coche);

    void iniciar_bloqueo_puente() {
        lock_guard<mutex> lock(mtx); 
        puente_bloqueado = true;
    }

    bool is_puente_bloqueado() {
        lock_guard<mutex> lock(mtx);
        return puente_bloqueado;
    }

    void reanudar_sistema() {
        unique_lock<mutex> lock(mtx);
        puente_bloqueado = false;
        sistema_en_pausa = false;

        sensor_izq_ok = true;
        sensor_der_ok = true;
        barrera_izq_ok = true;
        barrera_der_ok = true;
        
        cola_izquierda.notify_all();
        cola_derecha.notify_all();
    }
};

// ============================================================================
// VARIABLE GLOBAL DEL MONITOR (DEFINICIÓN REAL)
// ============================================================================
MonitorPuente monitor;

void log_evento(const string& mensaje) {
    if (monitor.sistema_en_pausa) {
        return; 
    }
    cerr << "[" << get_timestamp() << "] " << mensaje << endl;
}

// Implementación de las funciones de MonitorPuente 
void MonitorPuente::llega_cola(Coche* coche) {
    unique_lock<mutex> lock(mtx);
    coches_esperando[coche->direccion]++;
    log_evento("🚗 [SENSOR ENTRADA] Coche " + to_string(coche->id) + " detectado en cola " + direccion_str(coche->direccion) + " (esperando: " + to_string(coches_esperando[coche->direccion]) + ")");
}

void MonitorPuente::pasa_coche(Coche* coche) {
    unique_lock<mutex> lock(mtx);
    
    Direccion mi_dir = coche->direccion;
    Direccion otra_dir = (mi_dir == IZQUIERDA) ? DERECHA : IZQUIERDA;
    condition_variable& mi_cola = (mi_dir == IZQUIERDA) ? cola_izquierda : cola_derecha;
    
    log_evento("🛑 [ESTADO BLOQUEADO] Coche " + to_string(coche->id) + " entra en cola " + direccion_str(mi_dir));

    mi_cola.wait(lock, [&] {
        if (!monitor.sistema_activo) {
             return true; // Despierta si el sistema se está apagando
        }
        
        if (coche->estado == RECHAZADO) {
             return true; 
        }
        
        bool no_bloqueado = !puente_bloqueado;
        
        bool mi_sensor_ok = (mi_dir == IZQUIERDA) ? sensor_izq_ok : sensor_der_ok;
        bool mi_barrera_ok = (mi_dir == IZQUIERDA) ? barrera_izq_ok : barrera_der_ok;

        if (!mi_sensor_ok || !mi_barrera_ok) {
            return false; 
        }

        bool es_mi_turno = (turno == mi_dir || turno == NINGUNO);
        bool puente_libre = (coches_en_puente[otra_dir] == 0);
        bool hay_capacidad = (coches_en_puente[mi_dir] < MAX_COCHES_SIMULTANEOS);
        bool puede_pasar_seguido = true;
        if (coches_esperando[otra_dir] > 0) {
            puede_pasar_seguido = (coches_seguidos[mi_dir] < MAX_COCHES_SEGUIDOS);
        }

        // Condición 5: Revisión de Vehículo
        if (coche->peso_toneladas > MAX_PESO_TON || coche->altura_metros > MAX_ALTURA_M || coche->falla_mecanica_grave) {
            
            string razon = (coche->peso_toneladas > MAX_PESO_TON) ? "Límite de Peso Excedido" : 
                           (coche->altura_metros > MAX_ALTURA_M) ? "Límite de Altura Excedido" : "Falla Mecánica Grave";

            log_evento("⚠️ Coche " + to_string(coche->id) + " DETENIDO. Razón: " + razon);
            
            coche->estado = RECHAZADO;
            coches_esperando[mi_dir]--; 
            
            if (!puente_bloqueado) {
                puente_bloqueado = true; 
                sistema_en_pausa = true; 
                log_evento("🚨 Accidente/Infracción de vehículo fuerza la SUSPENSIÓN de todo el sistema.");
            }
            
            return true; 
        }

        return no_bloqueado && es_mi_turno && puente_libre && hay_capacidad && puede_pasar_seguido;
    });

    if (coche->estado == RECHAZADO || sistema_en_pausa || !sistema_activo) {
         return; 
    }

    log_evento("🟢 [TRANSICIÓN: LISTO -> EJECUCIÓN] Coche " + to_string(coche->id) + " obtiene permiso.");

    if (turno == NINGUNO) {
        turno = mi_dir;
    }
    
    // Si el coche salió del wait porque obtuvo permiso, se le resta de la cola y entra al puente.
    coches_esperando[mi_dir]--; 
    coches_en_puente[mi_dir]++;
    
    if (coches_esperando[otra_dir] > 0) {
        coches_seguidos[mi_dir]++;
    }
    
    coche->estado = CRUZANDO;
    coche->tiempo_inicio_cruce = chrono::system_clock::now();
    
    log_evento("🚦 [BARRERA ABRE] Coche " + to_string(coche->id) + " ENTRA desde " + direccion_str(mi_dir) + 
               " (en puente: " + to_string(coches_en_puente[mi_dir]) + ", seguidos: " + 
               to_string(coches_seguidos[mi_dir]) + "/" + to_string(MAX_COCHES_SEGUIDOS) + ")");
}

void MonitorPuente::sale_coche(Coche* coche) {
    unique_lock<mutex> lock(mtx);
    
    Direccion mi_dir = coche->direccion;
    Direccion otra_dir = (mi_dir == IZQUIERDA) ? DERECHA : IZQUIERDA;
    
    if (coche->estado == CRUZANDO) {
        coches_en_puente[mi_dir]--;
        total_cruzados++;
        coche->estado = FINALIZADO;
        log_evento("✅ [TRANSICIÓN: EJECUCIÓN -> TERMINADO] Coche " + to_string(coche->id) + " SALE");

        if (coches_en_puente[mi_dir] == 0) {
            if (coches_seguidos[mi_dir] >= MAX_COCHES_SEGUIDOS) {
                coches_seguidos[mi_dir] = 0;
            }
            if (coches_esperando[otra_dir] > 0) {
                turno = otra_dir;
                log_evento("🔄 Cambio de turno a " + direccion_str(otra_dir));
                (otra_dir == IZQUIERDA ? cola_izquierda : cola_derecha).notify_all();
            } else {
                turno = NINGUNO;
                coches_seguidos[mi_dir] = 0;
            }
        } else {
            (mi_dir == IZQUIERDA ? cola_izquierda : cola_derecha).notify_one();
        }
    } 
}

// ============================================================================
// FUNCIÓN DEL HILO COCHE
// ============================================================================

void tarea_coche(Coche* coche) {
    coche->tiempo_llegada = chrono::system_clock::now();
    coche->estado = ESPERANDO;
    
    monitor.llega_cola(coche);
    this_thread::sleep_for(chrono::milliseconds(rand() % 500));
    
    monitor.pasa_coche(coche);
    
    // CORRECCIÓN V12: Aseguramos que el hilo del coche salga si el sistema se está apagando
    if (!monitor.sistema_activo) {
        return; 
    }
    
    if (coche->estado == RECHAZADO) {
        log_evento("❌ Coche " + to_string(coche->id) + " RECHAZADO y desalojado. Finaliza hilo.");
    } else if (coche->estado == CRUZANDO) {
        this_thread::sleep_for(chrono::milliseconds(TIEMPO_CRUCE_MS));
        monitor.sale_coche(coche);
    } 
}

// ============================================================================
// GENERADOR DE COCHES 
// ============================================================================

void generador_coches(Direccion direccion) {
    vector<thread> hilos_coches;
    // La memoria de los coches debe ser persistente mientras los hilos corren
    vector<Coche> coches(TOTAL_COCHES_POR_LADO); 
    
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> prob_problema(1, 5);
    uniform_int_distribution<> peso_dist(10, 30);
    uniform_int_distribution<> altura_dist(3, 6);
    
    log_evento("🏁 Generador de coches " + direccion_str(direccion) + " iniciado");
    
    for (int i = 0; i < TOTAL_COCHES_POR_LADO; i++) {
        if (!monitor.sistema_activo) break;
        
        coches[i].id = (direccion * 100) + i + 1;
        coches[i].direccion = direccion;
        coches[i].estado = ESPERANDO;
        
        coches[i].peso_toneladas = peso_dist(gen);
        coches[i].altura_metros = altura_dist(gen);
        coches[i].falla_mecanica_grave = false;

        if (prob_problema(gen) == 1) { 
            int tipo_problema = rand() % 3;
            if (tipo_problema == 0) coches[i].peso_toneladas = MAX_PESO_TON + 1;
            else if (tipo_problema == 1) coches[i].altura_metros = MAX_ALTURA_M + 1;
            else coches[i].falla_mecanica_grave = true;

            log_evento("🚨 Generando Coche " + to_string(coches[i].id) + " con FALSA CAPACIDAD.");
        } else {
            coches[i].peso_toneladas = peso_dist(gen) % (MAX_PESO_TON - 1) + 1;
            coches[i].altura_metros = altura_dist(gen) % (MAX_ALTURA_M - 1) + 1;
            coches[i].peso_toneladas = max(1, coches[i].peso_toneladas); 
            coches[i].altura_metros = max(1, coches[i].altura_metros); 
        }

        hilos_coches.emplace_back(tarea_coche, &coches[i]);
        monitor.total_generados++;
        
        this_thread::sleep_for(chrono::milliseconds(500 + rand() % 1500));
    }
    
    for (auto& hilo : hilos_coches) {
        if (hilo.joinable()) {
            hilo.join();
        }
    }
    
    log_evento("🏁 Generador de coches " + direccion_str(direccion) + " finalizado");
}

// ============================================================================
// HILO DETECTOR DE FALLAS 
// ============================================================================

void detector_fallas() {
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> sleep_dist(10, 20); 
    uniform_int_distribution<> fail_dist(1, 4); 
    
    while (monitor.sistema_activo) {
        
        unique_lock<mutex> lock(monitor.mtx);
        // Usamos wait_for para que la CV de apagado pueda despertar este hilo
        monitor.cv_apagado.wait_for(lock, chrono::seconds(sleep_dist(gen)), [&]{ return !monitor.sistema_activo; });
        lock.unlock();

        if (!monitor.sistema_activo) break;
        
        if (fail_dist(gen) == 1 && !monitor.is_puente_bloqueado()) { 
            
            // Simulación de falla de componente (no implementada completamente, solo el bloqueo)
            monitor.iniciar_bloqueo_puente(); 
            
            monitor.sistema_en_pausa = true;
            log_evento("\n==============================================\n");
            log_evento("🚨 ¡ALARMA! Falla de Componente (Simulada).");
            log_evento("🛑 Sistema pausado. Se requiere intervención del Operario.");
            log_evento("==============================================\n");
            
            this_thread::sleep_for(chrono::seconds(1)); 
        }
    }
}

// ============================================================================
// HILO MONITOR DE ESTADO
// ============================================================================

void monitor_estado() {
    while (monitor.sistema_activo) {
        
        unique_lock<mutex> lock(monitor.mtx);
        // Usamos wait_for para que la CV de apagado pueda despertar este hilo
        monitor.cv_apagado.wait_for(lock, chrono::seconds(5), [&]{ return !monitor.sistema_activo; });
        lock.unlock();

        if (!monitor.sistema_activo) break;
        
        if (!monitor.sistema_en_pausa) {
             monitor.mostrar_estado();
        }
    }
}

// ============================================================================
// HILO DE INTERVENCIÓN DEL OPERARIO (Maneja CIN)
// ============================================================================

void tarea_intervencion() {
    int opcion = 0;
    
    while (monitor.sistema_activo) {
        
        if (monitor.sistema_en_pausa) {
            
            // 1. Mostrar menú 
            cout << "\n\n==============================================\n";
            cout << "### INTERVENCIÓN REQUERIDA (ESTADO SUSPENDIDO) ###\n";
            cout << "==============================================\n";
            monitor.mostrar_estado();
            cout << "Falla detectada. El sistema está en estado SUSPENDIDO.\n";
            cout << "1. Resolver y Reanudar el sistema.\n";
            cout << "2. Ignorar y esperar.\n";
            cout << "Seleccione opción (1 o 2): "; 
            
            // 2. Intentamos leer la opción.
            if (!(cin >> opcion)) {
                opcion = 1; 
                cin.clear();
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
            } else {
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
            }
            
            // 3. Aseguramos que la opción sea válida
            if (opcion != 1 && opcion != 2) {
                opcion = 1; 
            }

            // 4. Ejecutamos la acción.
            if (opcion == 1) {
                cout << "-> Seleccionada Opción 1: REANUDAR\n";
                monitor.reanudar_sistema();
            } else { 
                cout << "-> Seleccionada Opción 2: IGNORAR\n";
                this_thread::sleep_for(chrono::seconds(2)); 
            }
            opcion = 0; 

        } else {
            // En el estado operativo, solo esperamos un tiempo muy corto.
            this_thread::sleep_for(chrono::milliseconds(100)); 
        }
    }
    cerr << "[" << get_timestamp() << "] 🛑 Hilo de Intervención finalizado." << endl;
}


// ============================================================================
// FUNCIÓN PRINCIPAL (JEFE DE OPERACIONES)
// ============================================================================

int main() {
    srand(time(NULL)); 
    
    cout << "\n";
    cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    cout << "║         SISTEMA DE CONTROL DEL PUENTE DUERO (C++)            ║\n";
    cout << "║         Fallas y Modelo de 7 Estados (V13: ESPERA ACTIVA)    ║\n";
    cout << "╚═══════════════════════════════════════════════════════════════╝\n";
    cout << "\n";
    
    thread generador_izq(generador_coches, IZQUIERDA);
    thread generador_der(generador_coches, DERECHA);
    thread monitor_thread(monitor_estado);
    thread fallas_thread(detector_fallas);
    thread intervencion_thread(tarea_intervencion); 
    
    // Desacoplar el hilo de intervención. Evita el bloqueo de 'join()' con 'cin'.
    intervencion_thread.detach(); 
    
    // Esperamos a que los generadores terminen.
    if (generador_izq.joinable()) generador_izq.join();
    if (generador_der.joinable()) generador_der.join();
    
    // CORRECCIÓN V13: Esperar activamente si el sistema sigue en pausa.
    if (monitor.sistema_en_pausa) {
        cerr << "[" << get_timestamp() << "] ⏳ Generadores terminados, esperando intervención para apagar..." << endl;
        // Esperamos en un bucle simple hasta que el operario reanude el sistema.
        while (monitor.sistema_en_pausa) {
            this_thread::sleep_for(chrono::milliseconds(200));
        }
        cerr << "[" << get_timestamp() << "] ✓ Intervención completada. Procediendo al apagado final." << endl;
    }
    
    // Una vez que los generadores han terminado y NO hay intervención pendiente, cerramos el sistema.
    
    // 1. Desactivar el sistema globalmente
    monitor.sistema_activo = false; 

    // 2. Despertar todos los hilos durmientes (monitor, fallas, y coches en espera)
    monitor.cola_izquierda.notify_all();
    monitor.cola_derecha.notify_all();
    monitor.cv_apagado.notify_all(); 

    // 3. Esperar a que el resto de hilos terminen.
    if (monitor_thread.joinable()) monitor_thread.join();
    if (fallas_thread.joinable()) fallas_thread.join();
    // Intervención ya está detached, terminará tan pronto como 'sistema_activo' sea false.
    
    // --- Estadísticas Finales ---
    cout << "\n";
    cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    cout << "║                    ESTADÍSTICAS FINALES                       ║\n";
    cout << "╠═══════════════════════════════════════════════════════════════╣\n";
    cout << "║ Total coches generados:  " << monitor.total_generados << " ║\n";
    cout << "║ Total coches cruzados:   " << monitor.total_cruzados << " ║\n";
    cout << "║ Coches retenidos/desalojados: " << monitor.total_generados - monitor.total_cruzados << " ║\n";
    cout << "╚═══════════════════════════════════════════════════════════════╝\n";
    cout << "\n";
    
    cerr << "[" << get_timestamp() << "] ✓ Sistema finalizado correctamente" << endl;
    
    return 0;
}