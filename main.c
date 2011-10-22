
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ev.h"
#include "jsapi.h"

#define DEBUG_SPEW 0

// todo move this to command line parameter
#define NUM_THREADS 4
// calculate somehow?
#define RUNTIME_SIZE 128 * 1024 * 1024

// ****************************************************
// Managing the set of Actors that are ready to run
// ****************************************************

#define MAX_RUNNABLES_OUTSTANDING 4096
#define MAX_SCHEDULE_OUTSTANDING 4096

typedef struct _continuation {
    JSContext * cx;
    jsval * data;
    jsval * tag;
    jsval * cast;
    uint32 intval; // how much to read, or how much was written, or how long to wait
    struct _continuation * next; // the next chained continuation for this context
} Continuation;

static int shutting_down = 0;

static int actors_outstanding = 0;
static pthread_mutex_t actors_mutex = PTHREAD_MUTEX_INITIALIZER;

static Continuation *runnables[MAX_RUNNABLES_OUTSTANDING];
static int runnables_outstanding = 0;
static pthread_mutex_t runnables_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t runnables_condition = PTHREAD_COND_INITIALIZER;

static Continuation *schedule[MAX_SCHEDULE_OUTSTANDING];
static int schedule_outstanding = 0;
static pthread_mutex_t schedule_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_condition = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t reschedule_mutex = PTHREAD_MUTEX_INITIALIZER;

static jsval * cast_wait = NULL;
static jsval * cast_send = NULL;
static jsval * cast_recv = NULL;
static jsval * cast_url = NULL;

JSBool schedule_actor(Continuation * cont) {
    pthread_mutex_lock(&runnables_mutex);
    if (runnables_outstanding == MAX_RUNNABLES_OUTSTANDING) {
        // TODO block until space.
        pthread_mutex_unlock(&runnables_mutex);
        return JS_FALSE;
    }
    runnables[runnables_outstanding++] = cont;
    pthread_cond_signal(&runnables_condition);
    pthread_mutex_unlock(&runnables_mutex);
    return JS_TRUE;
}

JSBool schedule_cast(JSContext *cx, jsval * cast, jsval * data, jsval * tag) {
    Continuation * cont = (Continuation *)malloc(sizeof(Continuation));
    cont->cx = cx;
    cont->cast = cast;
    cont->data = data;
    cont->tag = tag;
    cont->intval = 0;
    cont->next = NULL;
    return schedule_actor(cont);
}

int main_schedule_io(JSContext * cx, jsval * cast, jsval * data, jsval * tag, int fileno) {
    pthread_mutex_lock(&schedule_mutex);
    if (schedule_outstanding == MAX_SCHEDULE_OUTSTANDING) {
        pthread_mutex_unlock(&schedule_mutex);
        return JS_FALSE;
    }

    Continuation * cont = (Continuation *)malloc(sizeof(Continuation));
    cont->cx = cx;
    cont->data = data;
    cont->tag = tag;
    cont->cast = cast;
    cont->intval = fileno;
    cont->next = NULL;

    schedule[schedule_outstanding++] = cont;
    pthread_cond_signal(&schedule_condition);
    pthread_mutex_unlock(&schedule_mutex);
    return 1;
}

int main_schedule_timer(JSContext * cx, uint32 timeout, uint32 tag) {
    pthread_mutex_lock(&schedule_mutex);
    if (schedule_outstanding == MAX_SCHEDULE_OUTSTANDING) {
        pthread_mutex_unlock(&schedule_mutex);
        // TODO block until space
        return JS_FALSE;
    }

    Continuation * cnt = (Continuation *)malloc(sizeof(Continuation));
    cnt->cx = cx;
    cnt->data = (jsval *)malloc(sizeof(jsval));
    cnt->tag = (jsval *)malloc(sizeof(jsval));
    cnt->cast = cast_wait;
    cnt->intval = timeout;
    cnt->next = NULL;

    JS_NewNumberValue(cx, timeout, cnt->data);
    JS_AddValueRoot(cx, cnt->data);

    JS_NewNumberValue(cx, tag, cnt->tag);
    JS_AddValueRoot(cx, cnt->tag);

    schedule[schedule_outstanding++] = cnt;
    pthread_cond_signal(&schedule_condition);
    pthread_mutex_unlock(&schedule_mutex);
    return 1;
}

