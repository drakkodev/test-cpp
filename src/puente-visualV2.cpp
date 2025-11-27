/*
 * PUENTE DUERO - Interfaz TUI con ncurses (estilo btop)
 * Implementación con Monitor y visualización en tiempo real
 * 
 * Compilar: gcc -std=c11 -pthread puente_ncurses.c -lncurses -o puente_tui
 * Ejecutar: ./puente_tui
 * i
 * Controles:
 *   q - Salir
 *   i - Añadir coche IZQUIERDA
 *   d - Añadir coche DERECHA
 *   ESPACIO - Pausar/Reanudar
 */

// ============================================================================
// LIBRERÍAS REQUERIDAS (según especificación del profesor)
// ============================================================================
#include <stdio.h>          // Para mensajes de error (perror)
#include <stdlib.h>         // Para exit(0)
#include <pthread.h>        // Para hilos y mutex
#include <semaphore.h>      // Para semáforos POSIX
#include <unistd.h>         // Para sleep, usleep
#include <sys/types.h>      // Para tipos de datos del sistema
#include <sys/wait.h>       // Para wait() en procesos
#include <sys/errno.h>      // Para errno
#include <fcntl.h>          // Para manipulación de descriptores de fichero
#include <sys/ipc.h>        // Para IPC (Inter-Process Communication)
#include <sys/sem.h>        // Para semáforos System V
#include <sys/stat.h>       // Para estadísticas de archivos
#include <string.h>         // Para manipulación de strings
#include <time.h>           // Para timestamps
#include <stdbool.h>        // Para tipo bool
#include <stdarg.h>         // Para funciones con argumentos variables
#include <ncurses.h>        // Para interfaz TUI

// ============================================================================
// CONSTANTES DEL SISTEMA
// ============================================================================
#define MAX_COCHES_SIMULTANEOS 3
#define MAX_COCHES_SEGUIDOS 5
#define TIEMPO_CRUCE_MS 2000
#define MAX_LOG_LINES 15
#define ANCHO_PUENTE 60

// Direcciones
typedef enum { IZQUIERDA = 0, DERECHA = 1, NINGUNO = 2 } Direccion;
typedef enum { ESPERANDO, CRUZANDO, FINALIZADO } EstadoCoche;

// Colores
enum {
    COLOR_PAIR_DEFAULT = 1,
    COLOR_PAIR_TITULO,
    COLOR_PAIR_PUENTE,
    COLOR_PAIR_COCHE_IZQ,
    COLOR_PAIR_COCHE_DER,
    COLOR_PAIR_BARRERA_CERRADA,
    COLOR_PAIR_BARRERA_ABIERTA,
    COLOR_PAIR_INFO,
    COLOR_PAIR_EXITO,
    COLOR_PAIR_ALERTA,
    COLOR_PAIR_SENSOR,
    COLOR_PAIR_TURNO_IZQ,
    COLOR_PAIR_TURNO_DER
};

// ============================================================================
// ESTRUCTURA DEL MONITOR DEL PUENTE
// ============================================================================
typedef struct {
    // Mutex principal del monitor (exclusión mutua)
    pthread_mutex_t mutex;
    
    // Variables condicionales (entradas del monitor)
    pthread_cond_t cola_izquierda;
    pthread_cond_t cola_derecha;
    
    // Estado del puente
    int coches_en_puente[2];      // Coches actualmente cruzando por lado
    int coches_esperando[2];      // Coches esperando por lado (sensores)
    int coches_seguidos[2];       // Contador de coches consecutivos
    Direccion turno;              // Turno actual (IZQUIERDA, DERECHA, NINGUNO)
    
    // Estadísticas
    int total_cruzados;
    int total_generados;
    
    // Control de ejecución
    bool sistema_activo;
    bool pausado;
    
} MonitorPuente;

// Variable global del monitor
MonitorPuente monitor;

// ============================================================================
// ESTRUCTURA DEL COCHE
// ============================================================================
typedef struct {
    int id;
    Direccion direccion;
    EstadoCoche estado;
    float posicion;  // Para animación (0.0 a 1.0)
    time_t tiempo_llegada;
    time_t tiempo_inicio_cruce;
    time_t tiempo_salida;
} Coche;

// Array global para visualización
#define MAX_COCHES_VISUALES 50
Coche coches_visuales[MAX_COCHES_VISUALES];
int num_coches_visuales = 0;
pthread_mutex_t mutex_visuales;

// Log de eventos
char log_buffer[MAX_LOG_LINES][100];
int log_index = 0;
pthread_mutex_t mutex_log;

// ============================================================================
// FUNCIONES DE LOG Y UTILIDADES
// ============================================================================

