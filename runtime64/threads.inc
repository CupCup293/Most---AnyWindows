typedef WIN DWORD (*ThreadStart)(void *);

typedef struct GuestThread {
    struct GuestThread *next;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    ThreadStart start;
    void *parameter;
    DWORD id, status;
    int done, cancelling;
} GuestThread;

static pthread_mutex_t thread_list_lock = PTHREAD_MUTEX_INITIALIZER;
static GuestThread *threads;
static _Thread_local GuestThread *active_guest_thread;
extern int guest_enter_thread(void);

static GuestThread *find_thread(uintptr_t handle)
{
    pthread_mutex_lock(&thread_list_lock);
    GuestThread *found = threads;
    while (found && (uintptr_t)found != handle)
        found = found->next;
    pthread_mutex_unlock(&thread_list_lock);
    return found;
}

static void finish_guest_thread(void *data)
{
    GuestThread *thread = data;
    pthread_mutex_lock(&thread->lock);
    thread->done = 1;
    pthread_cond_broadcast(&thread->changed);
    pthread_mutex_unlock(&thread->lock);
}

static void *run_guest_thread(void *data)
{
    GuestThread *thread = data;
    active_guest_thread = thread;
    pthread_mutex_lock(&thread->lock);
    thread->id = syscall(SYS_gettid);
    pthread_cond_broadcast(&thread->changed);
    pthread_mutex_unlock(&thread->lock);
    pthread_cleanup_push(finish_guest_thread, thread);
    DWORD status = guest_enter_thread() ? thread->start(thread->parameter) : 124;
    pthread_mutex_lock(&thread->lock);
    if (!thread->cancelling)
        thread->status = status;
    pthread_mutex_unlock(&thread->lock);
    pthread_cleanup_pop(1);
    return NULL;
}

WIN uintptr_t win_CreateThread(void *security, size_t stack_size, ThreadStart start,
                                void *parameter, DWORD flags, DWORD *id)
{
    (void)security;
    if (!start || (flags & ~0x10000u)) {
        last_error = 87;
        return 0;
    }
    GuestThread *thread = calloc(1, sizeof(*thread));
    if (!thread) {
        last_error = 8;
        return 0;
    }
    thread->start = start;
    thread->parameter = parameter;
    pthread_mutex_init(&thread->lock, NULL);
    pthread_cond_init(&thread->changed, NULL);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (stack_size && stack_size > PTHREAD_STACK_MIN)
        pthread_attr_setstacksize(&attributes, stack_size);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    int error = pthread_create(&thread->thread, &attributes, run_guest_thread, thread);
    pthread_attr_destroy(&attributes);
    if (error) {
        pthread_cond_destroy(&thread->changed);
        pthread_mutex_destroy(&thread->lock);
        free(thread);
        last_error = 8;
        return 0;
    }
    pthread_mutex_lock(&thread_list_lock);
    thread->next = threads;
    threads = thread;
    pthread_mutex_unlock(&thread_list_lock);
    if (id) {
        pthread_mutex_lock(&thread->lock);
        while (!thread->id)
            pthread_cond_wait(&thread->changed, &thread->lock);
        *id = thread->id;
        pthread_mutex_unlock(&thread->lock);
    }
    return (uintptr_t)thread;
}

static struct timespec deadline(DWORD timeout)
{
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += timeout / 1000;
    end.tv_nsec += (timeout % 1000) * 1000000;
    if (end.tv_nsec >= 1000000000) {
        ++end.tv_sec;
        end.tv_nsec -= 1000000000;
    }
    return end;
}

static DWORD wait_thread(GuestThread *thread, DWORD timeout)
{
    struct timespec end = deadline(timeout);
    pthread_mutex_lock(&thread->lock);
    int error = 0;
    while (!thread->done && !error)
        error = timeout == UINT32_MAX ? pthread_cond_wait(&thread->changed, &thread->lock)
            : pthread_cond_timedwait(&thread->changed, &thread->lock, &end);
    int done = thread->done;
    pthread_mutex_unlock(&thread->lock);
    if (done)
        return 0;
    if (error == ETIMEDOUT)
        return 258;
    last_error = 6;
    return UINT32_MAX;
}

WIN int win_GetExitCodeThread(uintptr_t handle, DWORD *status)
{
    GuestThread *thread = find_thread(handle);
    if (!thread || !status) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&thread->lock);
    *status = thread->done ? thread->status : 259;
    pthread_mutex_unlock(&thread->lock);
    return 1;
}

