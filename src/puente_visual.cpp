#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>

// ============================================================================
// CÓDIGOS DE COLOR ANSI (sin librerías externas)
// ============================================================================
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"

// Colores de texto
#define BLACK       "\033[30m"
#define RED         "\033[31m"
#define GREEN       "\033[32m"
#define YELLOW      "\033[33m"
#define BLUE        "\033[34m"
#define MAGENTA     "\033[35m"
#define CYAN        "\033[36m"
#define WHITE       "\033[37m"

// Colores de fondo
#define BG_RED      "\033[41m"
#define BG_GREEN    "\033[42m"
#define BG_YELLOW   "\033[43m"
#define BG_BLUE     "\033[44m"
#define BG_CYAN     "\033[46m"

// Caracteres especiales Unicode
#define CAR_LEFT    "🚗"
#define CAR_RIGHT   "🚙"
#define SENSOR      "🔴"
#define BARRIER     "🚧"
#define CHECK       "✓"
#define ARROW_RIGHT "→"
#define ARROW_LEFT  "←"
#define WARNING     "⚠"

// ============================================================================
// CONSTANTES DEL SISTEMA
// ============================================================================
#define MAX_COCHES_SIMULTANEOS 3
#define MAX_COCHES_SEGUIDOS 5
#define TOTAL_COCHES_POR_LADO 8
#define TIEMPO_CRUCE_MS 2000

// Direcciones
typedef enum { IZQUIERDA = 0, DERECHA = 1, NINGUNO = 2 } Direccion;
typedef enum { ESPERANDO, CRUZANDO, FINALIZADO } EstadoCoche;

// ============================================================================
// ESTRUCTURA DEL MONITOR DEL PUENTE
// ============================================================================
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cola_izquierda;
    pthread_cond_t cola_derecha;
    
    int coches_en_puente[2];
    int coches_esperando[2];
    int coches_seguidos[2];
    Direccion turno;
    
    int total_cruzados;
    int total_generados;
    bool sistema_activo;
    
} MonitorPuente;

MonitorPuente monitor;

// ============================================================================
// ESTRUCTURA DEL COCHE
// ============================================================================
typedef struct {
    int id;
    Direccion direccion;
    EstadoCoche estado;
    time_t tiempo_llegada;
    time_t tiempo_inicio_cruce;
    time_t tiempo_salida;
} Coche;

// ============================================================================
// FUNCIONES AUXILIARES
// ============================================================================

void limpiar_pantalla() {
    printf("\033[2J\033[H");
}

const char* direccion_str(Direccion dir) {
    switch(dir) {
        case IZQUIERDA: return "IZQUIERDA";
        case DERECHA: return "DERECHA";
        default: return "NINGUNO";
    }
}

const char* direccion_color(Direccion dir) {
    switch(dir) {
        case IZQUIERDA: return GREEN;
        case DERECHA: return BLUE;
        default: return WHITE;
    }
}

void obtener_timestamp(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "%H:%M:%S", tm_info);
}