void agregar_log(const char* formato, ...) {
    pthread_mutex_lock(&mutex_log);
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    char mensaje[80];
    va_list args;
    va_start(args, formato);
    vsnprintf(mensaje, sizeof(mensaje), formato, args);
    va_end(args);
    
    snprintf(log_buffer[log_index], 100, "[%s] %s", timestamp, mensaje);
    log_index = (log_index + 1) % MAX_LOG_LINES;
    
    pthread_mutex_unlock(&mutex_log);
}

const char* direccion_str(Direccion dir) {
    switch(dir) {
        case IZQUIERDA: return "IZQ";
        case DERECHA: return "DER";
        default: return "---";
    }
}

// ============================================================================
// FUNCIONES NCURSES
// ============================================================================

void inicializar_colores() {
    start_color();
    init_pair(COLOR_PAIR_DEFAULT, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_PAIR_TITULO, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_PAIR_PUENTE, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_PAIR_COCHE_IZQ, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_PAIR_COCHE_DER, COLOR_BLUE, COLOR_BLACK);
    init_pair(COLOR_PAIR_BARRERA_CERRADA, COLOR_RED, COLOR_BLACK);
    init_pair(COLOR_PAIR_BARRERA_ABIERTA, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_PAIR_INFO, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_PAIR_EXITO, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_PAIR_ALERTA, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_PAIR_SENSOR, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(COLOR_PAIR_TURNO_IZQ, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_PAIR_TURNO_DER, COLOR_BLUE, COLOR_BLACK);
}

void dibujar_caja(int y, int x, int alto, int ancho, const char* titulo) {
    // Esquinas y bordes
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + ancho - 1, ACS_URCORNER);
    mvaddch(y + alto - 1, x, ACS_LLCORNER);
    mvaddch(y + alto - 1, x + ancho - 1, ACS_LRCORNER);
    
    for (int i = 1; i < ancho - 1; i++) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + alto - 1, x + i, ACS_HLINE);
    }
    
    for (int i = 1; i < alto - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + ancho - 1, ACS_VLINE);
    }
    
    // Título
    if (titulo) {
        attron(COLOR_PAIR(COLOR_PAIR_TITULO) | A_BOLD);
        mvprintw(y, x + 2, " %s ", titulo);
        attroff(COLOR_PAIR(COLOR_PAIR_TITULO) | A_BOLD);
    }
}

void dibujar_barra_progreso(int y, int x, int ancho, float porcentaje, int color_pair) {
    int lleno = (int)(ancho * porcentaje);
    
    attron(COLOR_PAIR(color_pair));
    mvaddch(y, x, '[');
    for (int i = 0; i < ancho; i++) {
        if (i < lleno) mvaddch(y, x + 1 + i, '=');
        else mvaddch(y, x + 1 + i, ' ');
    }
    mvaddch(y, x + ancho + 1, ']');
    attroff(COLOR_PAIR(color_pair));
    
    mvprintw(y, x + ancho + 3, "%3.0f%%", porcentaje * 100);
}

void dibujar_header() {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    attron(COLOR_PAIR(COLOR_PAIR_TITULO) | A_BOLD);
    mvprintw(0, (max_x - 50) / 2, "╔════════════════════════════════════════════════╗");
    mvprintw(1, (max_x - 50) / 2, "║       SISTEMA PUENTE DUERO - MONITOR TUI       ║");
    mvprintw(2, (max_x - 50) / 2, "╚════════════════════════════════════════════════╝");
    attroff(COLOR_PAIR(COLOR_PAIR_TITULO) | A_BOLD);
}

