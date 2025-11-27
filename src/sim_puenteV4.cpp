#include <iostream>
#include <vector>
#include <random>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <cstring> 
#include <sys/ipc.h>  
#include <sys/sem.h>  
#include <wait.h>     
#include <unistd.h>   
#include <cstdio>     
#include <limits>   
#include <cstdlib>
#include <algorithm> 

using namespace std;

#define MAX_COCHES_SIMULTANEOS 3
#define TOTAL_COCHES_POR_LADO 4 
#define TIEMPO_CRUCE_MS 1000

#define SEM_KEY 1234 
#define NUM_SEMS 1
#define MUTEX_PUENTE 0 

enum Direccion { IZQUIERDA = 0, DERECHA = 1, NINGUNO = 2 };
enum EstadoCoche { ESPERANDO, CRUZANDO, FINALIZADO, RECHAZADO }; 

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};

int semid; 


string get_timestamp() {
    time_t now = time(NULL); 
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

void log_evento(const string& mensaje) {
    fprintf(stderr, "[%s - PID: %d] %s\n", get_timestamp().c_str(), getpid(), mensaje.c_str());
}

void sem_operacion(int sem_index, int op) {
    struct sembuf sbuf;
    sbuf.sem_num = sem_index;
    sbuf.sem_op = op; 
    sbuf.sem_flg = 0;
    if (semop(semid, &sbuf, 1) == -1) {
        perror("semop error");
        exit(1); 
    }
}

void sem_wait() {
    sem_operacion(MUTEX_PUENTE, -1);
}

void sem_signal() {
    sem_operacion(MUTEX_PUENTE, 1);
}

void tarea_coche(int id, Direccion direccion) {
    log_evento("Coche " + to_string(id) + " llega a cola " + (direccion == IZQUIERDA ? "IZQ" : "DER"));
    log_evento("Coche " + to_string(id) + " ESPERANDO permiso (P).");
    
    sem_wait(); 
    
    log_evento("Coche " + to_string(id) + " COMIENZA CRUCE.");
    usleep(TIEMPO_CRUCE_MS * 1000 + (rand() % 500) * 1000); 

    log_evento("Coche " + to_string(id) + " FINALIZA CRUCE.");
    sem_signal(); 

    exit(0); 
}

void generador_coches(Direccion direccion) {
    
    char log_message[100];
    const char* dir_str = (direccion == IZQUIERDA ? "IZQ" : "DER");
    
    sprintf(log_message, "[%s - PID: %d] Generador %s iniciado.", get_timestamp().c_str(), getpid(), dir_str);
    fprintf(stderr, "%s\n", log_message);
    
    for (int i = 0; i < TOTAL_COCHES_POR_LADO; i++) {
        int id = (direccion * 100) + i + 1;
        
        pid_t pid = fork();

        if (pid < 0) {
            perror("Error al hacer fork");
            exit(1);
        } else if (pid == 0) {
            tarea_coche(id, direccion);
        } else {
            log_evento("Proceso Generador creó Coche " + to_string(id) + " (PID Hijo: " + to_string(pid) + ")");
            usleep(500000 + rand() % 1500000); 
        }
    }
    
    int status;
    pid_t wpid;
    
    while ((wpid = wait(&status)) > 0);
    
    sprintf(log_message, "[%s - PID: %d] Generador %s finalizado.", get_timestamp().c_str(), getpid(), dir_str);
    fprintf(stderr, "%s\n", log_message);

    exit(0); 
}

int main() {
    srand(time(NULL)); 
    
    semid = semget(SEM_KEY, NUM_SEMS, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("Error al crear semáforo");
        return 1;
    }
    
    union semun arg;
    arg.val = MAX_COCHES_SIMULTANEOS;
    if (semctl(semid, MUTEX_PUENTE, SETVAL, arg) == -1) {
        perror("Error al inicializar semáforo");
        return 1;
    }
    log_evento("Semáforo IPC (ID: " + to_string(semid) + ") creado e inicializado con valor " + to_string(MAX_COCHES_SIMULTANEOS));

    pid_t pid_izq, pid_der;
    
    if ((pid_izq = fork()) == 0) {
        generador_coches(IZQUIERDA);
    } else if (pid_izq < 0) {
        perror("Error fork izq");
        return 1;
    }

    if ((pid_der = fork()) == 0) {
        generador_coches(DERECHA);
    } else if (pid_der < 0) {
        perror("Error fork der");
        return 1;
    }

    int status;
    pid_t wpid;
    
    log_evento("Proceso Principal esperando a que los generadores terminen...");
    while ((wpid = wait(&status)) > 0) {
        log_evento("Proceso hijo terminado (PID: " + to_string(wpid) + ")");
    }
    
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("Error al eliminar semáforo");
        return 1;
    }
    log_evento("Sistema IPC (semáforo) eliminado.");
    
    cout << "\n--- SIMULACIÓN FINALIZADA ---\n";
    
    return 0;
}