void log_evento(const char* formato, ...) {
    char timestamp[20];
    obtener_timestamp(timestamp, sizeof(timestamp));
    
    printf(DIM "[%s]" RESET " ", timestamp);
    
    va_list args;
    va_start(args, formato);
    vprintf(formato, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void imprimir_linea(char c, int longitud) {
    for (int i = 0; i < longitud; i++) {
        printf("%c", c);
    }
    printf("\n");
}

void mostrar_estado_puente() {
    pthread_mutex_lock(&monitor.mutex);
    
    printf("\n");
    printf(BOLD CYAN);
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║          ESTADO ACTUAL DEL PUENTE DUERO                     ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf(RESET);
    
    // Turno actual
    printf("║ " BOLD "Turno actual:" RESET "   ");
    if (monitor.turno == IZQUIERDA) {
        printf(BOLD GREEN "IZQUIERDA" RESET "                                   ║\n");
    } else if (monitor.turno == DERECHA) {
        printf(BOLD BLUE "DERECHA" RESET "                                     ║\n");
    } else {
        printf(YELLOW "NINGUNO (Libre)" RESET "                            ║\n");
    }
    
    printf(CYAN "╠══════════════════════════════════════════════════════════════╣\n" RESET);
    
    // Lado IZQUIERDO
    printf("║ " BOLD GREEN "LADO IZQUIERDO" RESET "                                           ║\n");
    printf("║   Esperando:        " BOLD "%d" RESET " coches                           ║\n", 
           monitor.coches_esperando[IZQUIERDA]);
    printf("║   En puente:        " BOLD "%d/%d" RESET " coches                         ║\n", 
           monitor.coches_en_puente[IZQUIERDA], MAX_COCHES_SIMULTANEOS);
    printf("║   Seguidos:         " BOLD "%d/%d" RESET " coches consecutivos            ║\n", 
           monitor.coches_seguidos[IZQUIERDA], MAX_COCHES_SEGUIDOS);
    
    // Barra de ocupación izquierda
    printf("║   Ocupacion: [");
    int ocupacion_izq = (monitor.coches_en_puente[IZQUIERDA] * 20) / MAX_COCHES_SIMULTANEOS;
    for (int i = 0; i < 20; i++) {
        if (i < ocupacion_izq) printf(GREEN "█" RESET);
        else printf(DIM "░" RESET);
    }
    printf("] %3d%%               ║\n", 
           (monitor.coches_en_puente[IZQUIERDA] * 100) / MAX_COCHES_SIMULTANEOS);
    
    printf(CYAN "╠══════════════════════════════════════════════════════════════╣\n" RESET);
    
    // Lado DERECHO
    printf("║ " BOLD BLUE "LADO DERECHO" RESET "                                             ║\n");
    printf("║   Esperando:        " BOLD "%d" RESET " coches                           ║\n", 
           monitor.coches_esperando[DERECHA]);
    printf("║   En puente:        " BOLD "%d/%d" RESET " coches                         ║\n", 
           monitor.coches_en_puente[DERECHA], MAX_COCHES_SIMULTANEOS);
    printf("║   Seguidos:         " BOLD "%d/%d" RESET " coches consecutivos            ║\n", 
           monitor.coches_seguidos[DERECHA], MAX_COCHES_SEGUIDOS);
    
    // Barra de ocupación derecha
    printf("║   Ocupacion: [");
    int ocupacion_der = (monitor.coches_en_puente[DERECHA] * 20) / MAX_COCHES_SIMULTANEOS;
    for (int i = 0; i < 20; i++) {
        if (i < ocupacion_der) printf(BLUE "█" RESET);
        else printf(DIM "░" RESET);
    }
    printf("] %3d%%               ║\n", 
           (monitor.coches_en_puente[DERECHA] * 100) / MAX_COCHES_SIMULTANEOS);
    
    printf(CYAN "╠══════════════════════════════════════════════════════════════╣\n" RESET);
    
    // Estadísticas globales
    printf("║ " BOLD MAGENTA "ESTADISTICAS GLOBALES" RESET "                                  ║\n");
    printf("║   Total generados:  " BOLD CYAN "%2d" RESET " coches                          ║\n", 
           monitor.total_generados);
    printf("║   Total cruzados:   " BOLD GREEN "%2d" RESET " coches                          ║\n", 
           monitor.total_cruzados);
    
    printf(BOLD CYAN);
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf(RESET "\n");
    
    pthread_mutex_unlock(&monitor.mutex);
}

// ============================================================================
// INICIALIZACIÓN DEL MONITOR
// ============================================================================

int inicializar_monitor() {
    if (pthread_mutex_init(&monitor.mutex, NULL) != 0) {
        perror(RED "Error al inicializar mutex del monitor" RESET);
        return -1;
    }
    
    if (pthread_cond_init(&monitor.cola_izquierda, NULL) != 0) {
        perror(RED "Error al inicializar cola izquierda" RESET);
        return -1;
    }
    
    if (pthread_cond_init(&monitor.cola_derecha, NULL) != 0) {
        perror(RED "Error al inicializar cola derecha" RESET);
        return -1;
    }
    
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
    
    log_evento(GREEN CHECK " Monitor del puente inicializado correctamente" RESET);
    return 0;
}

void destruir_monitor() {
    pthread_mutex_destroy(&monitor.mutex);
    pthread_cond_destroy(&monitor.cola_izquierda);
    pthread_cond_destroy(&monitor.cola_derecha);
    log_evento(GREEN CHECK " Monitor del puente destruido" RESET);
}

// ============================================================================
// PROCEDIMIENTOS DEL MONITOR
// ============================================================================

void llega_cola(Coche* coche) {
    pthread_mutex_lock(&monitor.mutex);
    
    monitor.coches_esperando[coche->direccion]++;
    const char* color = direccion_color(coche->direccion);
    const char* car_icon = (coche->direccion == IZQUIERDA) ? CAR_LEFT : CAR_RIGHT;
    
    log_evento(BOLD "%s" RESET " " MAGENTA "[SENSOR ENTRADA]" RESET " Coche %s%d%s detectado en cola %s%s%s (esperando: %s%d%s)",
               SENSOR, color, coche->id, RESET,
               color, direccion_str(coche->direccion), RESET,
               BOLD, monitor.coches_esperando[coche->direccion], RESET);
    
    pthread_mutex_unlock(&monitor.mutex);
}

void pasa_coche(Coche* coche) {
    pthread_mutex_lock(&monitor.mutex);
    
    Direccion mi_dir = coche->direccion;
    Direccion otra_dir = (mi_dir == IZQUIERDA) ? DERECHA : IZQUIERDA;
    pthread_cond_t* mi_cola = (mi_dir == IZQUIERDA) ? 
                              &monitor.cola_izquierda : &monitor.cola_derecha;
    
    while (true) {
        bool es_mi_turno = (monitor.turno == mi_dir || monitor.turno == NINGUNO);
        bool puente_libre = (monitor.coches_en_puente[otra_dir] == 0);
        bool hay_capacidad = (monitor.coches_en_puente[mi_dir] < MAX_COCHES_SIMULTANEOS);
        bool puede_pasar_seguido = true;
        
        if (monitor.coches_esperando[otra_dir] > 0) {
            puede_pasar_seguido = (monitor.coches_seguidos[mi_dir] < MAX_COCHES_SEGUIDOS);
        }
        
        if (es_mi_turno && puente_libre && hay_capacidad && puede_pasar_seguido) {
            break;
        }
        
        pthread_cond_wait(mi_cola, &monitor.mutex);
    }
    
    if (monitor.turno == NINGUNO) {
        monitor.turno = mi_dir;
    }
    
    monitor.coches_esperando[mi_dir]--;
    monitor.coches_en_puente[mi_dir]++;
    
    if (monitor.coches_esperando[otra_dir] > 0) {
        monitor.coches_seguidos[mi_dir]++;
    }
    
    coche->estado = CRUZANDO;
    coche->tiempo_inicio_cruce = time(NULL);
    
    const char* color = direccion_color(coche->direccion);
    const char* arrow = (mi_dir == IZQUIERDA) ? ARROW_RIGHT : ARROW_LEFT;
    
    log_evento(BOLD GREEN BARRIER " [BARRERA ABRE]" RESET " Coche %s%d%s %s%s%s ENTRA desde %s%s%s "
               "(en puente: %s%d%s, seguidos: %d/%d)",
               color, coche->id, RESET,
               color, arrow, RESET,
               color, direccion_str(mi_dir), RESET,
               BOLD, monitor.coches_en_puente[mi_dir], RESET,
               monitor.coches_seguidos[mi_dir], MAX_COCHES_SEGUIDOS);
    
    pthread_mutex_unlock(&monitor.mutex);
}

void sale_coche(Coche* coche) {
    pthread_mutex_lock(&monitor.mutex);
    
    Direccion mi_dir = coche->direccion;
    Direccion otra_dir = (mi_dir == IZQUIERDA) ? DERECHA : IZQUIERDA;
    
    monitor.coches_en_puente[mi_dir]--;
    monitor.total_cruzados++;
    coche->estado = FINALIZADO;
    coche->tiempo_salida = time(NULL);
    
    const char* color = direccion_color(coche->direccion);
    
    log_evento(BOLD CYAN CHECK " [SENSOR SALIDA]" RESET " Coche %s%d%s SALE del puente %s%s%s "
               "(quedan: %s%d%s, tiempo: %s%lds%s)",
               color, coche->id, RESET,
               color, direccion_str(mi_dir), RESET,
               BOLD, monitor.coches_en_puente[mi_dir], RESET,
               BOLD, coche->tiempo_salida - coche->tiempo_inicio_cruce, RESET);
    
    if (monitor.coches_en_puente[mi_dir] == 0) {
        if (monitor.coches_seguidos[mi_dir] >= MAX_COCHES_SEGUIDOS) {
            monitor.coches_seguidos[mi_dir] = 0;
        }
        
        if (monitor.coches_esperando[otra_dir] > 0) {
            monitor.turno = otra_dir;
            const char* turno_color = direccion_color(otra_dir);
            log_evento(BOLD YELLOW "🔄 Cambio de turno" RESET " %s %s%s%s %s",
                      (otra_dir == IZQUIERDA) ? ARROW_LEFT : ARROW_RIGHT,
                      turno_color, direccion_str(otra_dir), RESET,
                      (otra_dir == IZQUIERDA) ? ARROW_LEFT : ARROW_RIGHT);
            
            if (otra_dir == IZQUIERDA) {
                pthread_cond_broadcast(&monitor.cola_izquierda);
            } else {
                pthread_cond_broadcast(&monitor.cola_derecha);
            }
        } else {
            monitor.turno = NINGUNO;
            monitor.coches_seguidos[mi_dir] = 0;
        }
    } else {
        if (mi_dir == IZQUIERDA) {
            pthread_cond_signal(&monitor.cola_izquierda);
        } else {
            pthread_cond_signal(&monitor.cola_derecha);
        }
    }
    
    pthread_mutex_unlock(&monitor.mutex);
}

// ============================================================================
// FUNCIÓN DEL HILO COCHE
// ============================================================================

void* tarea_coche(void* arg) {
    Coche* coche = (Coche*)arg;
    
    coche->tiempo_llegada = time(NULL);
    coche->estado = ESPERANDO;
    llega_cola(coche);
    
    usleep((rand() % 500) * 1000);
    
    pasa_coche(coche);
    
    usleep(TIEMPO_CRUCE_MS * 1000);
    
    sale_coche(coche);
    
    return NULL;
}

// ============================================================================
// GENERADOR DE COCHES
// ============================================================================

void* generador_coches(void* arg) {
    Direccion direccion = *((Direccion*)arg);
    pthread_t hilos_coches[TOTAL_COCHES_POR_LADO];
    Coche coches[TOTAL_COCHES_POR_LADO];
    
    const char* color = direccion_color(direccion);
    log_evento(BOLD "🏁 Generador de coches %s%s%s iniciado" RESET, 
               color, direccion_str(direccion), RESET);
    
    for (int i = 0; i < TOTAL_COCHES_POR_LADO; i++) {
        if (!monitor.sistema_activo) break;
        
        coches[i].id = (direccion * 100) + i + 1;
        coches[i].direccion = direccion;
        coches[i].estado = ESPERANDO;
        
        if (pthread_create(&hilos_coches[i], NULL, tarea_coche, &coches[i]) != 0) {
            perror(RED "Error al crear hilo del coche" RESET);
            continue;
        }
        
        monitor.total_generados++;
        
        usleep((500 + rand() % 1500) * 1000);
    }
    
    for (int i = 0; i < TOTAL_COCHES_POR_LADO; i++) {
        pthread_join(hilos_coches[i], NULL);
    }
    
    log_evento(BOLD "🏁 Generador de coches %s%s%s finalizado" RESET, 
               color, direccion_str(direccion), RESET);
    return NULL;
}

// ============================================================================
// MONITOR DE ESTADO (HILO SEPARADO)
// ============================================================================

void* monitor_estado(void* arg) {
    while (monitor.sistema_activo) {
        sleep(5);
        mostrar_estado_puente();
    }
    return NULL;
}

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

int main(int argc, char* argv[]) {
    srand(time(NULL));
    
    limpiar_pantalla();
    
    printf("\n");
    printf(BOLD CYAN);
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                                                               ║\n");
    printf("║         " BOLD YELLOW "SISTEMA DE CONTROL DEL PUENTE DUERO" CYAN "              ║\n");
    printf("║         " RESET CYAN "Implementacion con Monitor y Semaforos POSIX" BOLD CYAN "     ║\n");
    printf("║                                                               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf(RESET "\n");
    
    printf(BOLD "Parametros del sistema:\n" RESET);
    printf("  " GREEN ">" RESET " Capacidad maxima:    " BOLD CYAN "%d" RESET " coches simultaneos\n", MAX_COCHES_SIMULTANEOS);
    printf("  " GREEN ">" RESET " Limite consecutivos: " BOLD CYAN "%d" RESET " coches por turno\n", MAX_COCHES_SEGUIDOS);
    printf("  " GREEN ">" RESET " Coches por lado:     " BOLD CYAN "%d" RESET " coches\n", TOTAL_COCHES_POR_LADO);
    printf("  " GREEN ">" RESET " Tiempo de cruce:     " BOLD CYAN "%d" RESET " segundos\n\n", TIEMPO_CRUCE_MS / 1000);
    
    printf(DIM);
    imprimir_linea('─', 65);
    printf(RESET "\n");
    
    if (inicializar_monitor() != 0) {
        fprintf(stderr, RED "Error critico: No se pudo inicializar el monitor\n" RESET);
        exit(EXIT_FAILURE);
    }
    
    pthread_t generador_izq, generador_der, monitor_thread;
    Direccion dir_izq = IZQUIERDA;
    Direccion dir_der = DERECHA;
    
    pthread_create(&monitor_thread, NULL, monitor_estado, NULL);
    
    log_evento(BOLD YELLOW "🚀 Iniciando sistema de control del puente" RESET);
    
    pthread_create(&generador_izq, NULL, generador_coches, &dir_izq);
    pthread_create(&generador_der, NULL, generador_coches, &dir_der);
    
    pthread_join(generador_izq, NULL);
    pthread_join(generador_der, NULL);
    
    monitor.sistema_activo = false;
    pthread_join(monitor_thread, NULL);
    
    printf("\n");
    printf(DIM);
    imprimir_linea('─', 65);
    printf(RESET "\n");
    
    printf(BOLD MAGENTA);
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                  ESTADISTICAS FINALES                         ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf(RESET "║ Total coches generados:  " BOLD CYAN "%2d" RESET " coches                     " MAGENTA "║\n", monitor.total_generados);
    printf(RESET "║ Total coches cruzados:   " BOLD GREEN "%2d" RESET " coches                     " MAGENTA "║\n", monitor.total_cruzados);
    printf(RESET "║ Coches en puente IZQ:    " GREEN "%2d" RESET " coches                     " MAGENTA "║\n", monitor.coches_en_puente[IZQUIERDA]);
    printf(RESET "║ Coches en puente DER:    " BLUE "%2d" RESET " coches                     " MAGENTA "║\n", monitor.coches_en_puente[DERECHA]);
    printf(RESET "║ Coches esperando IZQ:    " GREEN "%2d" RESET " coches                     " MAGENTA "║\n", monitor.coches_esperando[IZQUIERDA]);
    printf(RESET "║ Coches esperando DER:    " BLUE "%2d" RESET " coches                     " MAGENTA "║\n", monitor.coches_esperando[DERECHA]);
    printf(BOLD MAGENTA "╚═══════════════════════════════════════════════════════════════╝\n");
    printf(RESET "\n");
    
    log_evento(GREEN CHECK " Sistema finalizado correctamente" RESET);
    
    destruir_monitor();
    
    return EXIT_SUCCESS;
}