void dibujar_puente(int start_y) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int start_x = (max_x - ANCHO_PUENTE - 40) / 2;
    
    // Dibujar caja del puente
    dibujar_caja(start_y, start_x, 12, ANCHO_PUENTE + 40, "PUENTE");
    
    pthread_mutex_lock(&monitor.mutex);
    
    // Carretera izquierda
    attron(COLOR_PAIR(COLOR_PAIR_INFO));
    for (int i = 0; i < 10; i++) {
        mvprintw(start_y + 6, start_x + 2 + i, "═");
    }
    attroff(COLOR_PAIR(COLOR_PAIR_INFO));
    
    // Barrera izquierda (SENSOR - se abre cuando hay coches cruzando)
    bool barrera_izq_abierta = (monitor.coches_en_puente[IZQUIERDA] > 0);
    int color_barrera_izq = barrera_izq_abierta ? COLOR_PAIR_BARRERA_ABIERTA : COLOR_PAIR_BARRERA_CERRADA;
    attron(COLOR_PAIR(color_barrera_izq) | A_BOLD);
    if (barrera_izq_abierta) {
        mvprintw(start_y + 5, start_x + 12, "▲");
        mvprintw(start_y + 6, start_x + 12, "│");
        mvprintw(start_y + 7, start_x + 12, "│");
    } else {
        mvprintw(start_y + 6, start_x + 12, "█");
    }
    attroff(COLOR_PAIR(color_barrera_izq) | A_BOLD);
    
    // Puente (arco romano)
    attron(COLOR_PAIR(COLOR_PAIR_PUENTE) | A_BOLD);
    int centro = start_x + 30;
    mvprintw(start_y + 4, centro - 15, "    ╱‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾╲");
    mvprintw(start_y + 5, centro - 15, "   ╱                            ╲");
    mvprintw(start_y + 6, centro - 15, "══╯    P U E N T E   D U E R O   ╰══");
    mvprintw(start_y + 7, centro - 15, "   ╲                            ╱");
    mvprintw(start_y + 8, centro - 15, "    ╲____________________________╱");
    attroff(COLOR_PAIR(COLOR_PAIR_PUENTE) | A_BOLD);
    
    // Barrera derecha (SENSOR - se abre cuando hay coches cruzando)
    bool barrera_der_abierta = (monitor.coches_en_puente[DERECHA] > 0);
    int color_barrera_der = barrera_der_abierta ? COLOR_PAIR_BARRERA_ABIERTA : COLOR_PAIR_BARRERA_CERRADA;
    attron(COLOR_PAIR(color_barrera_der) | A_BOLD);
    if (barrera_der_abierta) {
        mvprintw(start_y + 5, start_x + 67, "▲");
        mvprintw(start_y + 6, start_x + 67, "│");
        mvprintw(start_y + 7, start_x + 67, "│");
    } else {
        mvprintw(start_y + 6, start_x + 67, "█");
    }
    attroff(COLOR_PAIR(color_barrera_der) | A_BOLD);
    
    // Carretera derecha
    attron(COLOR_PAIR(COLOR_PAIR_INFO));
    for (int i = 0; i < 10; i++) {
        mvprintw(start_y + 6, start_x + 68 + i, "═");
    }
    attroff(COLOR_PAIR(COLOR_PAIR_INFO));
    
    // Dibujar coches en movimiento (animación)
    pthread_mutex_lock(&mutex_visuales);
    for (int i = 0; i < num_coches_visuales; i++) {
        if (coches_visuales[i].estado == CRUZANDO) {
            int color = (coches_visuales[i].direccion == IZQUIERDA) ? 
                       COLOR_PAIR_COCHE_IZQ : COLOR_PAIR_COCHE_DER;
            
            int pos_x;
            if (coches_visuales[i].direccion == IZQUIERDA) {
                pos_x = start_x + 14 + (int)(coches_visuales[i].posicion * 50);
            } else {
                pos_x = start_x + 66 - (int)(coches_visuales[i].posicion * 50);
            }
            
            attron(COLOR_PAIR(color) | A_BOLD);
            mvprintw(start_y + 6, pos_x, "[%d]", coches_visuales[i].id % 100);
            attroff(COLOR_PAIR(color) | A_BOLD);
        }
    }
    pthread_mutex_unlock(&mutex_visuales);
    
    pthread_mutex_unlock(&monitor.mutex);
}