WIN int win_TerminateThread(uintptr_t handle, DWORD status)
{
    GuestThread *thread = find_thread(handle);
    if (!thread) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&thread->lock);
    if (thread->done) {
        pthread_mutex_unlock(&thread->lock);
        last_error = 5;
        return 0;
    }
    thread->cancelling = 1;
    thread->status = status;
    int error = pthread_cancel(thread->thread);
    pthread_mutex_unlock(&thread->lock);
    if (error) {
        last_error = 5;
        return 0;
    }
    return 1;
}

WIN void win_ExitThread(DWORD status)
{
    if (active_guest_thread) {
        pthread_mutex_lock(&active_guest_thread->lock);
        if (!active_guest_thread->cancelling)
            active_guest_thread->status = status;
        pthread_mutex_unlock(&active_guest_thread->lock);
    }
    pthread_exit(NULL);
}

WIN int win_SetThreadPriority(uintptr_t handle, int priority)
{
    GuestThread *thread = handle == (uintptr_t)-2 ? active_guest_thread : find_thread(handle);
    if (!thread && handle != (uintptr_t)-2) {
        last_error = 6;
        return 0;
    }
    int nice_value;
    switch (priority) {
    case -15: nice_value = 19; break;
    case -2: nice_value = 10; break;
    case -1: nice_value = 5; break;
    case 0: nice_value = 0; break;
    case 1: nice_value = -5; break;
    case 2: nice_value = -10; break;
    case 15: nice_value = -20; break;
    default: last_error = 87; return 0;
    }
    pid_t id = thread ? thread->id : syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, id, nice_value) == 0)
        return 1;
    last_error = 5;
    return 0;
}

typedef struct GuestEvent {
    struct GuestEvent *next;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int manual, signalled, closed;
    char *name;
} GuestEvent;
static pthread_mutex_t event_list_lock = PTHREAD_MUTEX_INITIALIZER;
static GuestEvent *events;

static GuestEvent *find_event(uintptr_t handle)
{
    pthread_mutex_lock(&event_list_lock);
    GuestEvent *found = events;
    while (found && (uintptr_t)found != handle)
        found = found->next;
    pthread_mutex_unlock(&event_list_lock);
    return found;
}

WIN uintptr_t win_CreateEventA(void *attributes, int manual, int initial, const char *name)
{
    (void)attributes;
    pthread_mutex_lock(&event_list_lock);
    if (name) {
        for (GuestEvent *found = events; found; found = found->next) {
            if (!found->closed && found->name && !strcmp(found->name, name)) {
                pthread_mutex_unlock(&event_list_lock);
                last_error = 183;
                return (uintptr_t)found;
            }
        }
    }
    GuestEvent *event = calloc(1, sizeof(*event));
    if (!event) {
        pthread_mutex_unlock(&event_list_lock);
        last_error = 8;
        return 0;
    }
    if (name)
        event->name = strdup(name);
    pthread_mutex_init(&event->lock, NULL);
    pthread_cond_init(&event->condition, NULL);
    event->manual = !!manual;
    event->signalled = !!initial;
    event->next = events;
    events = event;
    pthread_mutex_unlock(&event_list_lock);
    last_error = 0;
    return (uintptr_t)event;
}

static int modify_event(uintptr_t handle, int signal)
{
    GuestEvent *event = find_event(handle);
    if (!event) {
        last_error = 6;
        return 0;
    }
    pthread_mutex_lock(&event->lock);
    if (event->closed) {
        pthread_mutex_unlock(&event->lock);
        last_error = 6;
        return 0;
    }
    event->signalled = signal;
    if (signal) {
        if (event->manual)
            pthread_cond_broadcast(&event->condition);
        else
            pthread_cond_signal(&event->condition);
    }
    pthread_mutex_unlock(&event->lock);
    return 1;
}
WIN int win_SetEvent(uintptr_t event) { return modify_event(event, 1); }
WIN int win_ResetEvent(uintptr_t event) { return modify_event(event, 0); }

static DWORD wait_event(GuestEvent *event, DWORD timeout)
{
    struct timespec end = deadline(timeout);
    pthread_mutex_lock(&event->lock);
    int error = 0;
    while (!event->closed && !event->signalled && !error)
        error = timeout == UINT32_MAX ? pthread_cond_wait(&event->condition, &event->lock)
            : pthread_cond_timedwait(&event->condition, &event->lock, &end);
    int closed = event->closed;
    int signalled = event->signalled;
    if (!event->manual && !closed)
        event->signalled = 0;
    pthread_mutex_unlock(&event->lock);
    if (!closed && signalled)
        return 0;
    if (error == ETIMEDOUT)
        return 258;
    last_error = 6;
    return UINT32_MAX;
}