// ****************************************************
// Callbacks from c into the actor scheduler.
// ****************************************************

static void timer_callback(EV_P_ ev_timer *w, int revents) {
    Continuation * cont = (Continuation *)w->data;

    schedule_actor(cont);
    free(w);
}

static void io_callback(EV_P_ ev_io *w, int revents) {
    Continuation * cont = (Continuation *)w->data;

    schedule_actor(cont);
    ev_io_stop(ev_default_loop(0), w);
    free(w);
}

// ****************************************************
// api exposed to Actors:
//  fileno = connect(host, port)
//  close(fileno)
//  schedule_timer(timeout, request_id)
//  schedule_read(fileno, howmuch, request_id)
//  schedule_write(fileno, towrite, request_id)
// ****************************************************

// *** fileno = connect(host, port)
JSBool servo_connect(JSContext *cx, uintN argc, jsval *vp) {
    JSString *string;
    char host[256];
    int port;
    int fileno = 0;
    struct hostent *hostrec;

    int result = JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "Si", &string, &port);
    if (!result) {
        JS_ReportError(cx, "Invalid arguments to connect. Expected host, port");
        return JS_FALSE;
    }
    JS_EncodeStringToBuffer(string, host, 256);
    host[JS_GetStringLength(string)] = NULL;
    hostrec = gethostbyname(host);
    if (!hostrec) {
        JS_ReportError(cx, "Bad host");
        return JS_FALSE;
    }
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    bcopy(hostrec->h_addr, &(address.sin_addr.s_addr), hostrec->h_length);

    fileno = socket(PF_INET, SOCK_STREAM, 0);
    fcntl(fileno, F_SETFL, O_NONBLOCK);

    if (connect(fileno, (struct sockaddr *)&address, sizeof(address)) == -1) {
        if (errno != EISCONN && errno != EALREADY && errno != EINPROGRESS) {
            JS_ReportError(cx, "Error connecting %d", errno);
            return JS_FALSE;
        }
    }
    JS_SET_RVAL(cx, vp, INT_TO_JSVAL(fileno));
    return JS_TRUE;
}

// close(fileno)
JSBool servo_close(JSContext *cx, uintN argc, jsval *vp) {
    int fileno = 0;
    int result = JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "i", &fileno);
    if (!result) {
        JS_ReportError(cx, "Invalid arguments to close. Expected fileno\n");
        return JS_FALSE;
    }
    result = close(fileno);
    if (result == -1) {
        JS_ReportError(cx, "Close failed\n");
        return JS_FALSE;    
    }
    return JS_TRUE;
}

// schedule_timer(timeout, request_id)
JSBool servo_schedule_timer(JSContext *cx, uintN argc, jsval *vp) {
    uint32 timeout;
    uint32 tag;
    int result = JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "uu", &timeout, &tag);
    if (!result) {
        JS_ReportError(cx, "Invalid timeout\n");
        return JS_FALSE;
    }

    main_schedule_timer(cx, timeout, tag);

    return JS_TRUE;
}

// schedule_read(fileno, howmuch, request_id)
JSBool servo_schedule_read(JSContext *cx, uintN argc, jsval *vp) {
    int fileno = 0;
    int howmuch = 0;
    int tag = 0;

    int result = JS_ConvertArguments(
        cx, argc, JS_ARGV(cx, vp), "uu/u", &fileno, &howmuch, &tag);
    if (!result) {
        JS_ReportError(cx, "Invalid arguments: expected fileno, howmuch");
        return JS_FALSE;
    }

    jsval * howmuchval = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, howmuch, howmuchval);
    JS_AddValueRoot(cx, howmuchval);

    jsval * tagval = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, tag, tagval);
    JS_AddValueRoot(cx, tagval);

    main_schedule_io(cx, cast_recv, howmuchval, tagval, (uint32)fileno);

    return JS_TRUE;
}