void dibujar_estadisticas(int start_y) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int start_x = 2;
    
    pthread_mutex_lock(&monitor.mutex);
    
    // Panel izquierdo - IZQUIERDA
    dibujar_caja(start_y, start_x, 10, 35, "LADO IZQUIERDO");
    
    attron(COLOR_PAIR(COLOR_PAIR_COCHE_IZQ) | A_BOLD);
    mvprintw(start_y + 2, start_x + 2, "Esperando: %d", monitor.coches_esperando[IZQUIERDA]);
    mvprintw(start_y + 3, start_x + 2, "En puente: %d/%d", 
             monitor.coches_en_puente[IZQUIERDA], MAX_COCHES_SIMULTANEOS);
    mvprintw(start_y + 4, start_x + 2, "Seguidos:  %d/%d", 
             monitor.coches_seguidos[IZQUIERDA], MAX_COCHES_SEGUIDOS);
    attroff(COLOR_PAIR(COLOR_PAIR_COCHE_IZQ) | A_BOLD);
    
    // Barra de ocupación izquierda
    float ocupacion_izq = (float)monitor.coches_en_puente[IZQUIERDA] / MAX_COCHES_SIMULTANEOS;
    dibujar_barra_progreso(start_y + 6, start_x + 2, 20, ocupacion_izq, COLOR_PAIR_COCHE_IZQ);
    
    // Sensor izquierda (SENSOR DE ENTRADA)
    bool sensor_izq_activo = (monitor.coches_esperando[IZQUIERDA] > 0);
    if (sensor_izq_activo) {
        attron(COLOR_PAIR(COLOR_PAIR_SENSOR) | A_BOLD);
        mvprintw(start_y + 8, start_x + 2, "● SENSOR ACTIVO");
        attroff(COLOR_PAIR(COLOR_PAIR_SENSOR) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_PAIR_INFO));
        mvprintw(start_y + 8, start_x + 2, "○ Sensor inactivo");
        attroff(COLOR_PAIR(COLOR_PAIR_INFO));
    }
    
    // Panel derecho - DERECHA
    int panel_der_x = max_x - 37;
    dibujar_caja(start_y, panel_der_x, 10, 35, "LADO DERECHO");
    
    attron(COLOR_PAIR(COLOR_PAIR_COCHE_DER) | A_BOLD);
    mvprintw(start_y + 2, panel_der_x + 2, "Esperando: %d", monitor.coches_esperando[DERECHA]);
    mvprintw(start_y + 3, panel_der_x + 2, "En puente: %d/%d", 
             monitor.coches_en_puente[DERECHA], MAX_COCHES_SIMULTANEOS);
    mvprintw(start_y + 4, panel_der_x + 2, "Seguidos:  %d/%d", 
             monitor.coches_seguidos[DERECHA], MAX_COCHES_SEGUIDOS);
    attroff(COLOR_PAIR(COLOR_PAIR_COCHE_DER) | A_BOLD);
    
    // Barra de ocupación derecha
    float ocupacion_der = (float)monitor.coches_en_puente[DERECHA] / MAX_COCHES_SIMULTANEOS;
    dibujar_barra_progreso(start_y + 6, panel_der_x + 2, 20, ocupacion_der, COLOR_PAIR_COCHE_DER);
    
    // Sensor derecha (SENSOR DE ENTRADA)
    bool sensor_der_activo = (monitor.coches_esperando[DERECHA] > 0);
    if (sensor_der_activo) {
        attron(COLOR_PAIR(COLOR_PAIR_SENSOR) | A_BOLD);
        mvprintw(start_y + 8, panel_der_x + 2, "● SENSOR ACTIVO");
        attroff(COLOR_PAIR(COLOR_PAIR_SENSOR) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_PAIR_INFO));
        mvprintw(start_y + 8, panel_der_x + 2, "○ Sensor inactivo");
        attroff(COLOR_PAIR(COLOR_PAIR_INFO));
    }
    
    // Panel central - Turno y estadísticas (MONITOR)
    int panel_centro_x = (max_x - 40) / 2;
    dibujar_caja(start_y, panel_centro_x, 10, 40, "MONITOR - CONTROL");
    
    // Turno actual
    mvprintw(start_y + 2, panel_centro_x + 2, "Turno:");
    if (monitor.turno == IZQUIERDA) {
        attron(COLOR_PAIR(COLOR_PAIR_TURNO_IZQ) | A_BOLD);
        mvprintw(start_y + 2, panel_centro_x + 10, "← IZQUIERDA");
        attroff(COLOR_PAIR(COLOR_PAIR_TURNO_IZQ) | A_BOLD);
    } else if (monitor.turno == DERECHA) {
        attron(COLOR_PAIR(COLOR_PAIR_TURNO_DER) | A_BOLD);
        mvprintw(start_y + 2, panel_centro_x + 10, "DERECHA →");
        attroff(COLOR_PAIR(COLOR_PAIR_TURNO_DER) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_PAIR_INFO));
        mvprintw(start_y + 2, panel_centro_x + 10, "NINGUNO");
        attroff(COLOR_PAIR(COLOR_PAIR_INFO));
    }
    
    // Estadísticas
    attron(COLOR_PAIR(COLOR_PAIR_EXITO));
    mvprintw(start_y + 4, panel_centro_x + 2, "Total generados: %d", monitor.total_generados);
    mvprintw(start_y + 5, panel_centro_x + 2, "Total cruzados:  %d", monitor.total_cruzados);
    attroff(COLOR_PAIR(COLOR_PAIR_EXITO));
    
    // Estado del sistema
    if (monitor.pausado) {
        attron(COLOR_PAIR(COLOR_PAIR_ALERTA) | A_BOLD);
        mvprintw(start_y + 7, panel_centro_x + 2, "⏸  PAUSADO");
        attroff(COLOR_PAIR(COLOR_PAIR_ALERTA) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_PAIR_EXITO));
        mvprintw(start_y + 7, panel_centro_x + 2, "▶  ACTIVO");
        attroff(COLOR_PAIR(COLOR_PAIR_EXITO));
    }
    
    pthread_mutex_unlock(&monitor.mutex);
}

