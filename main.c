#include "jsapi.h"
#include "ev.h"
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

// ****************************************************
// Managing the set of Actors that are ready to run
// ****************************************************

#define MAX_RUNNABLES_OUTSTANDING 4096
#define RUNTIME_SIZE 128 * 1024 * 1024

typedef struct _continuation {
    JSContext * cx;
    jsval * data;
    jsval * tag;
} Continuation;

static Continuation *runnables[MAX_RUNNABLES_OUTSTANDING];
static int runnables_outstanding = 0;

JSBool schedule_actor(Continuation *cont) {
    if (runnables_outstanding == MAX_RUNNABLES_OUTSTANDING) {
      return JS_FALSE;
    }
    runnables[runnables_outstanding++] = cont;

    ev_break(ev_default_loop(0), EVBREAK_ONE);
    return JS_TRUE;
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
        JS_ReportError(cx, "Invalid arguments to close. Expected fileno");
        return JS_FALSE;
    }
    result = close(fileno);
    if (result == -1) {
        JS_ReportError(cx, "Close failed");
        return JS_FALSE;    
    }
    return JS_TRUE;
}

// schedule_timer(timeout, request_id)
static void timer_callback(EV_P_ ev_timer *w, int revents) {
    Continuation * cont = (Continuation *)w->data;
    jsval rval;

    JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_data", cont->data);
    int ok = JS_EvaluateScript(cont->cx, JS_GetGlobalObject(cont->cx), "cast('wait', _data)", 19, "main", 0, &rval);
    JS_RemoveValueRoot(cont->cx, cont->data);

    schedule_actor(cont);
    free(w);
}

JSBool servo_schedule_timer(JSContext *cx, uintN argc, jsval *vp) {
    jsdouble timeout = 0.1;
    jsdouble raw;
    int result = JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "dd", &timeout, &raw);
    if (!result) {
        JS_ReportError(cx, "Invalid timeout");
        return JS_FALSE;
    }

    Continuation * cnt = (Continuation *)malloc(sizeof(Continuation));
    cnt->cx = cx;
    cnt->data = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, raw, cnt->data);
    JS_AddValueRoot(cx, cnt->data);

    ev_timer *timer = (ev_timer *)malloc(sizeof(ev_timer));
    ev_timer_init(timer, timer_callback, timeout, 0.);
    timer->data = (void *)cnt;
    ev_timer_start(ev_default_loop(0), timer);
    return JS_TRUE;
}

// schedule_read(fileno, howmuch, request_id)
static void read_callback(EV_P_ ev_io *w, int revents) {
    Continuation * cont = (Continuation *)w->data;
    JSContext *cx = cont->cx;
    int32 howmuch;
    int32 tag;
    jsval rval;

    JS_ValueToInt32(cx, *cont->data, &howmuch);

    char * buffer = (char *)malloc(howmuch);

    uint32 amountread = recv(w->fd, buffer, howmuch, 0);
    buffer[amountread] = NULL;
    jsval the_string = STRING_TO_JSVAL(JS_NewStringCopyN(cx, buffer, amountread));
    jsval *fd = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, w->fd, fd);

    JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_fd", fd);
    JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_data", &the_string);
    JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_tag", cont->tag);
    int ok = JS_EvaluateScript(cont->cx, JS_GetGlobalObject(cont->cx), "cast('recv', [_fd, _data, _tag])", 32, "main", 0, &rval);

    JS_RemoveValueRoot(cx, cont->data);

    schedule_actor(cont);

    ev_io_stop(ev_default_loop(0), w);
    free(w);
}

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
    Continuation * cnt = (Continuation *)malloc(sizeof(Continuation));
    cnt->cx = cx;
    cnt->data = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, howmuch, cnt->data);
    JS_AddValueRoot(cx, cnt->data);
    cnt->tag = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, tag, cnt->tag);
    JS_AddValueRoot(cx, cnt->tag);

    ev_io *io = (ev_io *)malloc(sizeof(ev_io));
    ev_io_init(io, read_callback, fileno, EV_READ);
    io->data = (void *)cnt;
    ev_io_start(ev_default_loop(0), io);
    return JS_TRUE;
}

// schedule_write(fileno, towrite, request_id)
static void write_callback(EV_P_ ev_io *w, int revents) {
    Continuation * cont = (Continuation *)w->data;
    JSContext *cx = cont->cx;
    JSString *to_write_str = (JSString *)cont->data;
    int size = JS_GetStringLength(to_write_str);
    char * to_write = JS_EncodeString(cx, to_write_str);
    int size_sent = send(w->fd, to_write, size, 0);
    if (size_sent == -1) {
        printf("Error writing to fd %d (%d)\n", w->fd, errno);
        ev_io_stop(ev_default_loop(0), w);
        free(w);
    } else if (size_sent == size) {
        jsval rval;
        jsval *fd = (jsval *)malloc(sizeof(jsval));
        JS_NewNumberValue(cx, w->fd, fd);
        JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_fd", fd);
        jsval *sent_js = (jsval *)malloc(sizeof(jsval));
        JS_NewNumberValue(cx, size_sent, sent_js);
        JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_sent", sent_js);
        JS_SetProperty(cont->cx, JS_GetGlobalObject(cont->cx), "_tag", cont->tag);
        int ok = JS_EvaluateScript(cont->cx, JS_GetGlobalObject(cont->cx), "cast('send', [_fd, _sent, _tag])", 32, "main", 0, &rval);
        
        schedule_actor(cont);

        ev_io_stop(ev_default_loop(0), w);
        free(w);
    } else {
        printf("notallsent fail! %d %d\n", size, size_sent);
        ev_io_stop(ev_default_loop(0), w);
        free(w);
        // TODO really need to figure out a way to trigger this for testing.
        //cont->data = (jsval *)JS_NewStringCopyN(cx, to_write + size_sent, size - size_sent);
    }
}

