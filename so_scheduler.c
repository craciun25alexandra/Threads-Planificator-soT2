#include "so_scheduler.h"
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
//cate threaduri pot fi maxim(necesar pt alocarea de vectori)
#define NRMAXTHREAD 2500

typedef enum {
	NEW,
	READY,
	RUNNING,
	WAITING,
	TERMINATED
} state_t;

typedef struct {
	tid_t tid;
	int priority;
	int time;
    so_handler *handler;
    int which_ios; //evenimentul pt care asteapta
	sem_t sem;
    state_t status;
} thread_t;

typedef struct {
	unsigned int ios;
	unsigned int quantum;
	int thread_nr;
	thread_t *running;
	thread_t **waiting; //vector cu toate threadurile puse in wait
    thread_t **threads; //toate threadurile
	thread_t **prio_queue; //vector pe post de coada de prioritati
    int prio_size;
    int waiting_size;

} scheduler_t;

scheduler_t *scheduler;

int so_init(unsigned int cuanta, unsigned int io) {

    if(scheduler != NULL || io > SO_MAX_NUM_EVENTS || cuanta <= 0)
        return -1;
    scheduler = (scheduler_t *) calloc(sizeof(scheduler_t), 1);
    if(!scheduler)
        return -1;
    scheduler->running = NULL;
    scheduler->quantum = cuanta;
    scheduler->ios = io;
    scheduler->thread_nr = 0;
    scheduler->prio_size = 0;
    scheduler->waiting_size = 0;
    scheduler->waiting = (thread_t **) calloc(sizeof(thread_t *), NRMAXTHREAD);
    scheduler->threads = (thread_t **) calloc(sizeof(thread_t *), NRMAXTHREAD);
	scheduler->prio_queue = (thread_t **) calloc(sizeof(thread_t*), NRMAXTHREAD);
    return 0;
}

//functia pune threadul dat ca parametru in ordine descrescatoare a prioritatii in coada de prioritati
void add_in_queue(thread_t *t) {
    
    int i, j;
    i = 0;
    //itereaza in prio_queue cat timp elementele din vector au prioritatea mai mare
    while(i < scheduler->prio_size && scheduler->prio_queue[i]->priority >= t->priority) {
        i++;
    }
    //muta celelalte elementele cu o pozitie la dreapta
	if (i < scheduler->prio_size)
		for (j = scheduler->prio_size; j > i; j--)
			scheduler->prio_queue[j] = scheduler->prio_queue[j - 1];
	scheduler->prio_queue[i] = t;
    scheduler->prio_size++;
}

//functia sterge primul element din vectorul de prioritati
void delete_from_prio() {
    //muta elementele cu o pozitie la stanga
	for (int i = 0; i < scheduler->prio_size - 1; i++)
		scheduler->prio_queue[i] = scheduler->prio_queue[i + 1];
    scheduler->prio_queue[scheduler->prio_size - 1] = NULL;
	scheduler->prio_size-- ;
}

//functia se ocupa de preemptie si actualizare dupa o informatie executata
void so_plan() {

    thread_t *current_thread_running = scheduler->running;
    //daca nu mai sunt alte threaduri in coada atunci il trezeste(pentru a se executa)
    //pe cel ce era in running
	if ((current_thread_running && scheduler->prio_size == 0)) {
		//se verifica cuanta si se reseteaza la nevoie
        if (current_thread_running->time <= 0)
			current_thread_running->time = scheduler->quantum;
		sem_post(&current_thread_running->sem);
	}
    //daca threadul curent in running este blocat sau si a terminat executia, trece la urmatorul
    //thread din coada
    else if(current_thread_running->status == WAITING || current_thread_running->status == TERMINATED) {
		scheduler->running = scheduler->prio_queue[0];
        //dupa fiecare switch se reseteaza cuanta
        scheduler->running->time = scheduler->quantum;
	    delete_from_prio(); //threadul ales este eliminat din coada de prioritati
        scheduler->running->status = RUNNING;
	    sem_post(&scheduler->running->sem); //i se da post pt a rula
	}
    //daca primul element din coada are o prioritate mai mare, se face switch
	else if (scheduler->prio_queue[0]->priority > current_thread_running->priority) {
		scheduler->running = scheduler->prio_queue[0];
        scheduler->running->time = scheduler->quantum;
	    delete_from_prio();
        scheduler->running->status = RUNNING;
	    sem_post(&scheduler->running->sem);
        //il adaug pe cel initial inapoi in coada, dupa ce l am activat pe celalalt
		add_in_queue(current_thread_running);
	}
    //daca a expirat cuanta iar cel din coada are prioritatea >= cu cel curent se face switch intre cele 2
	else if (current_thread_running->time <= 0 && 
current_thread_running->priority <= scheduler->prio_queue[0]->priority) {
		scheduler->running = scheduler->prio_queue[0];
        scheduler->running->time = scheduler->quantum;
	    delete_from_prio();
        scheduler->running->status = RUNNING;
	    sem_post(&scheduler->running->sem);
		add_in_queue(current_thread_running);
		
	}
    //daca nu a fost gasit alt thread care trebuie sa ruleze, va rula cel curent
    else {
        if (current_thread_running->time <= 0)
			current_thread_running->time = scheduler->quantum;
        sem_post(&current_thread_running->sem);
    }
}