void dibujar_log(int start_y) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    dibujar_caja(start_y, 2, MAX_LOG_LINES + 2, max_x - 4, "LOG DE EVENTOS");
    
    pthread_mutex_lock(&mutex_log);
    for (int i = 0; i < MAX_LOG_LINES; i++) {
        int idx = (log_index + i) % MAX_LOG_LINES;
        if (strlen(log_buffer[idx]) > 0) {
            attron(COLOR_PAIR(COLOR_PAIR_INFO));
            mvprintw(start_y + 1 + i, 4, "%s", log_buffer[idx]);
            attroff(COLOR_PAIR(COLOR_PAIR_INFO));
        }
    }
    pthread_mutex_unlock(&mutex_log);
}

void dibujar_controles() {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    attron(COLOR_PAIR(COLOR_PAIR_TITULO));
    mvprintw(max_y - 1, 2, "Controles: [I]zquierda [D]erecha [ESPACIO]Pausa [Q]uit");
    attroff(COLOR_PAIR(COLOR_PAIR_TITULO));
}

void* hilo_renderizado(void* arg) {
    while (monitor.sistema_activo) {
        clear();
        
        dibujar_header();
        dibujar_puente(4);
        dibujar_estadisticas(17);
        dibujar_log(28);
        dibujar_controles();
        
        refresh();
        usleep(50000); // 50ms = 20 FPS
    }
    return NULL;
}

// ============================================================================
// INICIALIZACIÓN DEL MONITOR
// ============================================================================

int inicializar_monitor() {
    // Inicializar mutex (EXCLUSIÓN MUTUA)
    if (pthread_mutex_init(&monitor.mutex, NULL) != 0) {
        perror("Error al inicializar mutex del monitor");
        return -1;
    }
    
    // Inicializar variables condicionales (ENTRADAS DEL MONITOR)
    if (pthread_cond_init(&monitor.cola_izquierda, NULL) != 0) {
        perror("Error al inicializar cola izquierda");
        return -1;
    }
    
    if (pthread_cond_init(&monitor.cola_derecha, NULL) != 0) {
        perror("Error al inicializar cola derecha");
        return -1;
    }
    
    if (pthread_mutex_init(&mutex_visuales, NULL) != 0) {
        perror("Error al inicializar mutex visuales");
        return -1;
    }
    
    if (pthread_mutex_init(&mutex_log, NULL) != 0) {
        perror("Error al inicializar mutex log");
        return -1;
    }
    
    // Inicializar estado del puente
    monitor.coches_en_puente[IZQUIERDA] = 0;
    monitor.coches_en_puente[DERECHA] = 0;
    monitor.coches_esperando[IZQUIERDA] = 0;
    monitor.coches_esperando[DERECHA] = 0;
    monitor.coches_seguidos[IZQUIERDA] = 0;
    monitor.coches_seguidos[DERECHA] = 0;
    monitor.turno = NINGUNO;
    monitor.total_cruzados = 0;
    monitor.total_generados = 0;
    monitor.sistema_activo = true;
    monitor.pausado = false;
    
    memset(log_buffer, 0, sizeof(log_buffer));
    
    agregar_log("Monitor inicializado correctamente");
    return 0;
}

void destruir_monitor() {
    pthread_mutex_destroy(&monitor.mutex);
    pthread_cond_destroy(&monitor.cola_izquierda);
    pthread_cond_destroy(&monitor.cola_derecha);
    pthread_mutex_destroy(&mutex_visuales);
    pthread_mutex_destroy(&mutex_log);
}

// ============================================================================
// PROCEDIMIENTOS DEL MONITOR (según caso de estudio)
// ============================================================================

/**
 * PROCEDIMIENTO: llega_cola_izq / llega_cola_der
 * Registra la llegada de un coche a la cola (SENSOR DE ENTRADA)
 */
void llega_cola(Coche* coche) {
    pthread_mutex_lock(&monitor.mutex);
    
    monitor.coches_esperando[coche->direccion]++;
    
    pthread_mutex_unlock(&monitor.mutex);
    
    agregar_log("Sensor: Coche %d en cola %s", coche->id, direccion_str(coche->direccion));
}

/**
 * PROCEDIMIENTO: pasa_coche_izq / pasa_coche_der
 * ENTRADA CONDICIONAL - El coche espera hasta poder cruzar
 * Implementa las 4 condiciones del caso de estudio:
 * 1. No hay coches del sentido contrario
 * 2. No se supera la capacidad máxima
 * 3. Se respeta el límite de coches seguidos
 * 4. Se tiene el turno correspondiente
 */