JSBool servo_schedule_write(JSContext *cx, uintN argc, jsval *vp) {
    int fileno = 0;
    int tag = 0;
    JSString *data;

    int result = JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "uS/u", &fileno, &data, &tag);
    if (!result) {
        JS_ReportError(cx, "Invalid arguments: expected fileno, data");
        return JS_FALSE;
    }
    Continuation * cnt = (Continuation *)malloc(sizeof(Continuation));
    cnt->cx = cx;
    cnt->data = (jsval *)data;

    cnt->tag = (jsval *)malloc(sizeof(jsval));
    JS_NewNumberValue(cx, tag, cnt->tag);
    JS_AddValueRoot(cx, cnt->tag);

    ev_io *io = (ev_io *)malloc(sizeof(ev_io));
    ev_io_init(io, write_callback, fileno, EV_WRITE);
    io->data = (void *)cnt;
    ev_io_start(ev_default_loop(0), io);
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
Print(JSContext *cx, uintN argc, jsval *vp)
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
    JS_FN("print", Print, 0, 0),
    JS_FS_END
};

/*
JSTrapStatus SpewHook(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval, void *closure) {
    const char* filename = JS_GetScriptFilename(cx, script);
    uintN lineno = JS_PCToLineNumber(cx, script, pc);

    printf("%s %d\n", filename, lineno);

    return JSTRAP_CONTINUE;
}
*/

// ****************************************************
// API for servo to start Actors.
// ****************************************************

JSContext *spawn(JSRuntime *rt, const char * filename) {
    JSContext *cx = JS_NewContext(rt, 8192);
    if (cx == NULL)
        return NULL;
    JS_SetOptions(cx, JSOPTION_VAROBJFIX | JSOPTION_JIT | JSOPTION_METHODJIT);
    JS_SetVersion(cx, JSVERSION_LATEST);
    JS_SetErrorReporter(cx, report_error);

//    JS_SetInterrupt(cx->runtime, &SpewHook, NULL);

    JSObject  *global = JS_NewCompartmentAndGlobalObject(cx, &global_class, NULL);
    if (global == NULL)
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

    JS_SetGlobalObject(cx, global);
    if (!JS_InitStandardClasses(cx, global))
        return NULL;
    if (!JS_DefineFunctions(cx, global, servo_global_functions))
        return NULL;

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

/*    JSScript *jquery = JS_CompileFile(cx, global, "jquery-1.6.4.js");
    if (!jquery)
        return NULL;
    ok = JS_ExecuteScript(cx, global, jquery, &rval);
    if (!ok) {
        printf("fail!\n");
        return NULL;
    }

    JSScript *qunit = JS_CompileFile(cx, global, "qunit.js");
    if (!qunit)
        return NULL;
    ok = JS_ExecuteScript(cx, global, qunit, &rval);
    if (!ok) {
        printf("fail.\n");
        return NULL;
    }

    // set up qunit for use from the command line
    ok = JS_EvaluateScript(cx, global, "QUnit.init(); QUnit.config.blocking = false; QUnit.config.autorun = true; QUnit.config.updateRate = 0; QUnit.log = function(res, msg) { print(res ? 'PASS' : 'FAIL', msg === undefined ? '' : msg); }", 197, "main", 0, &rval);

    JSScript *core = JS_CompileFile(cx, global, "core.js");
    if (!core)
        return NULL;
    ok = JS_ExecuteScript(cx, global, core, &rval);
    if (!ok) {
        printf("fail\n");
        return NULL;
    }

    // fire the document loaded event
    ok = JS_EvaluateScript(cx, global, "var event = document.createEvent('customevent'); event.initEvent('DOMContentLoaded', false, true); document.dispatchEvent(event); delete event; 1", 145, "main", 0, &rval);
*/

    Continuation *cont = (Continuation *)malloc(sizeof(Continuation));
    cont->cx = cx;
    cont->data = NULL;

    schedule_actor(cont);
    return cx;
}

// Main servo program.
// Read urls from command line arguments, and start one
// servo.js actor per url.

int main(int argc, const char *argv[]) {
    struct ev_loop *loop = ev_default_loop(0);

    jsval rval;
    JSString *str;
    JSBool ok;

    JSObject *sandbox;
    JSContext *runnable;
    Continuation *continuation;

    JSRuntime *rt = JS_NewRuntime(RUNTIME_SIZE);
    if (rt == NULL)
        return 1;

    for (int i = 1; i < argc; i++) {
        JSContext * new_actor = spawn(rt, "servo.js");
        if (!new_actor)
            return 1;

        jsval url = STRING_TO_JSVAL(JS_NewStringCopyZ(new_actor, argv[i]));
        JS_SetProperty(
            new_actor, JS_GetGlobalObject(new_actor),
            "_url", &url);
        ok = JS_EvaluateScript(
            new_actor, JS_GetGlobalObject(new_actor),
            "cast('url', _url)", 17, "main", 0, &rval);
    }

    // *************************************************************
    while (runnables_outstanding) {
        while (runnables_outstanding) {
            continuation = runnables[--runnables_outstanding];
            runnable = continuation->cx;
            free(continuation);

            sandbox = JS_GetGlobalObject(runnable);
            ok = JS_EvaluateScript(runnable, sandbox, "resume()", 8, "main", 0, &rval);
            if (!ok) {
                return 1;
            }
            // *************************************************************
        }
        ev_run(loop, 0);
    }

    /* Clean things up and shut down SpiderMonkey. */
    JS_DestroyRuntime(rt);
    JS_ShutDown();
    return 0;
}