// schedule_write(fileno, towrite, request_id)
JSBool servo_schedule_write(JSContext *cx, uintN argc, jsval *vp) {
    int fileno = 0;
    JSString *data;
    int tag = 0;

    int result = JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "uS/u", &fileno, &data, &tag);
    if (!result) {
        JS_ReportError(cx, "Invalid arguments: expected fileno, data");
        return JS_FALSE;
    }

    jsval * tagval = (jsval *)malloc(sizeof(tagval));
    JS_NewNumberValue(cx, tag, tagval);
    JS_AddValueRoot(cx, tagval);

    jsval * dataval = (jsval *)malloc(sizeof(jsval));
    *dataval = STRING_TO_JSVAL(data);
    JS_AddValueRoot(cx, dataval);

    main_schedule_io(cx, cast_send, dataval, tagval, (uint32)fileno);

    return JS_TRUE;
}

// ****************************************************
// Boilerplate embedding stuff
// ****************************************************

static JSBool global_resolve(JSContext *cx, JSObject *obj, jsid id, uintN flags, JSObject **objp) {
    JSBool resolved;

    if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
        return JS_FALSE;
    if (resolved) {
        *objp = obj;
        return JS_TRUE;
    }
    return JS_TRUE;
}

static JSClass global_class = {
    "global", JSCLASS_NEW_RESOLVE | JSCLASS_GLOBAL_FLAGS | JSCLASS_HAS_PRIVATE,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, (JSResolveOp)global_resolve, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

void report_error(JSContext *cx, const char *message, JSErrorReport *report) {
    fprintf(
        stderr, "%s@%u %s\n",
        report->filename ? report->filename : "<no filename>",
        (unsigned int) report->lineno, message);
}

static JSBool
servo_print(JSContext *cx, uintN argc, jsval *vp)
{
    jsval *argv;
    uintN i;
    JSString *str;
    char *bytes;

    argv = JS_ARGV(cx, vp);
    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        bytes = JS_EncodeString(cx, str);
        if (!bytes)
            return JS_FALSE;
        printf("%s%s", i ? " " : "", bytes);
        JS_free(cx, bytes);
    }
    printf("\n");
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

static JSFunctionSpec servo_global_functions[] = {
    JS_FS("socket_connect",   servo_connect,   2, 0),
    JS_FS("socket_close", servo_close, 1, 0),
    JS_FS("schedule_timer", servo_schedule_timer, 1, 0),
    JS_FS("schedule_read", servo_schedule_read, 1, 0),
    JS_FS("schedule_write", servo_schedule_write, 1, 0),
    JS_FN("print", servo_print, 0, 0),
    JS_FS_END
};

#if DEBUG_SPEW
JSTrapStatus SpewHook(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval, void *closure) {
    const char* filename = JS_GetScriptFilename(cx, script);
    uintN lineno = JS_PCToLineNumber(cx, script, pc);

    printf("%s %d\n", filename, lineno);

    return JSTRAP_CONTINUE;
}
#endif

// ****************************************************
// API for servo to start Actors.
// ****************************************************

JSContext *spawn(JSRuntime *rt, const char * filename) {
    pthread_mutex_lock(&actors_mutex);
    actors_outstanding++;
    pthread_mutex_unlock(&actors_mutex);

    JSContext *cx = JS_NewContext(rt, 8192);
    if (cx == NULL)
        return NULL;

    JS_SetContextPrivate(cx, 0);

    JS_SetContextThread(cx);
    JS_BeginRequest(cx);

    JS_SetOptions(cx, JSOPTION_VAROBJFIX | JSOPTION_JIT | JSOPTION_METHODJIT);
    JS_SetVersion(cx, JSVERSION_LATEST);
    JS_SetErrorReporter(cx, report_error);

    JSObject  *global = JS_NewCompartmentAndGlobalObject(cx, &global_class, NULL);
    if (global == NULL)
        return NULL;

    JS_SetGlobalObject(cx, global);
    if (!JS_InitStandardClasses(cx, global))
        return NULL;
    if (!JS_DefineFunctions(cx, global, servo_global_functions))
        return NULL;

    JSFunction * sentinel = JS_CompileFunction(
        cx, global, "_sentinel", 0, NULL, "null;", 5, "null", 0);

    FILE *the_file = fopen(filename, "r");
    if (!the_file)
        return NULL;
    fseek(the_file, 0, SEEK_END);
    int file_size = ftell(the_file);
    rewind(the_file);
    char *file_data = (char*) calloc(sizeof(char), file_size + 17);
    fread(file_data, 1, file_size, the_file);
    if(ferror(the_file))
        return NULL;
    fclose(the_file);
    memcpy(file_data + file_size, ";yield _sentinel;", 17);

    JSFunction * func = JS_CompileFunction(
        cx, global, "_main", 0, NULL, file_data, file_size + 17, filename, 0);
    if (func == NULL) {
        printf("null func\n");
        return NULL;
    }

    jsval window_object = OBJECT_TO_JSVAL(global);
    JS_SetProperty(cx, global, "window", &window_object);

    JSObject *navigator = JS_NewObject(cx, NULL, NULL, NULL);
    if (!navigator)
        return NULL;
    jsval navigator_object = OBJECT_TO_JSVAL(navigator);
    JS_SetProperty(cx, global, "navigator", &navigator_object);

    jsval rval;
    JSString *str;
    JSBool ok;

    JSScript *actormain = JS_CompileFile(cx, global, "actormain.js");
    if (!actormain)
        return NULL;

    ok = JS_ExecuteScript(cx, global, actormain, &rval);
    if (!ok)
        return NULL;

    JSScript *domjs = JS_CompileFile(cx, global, "deps/dom.js/dom.js");
    if (!domjs)
        return NULL;

    ok = JS_ExecuteScript(cx, global, domjs, &rval);
    if (!ok)
        return NULL;

    ok = JS_EvaluateScript(cx, global, "window.navigator.userAgent = 'servo 0.1a'", 41, "main", 0, &rval);

    JS_EndRequest(cx);
    JS_ClearContextThread(cx);

    schedule_cast(cx, NULL, NULL, NULL);
    return cx;
}

// Main actor dispatcher.
void * thread_main(void * runtime_in) {
    JSRuntime *rt = (JSRuntime *)runtime_in;

    jsval rval;
    JSString *str;
    JSBool ok;

    JSObject *sandbox;
    JSContext *runnable;
    Continuation *continuation;

    jsval * data;
    jsval * tag;
    jsval * cast;
    uint32 intval;

    while (1) {
        if (shutting_down) {
            return 0;
        }

        // ***************
        // *** Locate Actor
        pthread_mutex_lock(&runnables_mutex);
        if (!runnables_outstanding) {
            pthread_cond_wait(&runnables_condition, &runnables_mutex);
            pthread_mutex_unlock(&runnables_mutex);
            // Check to see if now shutting down.
            continue;
        }
        continuation = runnables[--runnables_outstanding];

        runnable = continuation->cx;
        data = continuation->data;
        tag = continuation->tag;
        cast = continuation->cast;
        intval = continuation->intval;

        // ***************
        // *** Reschedule
        pthread_mutex_lock(&reschedule_mutex);
        if (JS_GetContextThread(runnable)) {
            Continuation * resched = (Continuation *)JS_GetContextPrivate(runnable);
            while (resched->next) {
                resched = resched->next;
            }
            resched->next = continuation;
            pthread_mutex_unlock(&reschedule_mutex);
            pthread_mutex_unlock(&runnables_mutex);
            continue;
        }

        JS_SetContextPrivate(runnable, (void *)continuation);
        pthread_mutex_unlock(&reschedule_mutex);
        // *** Reschedule
        // ***************

        JS_SetContextThread(runnable);
        JS_BeginRequest(runnable);

        pthread_mutex_unlock(&runnables_mutex);
        // *** Locate Actor
        // ***************

        // *************************************************************
        sandbox = JS_GetGlobalObject(runnable);

        if (cast == cast_wait) {
            JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_data", data);
            int ok = JS_EvaluateScript(runnable, JS_GetGlobalObject(runnable), "cast('wait', _data)", 19, "main", 0, &rval);
            if (!ok) {
                printf("cast did not return ok?!\n");
            }
            JS_RemoveValueRoot(runnable, data);        
        } else if (cast == cast_send) {
            JSString *to_write_str = JSVAL_TO_STRING(*data);
            int size = JS_GetStringLength(to_write_str);
            char * to_write = JS_EncodeString(runnable, to_write_str);
            int size_sent = send(intval, to_write, size, 0);
            if (size_sent == -1) {
                printf("Error writing to fd %d (%d)\n", intval, errno);
            } else {
                jsval *fd = (jsval *)malloc(sizeof(jsval));
                JS_NewNumberValue(runnable, intval, fd);
                JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_fd", fd);
                jsval *sent_js = (jsval *)malloc(sizeof(jsval));
                JS_NewNumberValue(runnable, size_sent, sent_js);
                JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_sent", sent_js);
                JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_tag", tag);
                ok = JS_EvaluateScript(runnable, JS_GetGlobalObject(runnable), "cast('send', [_fd, _sent, _tag])", 32, "main", 0, &rval);
                if (!ok) {
                    printf("cast did not return ok?!\n");
                }
            }
        } else if (cast == cast_recv) {
            int32 howmuch;
            JS_ValueToInt32(runnable, *data, &howmuch);
            char * buffer = (char *)malloc(howmuch);

            uint32 amountread = recv(intval, buffer, howmuch, 0);
            buffer[amountread] = NULL;
            jsval the_string = STRING_TO_JSVAL(JS_NewStringCopyN(runnable, buffer, amountread));
            jsval *fd = (jsval *)malloc(sizeof(jsval));
            JS_NewNumberValue(runnable, intval, fd);

            JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_fd", fd);
            JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_data", &the_string);
            if (tag) {
            JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_tag", tag);
                ok = JS_EvaluateScript(runnable, JS_GetGlobalObject(runnable), "cast('recv', [_fd, _data, _tag])", 32, "main", 0, &rval);
            } else {
                ok = JS_EvaluateScript(runnable, JS_GetGlobalObject(runnable), "cast('recv', [_fd, _data])", 26, "main", 0, &rval);
            }
            if (!ok) {
                printf("cast did not return ok?!\n");
            }

            JS_RemoveValueRoot(runnable, data);
            JS_RemoveValueRoot(runnable, tag);
        } else if (cast == cast_url) {
            JS_SetProperty(runnable, JS_GetGlobalObject(runnable), "_data", data);
            int ok = JS_EvaluateScript(runnable, JS_GetGlobalObject(runnable), "cast('url', _data)", 18, "main", 0, &rval);
            if (!ok) {
                printf("cast did not return ok?!\n");
            }
            JS_RemoveValueRoot(runnable, data);        
        }

        ok = JS_EvaluateScript(runnable, sandbox, "resume()", 8, "main", 0, &rval);
        if (!ok) {
            printf("resume did not return ok?!\n");
        }

        JS_EndRequest(runnable);
        JS_ClearContextThread(runnable);

        pthread_mutex_lock(&reschedule_mutex);
        if (continuation->next) {
            schedule_actor(continuation->next);
        }
        pthread_mutex_unlock(&reschedule_mutex);

        free(continuation);

        if (JSVAL_IS_NULL(rval)) {
            // The Actor has finished, can destroy it's context.
            pthread_mutex_lock(&actors_mutex);
            JS_DestroyContext(runnable);
            actors_outstanding--;
            if (!actors_outstanding) {
                pthread_cond_signal(&schedule_condition);
            }
            pthread_mutex_unlock(&actors_mutex);
        }
        // *************************************************************
    }
}

// Main servo program.
// Read urls from command line arguments, and start one
// servo.js actor per url.

int main(int argc, const char *argv[]) {
    int ok;
    pthread_t threads[NUM_THREADS];
    struct ev_loop * loop = ev_default_loop(0); 
    JSRuntime *rt = JS_NewRuntime(RUNTIME_SIZE);
    if (rt == NULL)
        return 1;

#if DEBUG_SPEW
    JS_SetInterrupt(rt, &SpewHook, NULL);
#endif

    JSContext *cx = JS_NewContext(rt, 8192);
    if (cx == NULL)
        return NULL;

    JS_SetOptions(cx, JSOPTION_VAROBJFIX | JSOPTION_JIT | JSOPTION_METHODJIT);
    JS_SetVersion(cx, JSVERSION_LATEST);
    JS_SetErrorReporter(cx, report_error);

    JSObject  *global = JS_NewCompartmentAndGlobalObject(cx, &global_class, NULL);
    if (global == NULL)
        return NULL;

    JS_SetGlobalObject(cx, global);
    if (!JS_InitStandardClasses(cx, global))
        return NULL;
    if (!JS_DefineFunctions(cx, global, servo_global_functions))
        return NULL;

    cast_wait = (jsval *)malloc(sizeof(jsval));
    cast_send = (jsval *)malloc(sizeof(jsval));
    cast_recv = (jsval *)malloc(sizeof(jsval));
    cast_url = (jsval *)malloc(sizeof(jsval));

    JS_SetContextThread(cx);
    JS_BeginRequest(cx);

    ok = JS_EvaluateScript(cx, JS_GetGlobalObject(cx), "'wait'", 6, "main", 0, cast_wait);
    ok = JS_EvaluateScript(cx, JS_GetGlobalObject(cx), "'send'", 6, "main", 0, cast_send);
    ok = JS_EvaluateScript(cx, JS_GetGlobalObject(cx), "'recv'", 6, "main", 0, cast_recv);
    ok = JS_EvaluateScript(cx, JS_GetGlobalObject(cx), "'url'", 5, "main", 0, cast_url);

    JS_AddValueRoot(cx, cast_wait);
    JS_AddValueRoot(cx, cast_send);
    JS_AddValueRoot(cx, cast_recv);
    JS_AddValueRoot(cx, cast_url);

    for (int i = 1; i < argc; i++) {
        JSContext * new_actor = spawn(rt, "servo.js");
        if (!new_actor)
            return 1;
        
        jsval urlstr = STRING_TO_JSVAL(JS_NewStringCopyZ(new_actor, argv[i]));
        JS_AddValueRoot(cx, &urlstr);
        schedule_cast(new_actor, cast_url, &urlstr, NULL);
    }
    if (argc == 1) {
        JSContext * new_actor = spawn(rt, "servo.js");
        if (!new_actor)
            return 1;
        
        jsval urlstr = STRING_TO_JSVAL(JS_NewStringCopyZ(new_actor, "http://localhost/"));
        JS_AddValueRoot(cx, &urlstr);
        schedule_cast(new_actor, cast_url, &urlstr, NULL);
    }
    JS_EndRequest(cx);
    JS_ClearContextThread(cx);

    for (int i = 0; i < NUM_THREADS; i++) {
        ok = pthread_create(&threads[i], NULL, thread_main, (void *)&rt);
        if (ok != 0) {
            printf("pthread_create had an error %d\n", ok);
        }
    }

    while (1) {
        pthread_mutex_lock(&schedule_mutex);
        if (!schedule_outstanding) {
            pthread_cond_wait(&schedule_condition, &schedule_mutex);
        }
        while (schedule_outstanding) {
            Continuation *to_schedule = schedule[--schedule_outstanding];
            if (to_schedule->cast == cast_wait) {
                ev_timer *timer = (ev_timer *)malloc(sizeof(ev_timer));
                ev_timer_init(timer, timer_callback, to_schedule->intval / 1000.0, 0.);
                timer->data = (void *)to_schedule;
                ev_timer_start(loop, timer);
            } else if (to_schedule->cast == cast_send) {
                ev_io *io = (ev_io *)malloc(sizeof(ev_io));
                ev_io_init(io, io_callback, to_schedule->intval, EV_WRITE);
                io->data = (void *)to_schedule;
                ev_io_start(loop, io);
            } else if (to_schedule->cast == cast_recv) {
                ev_io *io = (ev_io *)malloc(sizeof(ev_io));
                ev_io_init(io, io_callback, to_schedule->intval, EV_READ);
                io->data = (void *)to_schedule;
                ev_io_start(loop, io);
            } else {
                printf("Ignored unknown schedule event\n");
            }
        }
        pthread_mutex_unlock(&schedule_mutex);

        pthread_mutex_lock(&actors_mutex);
        if (!actors_outstanding) {
            pthread_mutex_unlock(&actors_mutex);
            break;
        }
        pthread_mutex_unlock(&actors_mutex);
        
        ev_run(loop, 0);
    }

    shutting_down = 1;
    pthread_cond_broadcast(&runnables_condition);

    /* Clean things up and shut down SpiderMonkey. */
    JS_DestroyRuntime(rt);
    JS_ShutDown();

    for (int j = 0; j < NUM_THREADS; j++) {
        pthread_cancel(threads[j]);
    }

    pthread_exit(NULL);

    return 0;
}