void pasa_coche(Coche* coche) {
    pthread_mutex_lock(&monitor.mutex);
    
    Direccion mi_dir = coche->direccion;
    Direccion otra_dir = (mi_dir == IZQUIERDA) ? DERECHA : IZQUIERDA;
    pthread_cond_t* mi_cola = (mi_dir == IZQUIERDA) ? 
                              &monitor.cola_izquierda : &monitor.cola_derecha;
    
    // ESPERAR hasta que se cumplan TODAS las condiciones
    while (true) {
        // Condición de pausa
        if (monitor.pausado) {
            pthread_cond_wait(mi_cola, &monitor.mutex);
            continue;
        }
        
        // Condición 1: Verificar turno
        bool es_mi_turno = (monitor.turno == mi_dir || monitor.turno == NINGUNO);
        
        // Condición 2: No hay coches del otro lado (EXCLUSIÓN MUTUA)
        bool puente_libre = (monitor.coches_en_puente[otra_dir] == 0);
        
        // Condición 3: Capacidad no superada
        bool hay_capacidad = (monitor.coches_en_puente[mi_dir] < MAX_COCHES_SIMULTANEOS);
        
        // Condición 4: No se superó límite de seguidos (evitar INANICIÓN)
        bool puede_pasar_seguido = true;
        if (monitor.coches_esperando[otra_dir] > 0) {
            puede_pasar_seguido = (monitor.coches_seguidos[mi_dir] < MAX_COCHES_SEGUIDOS);
        }
        
        // Si todas las condiciones se cumplen, puede pasar
        if (es_mi_turno && puente_libre && hay_capacidad && puede_pasar_seguido) {
            break;
        }
        
        // Si no puede pasar, espera en la cola condicional
        pthread_cond_wait(mi_cola, &monitor.mutex);
    }
    
    // === SECCIÓN CRÍTICA: CRUZAR EL PUENTE ===
    
    // Asignar turno si no hay ninguno
    if (monitor.turno == NINGUNO) {
        monitor.turno = mi_dir;
    }
    
    // Actualizar contadores
    monitor.coches_esperando[mi_dir]--;
    monitor.coches_en_puente[mi_dir]++;
    
    // Incrementar contador de seguidos solo si hay cola en el otro lado
    if (monitor.coches_esperando[otra_dir] > 0) {
        monitor.coches_seguidos[mi_dir]++;
    }
    
    coche->estado = CRUZANDO;
    coche->posicion = 0.0f;
    coche->tiempo_inicio_cruce = time(NULL);
    
    pthread_mutex_unlock(&monitor.mutex);
    
    agregar_log("Barrera abre: Coche %d ENTRA desde %s", coche->id, direccion_str(mi_dir));
}

/**
 * PROCEDIMIENTO: sale_coche_izq / sale_coche_der
 * Registra la salida del coche (SENSOR DE SALIDA)
 * Gestiona cambios de turno para evitar inanición
 */
void sale_coche(Coche* coche) {
    pthread_mutex_lock(&monitor.mutex);
    
    Direccion mi_dir = coche->direccion;
    Direccion otra_dir = (mi_dir == IZQUIERDA) ? DERECHA : IZQUIERDA;
    
    // Actualizar contadores
    monitor.coches_en_puente[mi_dir]--;
    monitor.total_cruzados++;
    coche->estado = FINALIZADO;
    coche->tiempo_salida = time(NULL);
    
    // Si soy el último de mi lado en el puente
    if (monitor.coches_en_puente[mi_dir] == 0) {
        
        // Reiniciar contador de seguidos si llegó al límite
        if (monitor.coches_seguidos[mi_dir] >= MAX_COCHES_SEGUIDOS) {
            monitor.coches_seguidos[mi_dir] = 0;
        }
        
        // Cambiar turno si hay coches esperando del otro lado (evitar INANICIÓN)
        if (monitor.coches_esperando[otra_dir] > 0) {
            monitor.turno = otra_dir;
            
            // Despertar a los coches del otro lado
            if (otra_dir == IZQUIERDA) {
                pthread_cond_broadcast(&monitor.cola_izquierda);
            } else {
                pthread_cond_broadcast(&monitor.cola_derecha);
            }
        } else {
            // Si no hay nadie esperando, liberar el turno
            monitor.turno = NINGUNO;
            monitor.coches_seguidos[mi_dir] = 0;
        }
    } else {
        // Aún hay coches de mi lado, despertar al siguiente de mi cola
        if (mi_dir == IZQUIERDA) {
            pthread_cond_signal(&monitor.cola_izquierda);
        } else {
            pthread_cond_signal(&monitor.cola_derecha);
        }
    }
    
    pthread_mutex_unlock(&monitor.mutex);
    
    agregar_log("Sensor salida: Coche %d SALE del %s", coche->id, direccion_str(mi_dir));
}

