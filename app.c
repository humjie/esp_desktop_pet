#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <json-c/json.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tk.h>
#include <unistd.h>

extern char **environ;

static struct json_object *quotes;
static size_t count;
static size_t current;
static int has_current;

static const char *field(size_t index, const char *key) {
    struct json_object *value = NULL;
    json_object_object_get_ex(json_object_array_get_idx(quotes, index), key, &value);
    return json_object_get_string(value);
}

static int load_quotes(void) {
    quotes = json_object_from_file("quotes.json");
    if (!quotes || !json_object_is_type(quotes, json_type_array) ||
        !(count = json_object_array_length(quotes))) {
        fputs("quotes.json must contain a nonempty JSON array.\n", stderr);
        return 0;
    }
    const char *keys[] = {"text", "category", "source"};
    for (size_t i = 0; i < count; i++) {
        struct json_object *quote = json_object_array_get_idx(quotes, i);
        for (size_t k = 0; k < 3; k++) {
            struct json_object *value = NULL;
            if (!json_object_is_type(quote, json_type_object) ||
                !json_object_object_get_ex(quote, keys[k], &value) ||
                !json_object_is_type(value, json_type_string)) {
                fprintf(stderr, "Quote %zu needs a string for %s.\n", i + 1, keys[k]);
                return 0;
            }
            const char *text = json_object_get_string(value);
            while (*text && isspace((unsigned char)*text)) text++;
            if (!*text || strlen(json_object_get_string(value)) !=
                          (size_t)json_object_get_string_len(value)) {
                fprintf(stderr, "Quote %zu: %s is empty or contains a null byte.\n",
                        i + 1, keys[k]);
                return 0;
            }
        }
    }
    return 1;
}

static int export_markdown(void) {
    FILE *file = fopen("quotes.md", "w");
    if (!file) { perror("quotes.md"); return 0; }
    int ok = fprintf(file, "# Quotes\n") >= 0;
    for (size_t i = 0; i < count && ok; i++) {
        ok = fprintf(file, "\n## %zu\n\n%s\n\n**Category:** %s\n\n**Source:** %s\n",
                     i + 1, field(i, "text"), field(i, "category"),
                     field(i, "source")) >= 0;
    }
    if (fclose(file) != 0) ok = 0;
    if (!ok) fputs("Could not write quotes.md.\n", stderr);
    return ok;
}

static int next_card(ClientData data, Tcl_Interp *interp, int argc,
                     Tcl_Obj *const argv[]) {
    (void)data; (void)argc; (void)argv;
    if (!has_current) current = (size_t)rand() % count;
    else if (count > 1) current = (current + 1 + (size_t)rand() % (count - 1)) % count;
    has_current = 1;
    /* Set variables as data, so quote text is never evaluated as Tcl code. */
    Tcl_SetVar(interp, "quote_text", field(current, "text"), TCL_GLOBAL_ONLY);
    Tcl_SetVar(interp, "category", field(current, "category"), TCL_GLOBAL_ONLY);
    return TCL_OK;
}

static int open_source(ClientData data, Tcl_Interp *interp, int argc,
                       Tcl_Obj *const argv[]) {
    (void)data; (void)argc; (void)argv;
    const char *source = field(current, "source");
    if (strncmp(source, "https://", 8) && strncmp(source, "http://", 7)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Source must be an HTTP(S) URL.", -1));
        return TCL_ERROR;
    }
    char *args[] = {"xdg-open", (char *)source, NULL};
    pid_t child;
    int error = posix_spawnp(&child, "xdg-open", NULL, NULL, args, environ);
    if (error) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(strerror(error), -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

int main(int argc, char **argv) {
    int export = argc == 2 && !strcmp(argv[1], "--export-md");
    int check = argc == 2 && !strcmp(argv[1], "--check");
    if (argc > 1 && !export && !check) {
        fputs("Usage: ./run.sh [--export-md | --check]\n", stderr);
        return 1;
    }
    if (!load_quotes()) {
        if (quotes) json_object_put(quotes);
        return 1;
    }
    if (export || check) {
        int ok = export ? export_markdown() : 1;
        if (check) printf("Loaded %zu quotes.\n", count);
        json_object_put(quotes);
        return ok ? 0 : 1;
    }
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    /* Automatically reap the source opener without a polling timer. */
    signal(SIGCHLD, SIG_IGN);
    Tcl_FindExecutable(argv[0]);
    Tcl_Interp *interp = Tcl_CreateInterp();
    int ok = Tcl_Init(interp) == TCL_OK && Tk_Init(interp) == TCL_OK;
    if (ok) {
        Tcl_CreateObjCommand(interp, "nextCard", next_card, NULL, NULL);
        Tcl_CreateObjCommand(interp, "openSource", open_source, NULL, NULL);
        ok = Tcl_EvalFile(interp, "ui.tcl") == TCL_OK;
    }
    if (ok) Tk_MainLoop();
    else fprintf(stderr, "Could not open Quote Cards: %s\n", Tcl_GetStringResult(interp));
    Tcl_DeleteInterp(interp);
    Tcl_Finalize();
    json_object_put(quotes);
    return ok ? 0 : 1;
}