//functia consuma o unitate de timp si verifica preemptia
void so_exec() {
    //se retine threadul curent
    thread_t *aux = scheduler->running;
    //se scade o unitate de timp pe executie
    scheduler->running->time--;
    //se actualizeaza planificatorul
    so_plan();
    //asteapta daca threadul a fost preemptat
    sem_wait(&aux->sem);
}

void* start_thread(void* args) {

    thread_t *t = (thread_t *)args;
    //asteapta ca handlerul sa fie planificat
	sem_wait(&t->sem);
    t->handler(t->priority); //se executa
    t->status = TERMINATED;
    //se cauta urmatorul thread ce trebuie executat
    so_plan();
    return NULL; 
}

tid_t so_fork(so_handler *func, unsigned int priority) {
    
    if(!func || priority > SO_MAX_PRIO)
        return INVALID_TID;
    thread_t *t = (thread_t*) calloc(sizeof(thread_t), 1);
    t->priority = priority;
	t->time = scheduler->quantum;
	t->handler = func;
    t->status = NEW;
    t->which_ios = -1; //nu asteapta dupa niciun eveniment
    sem_init(&t->sem, 0, 0); //initializez semaforul
    //creez threadul
    pthread_create(&t->tid, NULL, start_thread, (void*)t);
    add_in_queue(t); //adaug in coada de prio
    scheduler->threads[scheduler->thread_nr] = t;
    scheduler->thread_nr++;
    //daca nu exista un thread care ruleaza, se alege din coada de prioritati
    //intra pe acest caz primul thread(thread 0)
    if(scheduler->running == NULL) {
        scheduler->running = scheduler->prio_queue[0];
        scheduler->running->time = scheduler->quantum;
	    delete_from_prio();
        scheduler->running->status = RUNNING;
	    sem_post(&scheduler->running->sem); //il trezeste pt a se executa
    }
    //altfel, se executa o instructiune
    else 
        so_exec();
    return t->tid;
}

//adauga in vectorul de waiting; nu este nevoie de o ordine anume
//este folosita pt a se stoca threadurile blocate; cand se vor trezi, vor fi puse
//in ordinea prioritatilor in coada
void add_in_waiting(thread_t *t) {

	scheduler->waiting[scheduler->waiting_size] = t;
    scheduler->waiting_size++;

}

//functia blocheaza threadul din running (care nu se mai afla in prio_queue) si se pune
//in vectorul de waiting
int so_wait(unsigned int io) {

    if (io >= scheduler->ios)
		return -1;
    scheduler->running->which_ios = io; //stochez dupa ce event asteapta
	scheduler->running->status = WAITING;
    add_in_waiting(scheduler->running);
    //se consuma o unitate de timp; se replanifica scheduler
    so_exec();
	return 0;
}

//functia pune in coada de prioritati threadurile blocate ce au primit signal de la eventul
//pe care il asteaptau, le sterge din vectorul de waiting si replanifica schedulerul 
int so_signal(unsigned int io) {
    
    if (io >= scheduler->ios)
		return -1;
    int nr = 0; //cate elemente au fost gasite
    for(int i = 0; i < scheduler->waiting_size; i++)
        if(scheduler->waiting[i] && scheduler->waiting[i]->status == WAITING &&
                scheduler->waiting[i]->which_ios == io) { //daca asteapta dupa eventul dat
            scheduler->waiting[i]->which_ios = -1; //nu mai asteapta dupa niciun event
            scheduler->waiting[i]->status = READY; //marcheaza faptul ca s a trezit
            add_in_queue(scheduler->waiting[i]); //il pune in coada de prio
            nr++;
        }
    //stergerea din waiting
    //in urma stergerii, in timpul forului, scheduler->waiting->size s ar fi schimbat si
    //nu s ar fi verificat corect toate elementele, drept urmare stochez valoarea sa inainte 
    int size = scheduler->waiting_size;
    for(int i = 0; i < size; i++)
        if(scheduler->waiting[i] && scheduler->waiting[i]->status == READY) { //daca a fost trezit
            for(int j = i; j < scheduler->waiting_size - 1; j++)
            //de la pozitia elementului gasit, celelalte se deplaseaza o pozitie la stanga
                scheduler->waiting[j] = scheduler->waiting[j+1];
            scheduler->waiting[scheduler->waiting_size - 1] = NULL;
            scheduler->waiting_size-- ;
            }
    //se consuma timp; se replanifica
    so_exec();
    return nr;
}

//functia asteapta si executa threadurile ramase, iar apoi elibereaza memorie
void so_end() {

    if(scheduler) {
        //ia threadurile din vectorul ce le include pe toate
        for (int i = 0; i < scheduler->thread_nr; i++) {
            //asteapta threadul sa se termine
            pthread_join(scheduler->threads[i]->tid, NULL);
        }
        //apoi ii distruge semaforul si elibereaza memoria
        for (int i = 0; i < scheduler->thread_nr; i++) {
            sem_destroy(&scheduler->threads[i]->sem);
            free(scheduler->threads[i]);
        }
        //elibereaza elementele schedulerului
        for (int i = 0; i < scheduler->prio_size; i++)
            free(scheduler->prio_queue[i]);
        for (int i = 0; i < scheduler->waiting_size; i++)
            free(scheduler->waiting[i]);
        free(scheduler->waiting);
        free(scheduler->threads);
	    free(scheduler->prio_queue);
        free(scheduler);
        scheduler = NULL;
    }
}