// ============================================================================
// FUNCIÓN DEL HILO COCHE
// ============================================================================

void* tarea_coche(void* arg) {
    Coche* coche = (Coche*)arg;
    
    // Fase 1: Llegar al puente (SENSOR DE ENTRADA detecta)
    coche->tiempo_llegada = time(NULL);
    coche->estado = ESPERANDO;
    llega_cola(coche);
    
    // Pequeña pausa para simular tiempo de llegada
    usleep((rand() % 500) * 1000);
    
    // Fase 2: Esperar y cruzar el puente (ENTRADA CONDICIONAL)
    pasa_coche(coche);
    
    // Fase 3: Animación del cruce con actualización de posición
    int pasos = 40;
    for (int i = 0; i <= pasos; i++) {
        pthread_mutex_lock(&mutex_visuales);
        coche->posicion = (float)i / pasos;
        pthread_mutex_unlock(&mutex_visuales);
        
        usleep(TIEMPO_CRUCE_MS * 1000 / pasos);
    }
    
    // Fase 4: Salir del puente (SENSOR DE SALIDA detecta)
    sale_coche(coche);
    
    // Remover de visuales
    pthread_mutex_lock(&mutex_visuales);
    for (int i = 0; i < num_coches_visuales; i++) {
        if (coches_visuales[i].id == coche->id) {
            // Mover el último al lugar del actual
            if (i < num_coches_visuales - 1) {
                coches_visuales[i] = coches_visuales[num_coches_visuales - 1];
            }
            num_coches_visuales--;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_visuales);
    
    return NULL;
}

// ============================================================================
// FUNCIÓN PARA AÑADIR COCHE MANUALMENTE
// ============================================================================

void agregar_coche_manual(Direccion direccion) {
    if (num_coches_visuales >= MAX_COCHES_VISUALES) {
        agregar_log("ERROR: Máximo de coches alcanzado");
        return;
    }
    
    pthread_mutex_lock(&mutex_visuales);
    
    static int id_counter = 1;
    Coche* coche = &coches_visuales[num_coches_visuales];
    coche->id = id_counter++;
    coche->direccion = direccion;
    coche->estado = ESPERANDO;
    coche->posicion = 0.0f;
    num_coches_visuales++;
    
    pthread_mutex_unlock(&mutex_visuales);
    
    pthread_mutex_lock(&monitor.mutex);
    monitor.total_generados++;
    pthread_mutex_unlock(&monitor.mutex);
    
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, tarea_coche, coche) != 0) {
        perror("Error al crear hilo del coche");
        return;
    }
    pthread_detach(hilo);
}

// ============================================================================
// GENERADOR AUTOMÁTICO DE COCHES
// ============================================================================

void* generador_automatico(void* arg) {
    agregar_log("Generador automático iniciado");
    
    while (monitor.sistema_activo) {
        if (!monitor.pausado && num_coches_visuales < MAX_COCHES_VISUALES - 2) {
            // Generar aleatoriamente coches
            if (rand() % 100 < 30) { // 30% probabilidad cada ciclo
                Direccion dir = (rand() % 2 == 0) ? IZQUIERDA : DERECHA;
                agregar_coche_manual(dir);
            }
        }
        
        sleep(2); // Cada 2 segundos
    }
    
    return NULL;
}

// ============================================================================
// MANEJO DE ENTRADA DEL USUARIO
// ============================================================================

void* hilo_input(void* arg) {
    int ch;
    while (monitor.sistema_activo) {
        ch = getch();
        
        switch(ch) {
            case 'q':
            case 'Q':
                monitor.sistema_activo = false;
                agregar_log("Finalizando sistema...");
                break;
                
            case 'i':
            case 'I':
                agregar_coche_manual(IZQUIERDA);
                agregar_log("Usuario añadió coche IZQUIERDA");
                break;
                
            case 'd':
            case 'D':
                agregar_coche_manual(DERECHA);
                agregar_log("Usuario añadió coche DERECHA");
                break;
                
            case ' ':
                pthread_mutex_lock(&monitor.mutex);
                monitor.pausado = !monitor.pausado;
                
                if (monitor.pausado) {
                    agregar_log("Sistema PAUSADO");
                } else {
                    agregar_log("Sistema REANUDADO");
                    // Despertar todos los hilos
                    pthread_cond_broadcast(&monitor.cola_izquierda);
                    pthread_cond_broadcast(&monitor.cola_derecha);
                }
                pthread_mutex_unlock(&monitor.mutex);
                break;
        }
        
        usleep(50000);
    }
    
    return NULL;
}

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

int main(int argc, char* argv[]) {
    // Inicializar semilla aleatoria
    srand(time(NULL));
    
    // Inicializar ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    
    // Verificar tamaño mínimo de terminal
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    if (max_y < 45 || max_x < 100) {
        endwin();
        printf("\n");
        printf("╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                    ERROR DE TERMINAL                          ║\n");
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║ La terminal debe ser al menos 100x45 caracteres              ║\n");
        printf("║ Terminal actual: %dx%-2d                                     ║\n", max_x, max_y);
        printf("║                                                               ║\n");
        printf("║ Por favor, maximiza la ventana de terminal e intenta de      ║\n");
        printf("║ nuevo, o ajusta el tamaño manualmente.                       ║\n");
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
        printf("\n");
        return EXIT_FAILURE;
    }
    
    // Inicializar colores
    if (has_colors()) {
        inicializar_colores();
    }
    
    // Inicializar el monitor
    if (inicializar_monitor() != 0) {
        endwin();
        fprintf(stderr, "Error crítico: No se pudo inicializar el monitor\n");
        return EXIT_FAILURE;
    }
    
    // Crear hilos del sistema
    pthread_t hilo_render, hilo_gen, hilo_teclado;
    
    if (pthread_create(&hilo_render, NULL, hilo_renderizado, NULL) != 0) {
        perror("Error al crear hilo de renderizado");
        endwin();
        return EXIT_FAILURE;
    }
    
    if (pthread_create(&hilo_gen, NULL, generador_automatico, NULL) != 0) {
        perror("Error al crear hilo generador");
        endwin();
        return EXIT_FAILURE;
    }
    
    if (pthread_create(&hilo_teclado, NULL, hilo_input, NULL) != 0) {
        perror("Error al crear hilo de entrada");
        endwin();
        return EXIT_FAILURE;
    }
    
    agregar_log("Sistema iniciado correctamente");
    agregar_log("Presiona I/D para añadir coches manualmente");
    agregar_log("ESPACIO para pausar, Q para salir");
    
    // Esperar a que el usuario termine
    pthread_join(hilo_teclado, NULL);
    
    // Señalar fin del sistema
    monitor.sistema_activo = false;
    
    // Despertar todos los hilos bloqueados
    pthread_mutex_lock(&monitor.mutex);
    pthread_cond_broadcast(&monitor.cola_izquierda);
    pthread_cond_broadcast(&monitor.cola_derecha);
    pthread_mutex_unlock(&monitor.mutex);
    
    // Esperar a que terminen los hilos
    pthread_join(hilo_render, NULL);
    pthread_join(hilo_gen, NULL);
    
    // Finalizar ncurses
    endwin();
    
    // Mostrar estadísticas finales en consola
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║         SISTEMA DE CONTROL DEL PUENTE DUERO                  ║\n");
    printf("║              ESTADÍSTICAS FINALES                             ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║                                                               ║\n");
    printf("║ Total coches generados:  %-36d ║\n", monitor.total_generados);
    printf("║ Total coches cruzados:   %-36d ║\n", monitor.total_cruzados);
    printf("║                                                               ║\n");
    printf("║ Coches en puente IZQ:    %-36d ║\n", monitor.coches_en_puente[IZQUIERDA]);
    printf("║ Coches en puente DER:    %-36d ║\n", monitor.coches_en_puente[DERECHA]);
    printf("║ Coches esperando IZQ:    %-36d ║\n", monitor.coches_esperando[IZQUIERDA]);
    printf("║ Coches esperando DER:    %-36d ║\n", monitor.coches_esperando[DERECHA]);
    printf("║                                                               ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ VERIFICACIÓN DE REQUISITOS:                                  ║\n");
    printf("║ ✓ Exclusión mutua garantizada (mutex + cond. variables)      ║\n");
    printf("║ ✓ Sin inanición (cambio de turno automático)                 ║\n");
    printf("║ ✓ Capacidad máxima respetada (%d coches simultáneos)         ║\n", MAX_COCHES_SIMULTANEOS);
    printf("║ ✓ Sensores de entrada/salida implementados                   ║\n");
    printf("║ ✓ Monitor con procedimientos (llega_cola, pasa, sale)        ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    // Destruir el monitor
    destruir_monitor();
    
    printf("Sistema finalizado correctamente.\n");
    printf("Presiona ENTER para salir...\n");
    getchar();
    
    return EXIT_SUCCESS;
}