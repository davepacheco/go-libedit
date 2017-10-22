#include <histedit.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <setjmp.h>
#include <termios.h>
#include <signal.h>
#include <stdlib.h>

#include "c_editline.h"

char *go_libedit_emptycstring = (char*)"";
const char* go_libedit_mode_read = "r";
const char* go_libedit_mode_write = "w";
const char* go_libedit_mode_append = "a";
const char* go_libedit_locale1 = "en_US.UTF-8";
const char* go_libedit_locale2 = "C.UTF-8";

void go_libedit_set_string_array(char **ar, int p, char *s) {
    ar[p] = s;
}

int go_libedit_get_clientdata(EditLine *el) {
    void* p;
    el_get(el, EL_CLIENTDATA, &p);
    return (int)(intptr_t)p;
}

void go_libedit_set_clientdata(EditLine *el, int v) {
    void *p = (void*)(intptr_t) v;
    el_set(el, EL_CLIENTDATA, p);
}

/************** prompts **************/

static char *go_libedit_lprompt_str;
static char *go_libedit_rprompt_str;

static char* go_libedit_lprompt(EditLine *el) {
    return go_libedit_lprompt_str ? go_libedit_lprompt_str : "?";
}

static char* go_libedit_rprompt(EditLine *el) {
    return go_libedit_rprompt_str ? go_libedit_rprompt_str : "";
}

/************** signals **************/

// This logic replicates libedit's sig.c.
#define	ALLSIGS		\
	_DO(SIGINT)	\
	_DO(SIGTSTP)	\
	_DO(SIGQUIT)	\
	_DO(SIGHUP)	\
	_DO(SIGTERM)	\
	_DO(SIGCONT)	\
	_DO(SIGWINCH)
#define ALLSIGSNO	7

typedef struct {
    struct sigaction sig_action[ALLSIGSNO];
    sigset_t sig_set;
    volatile sig_atomic_t sig_no;
} go_libedit_signal_t;

static const int sighdl[] = {
#define	_DO(a)	(a),
	ALLSIGS
#undef	_DO
	- 1
};

static sigjmp_buf jmpbuf;

static void go_libedit_intr_handler(int signo) {
    siglongjmp(jmpbuf, 1);
}

static go_libedit_signal_t* go_libedit_sig_init() {
	size_t i;
	sigset_t oset;
	go_libedit_signal_t *sigcfg;

	sigcfg = malloc(sizeof(go_libedit_signal_t));
	if (!sigcfg)
	    return NULL;

	(void) sigemptyset(&sigcfg->sig_set);
#define	_DO(a) (void) sigaddset(&sigcfg->sig_set, a);
	ALLSIGS
#undef	_DO

	return sigcfg;
}

// Disable whatever signal handlers Go had set up, but keep them
// save for restore by libedit_sig_clr().
static void go_libedit_sig_set(EditLine *el, go_libedit_signal_t *sigcfg) {
	size_t i;
	sigset_t oset;
	struct sigaction osa, nsa;

	nsa.sa_flags = 0;
	sigemptyset(&nsa.sa_mask);

	(void) sigprocmask(SIG_BLOCK, &sigcfg->sig_set, &oset);

	for (i = 0; sighdl[i] != -1; i++) {
	    sigcfg->sig_action[i].sa_handler = SIG_ERR;
	    sigcfg->sig_action[i].sa_flags = 0;
	    sigemptyset(&sigcfg->sig_action[i].sa_mask);

	    nsa.sa_handler = (sighdl[i] == SIGINT) ? go_libedit_intr_handler : SIG_DFL;
	    if (sigaction(sighdl[i], &nsa, &osa) != -1)
		sigcfg->sig_action[i] = osa;
	}
	(void) sigprocmask(SIG_SETMASK, &oset, NULL);
}

// Restore the original signal handlers.
static void go_libedit_sig_clr(go_libedit_signal_t *sigcfg)
{
	size_t i;
	sigset_t oset;

	(void) sigprocmask(SIG_BLOCK, &sigcfg->sig_set, &oset);

	for (i = 0; sighdl[i] != -1; i++)
	    if (sigcfg->sig_action[i].sa_handler != SIG_ERR)
		(void)sigaction(sighdl[i], &sigcfg->sig_action[i], NULL);

	(void)sigprocmask(SIG_SETMASK, &oset, NULL);
}

/********** initialization ***********/

static unsigned char	 _el_rl_complete(EditLine *, int);

void go_libedit_rebind_ctrls(EditLine *e) {
    // Word completion - this has to go after loading the default
    // mappings.
    el_set(e, EL_ADDFN, "rl_complete",
	   "ReadLine compatible completion function",
	   _el_rl_complete);
    el_set(e, EL_BIND, "^I", "rl_complete", NULL);

    // Readline history search. People are used to this.
    el_set(e, EL_BIND, "^R", "em-inc-search-prev", NULL);
}

EditLine* go_libedit_init(char *appName, void** el_signal,
			  FILE* fin, FILE* fout, FILE *ferr) {
    // Prepare signal handling.
    go_libedit_signal_t *sigcfg = go_libedit_sig_init();
    if (!sigcfg) {
	return NULL;
    }
    (*el_signal) = sigcfg;

    // Create the editor.
    EditLine *e = el_init(appName, fin, fout, ferr);
    if (!e) {
	free(sigcfg);
	return NULL;
    }

    // Do we really want to edit?
    int editmode = 1;
    struct termios t;
    if (tcgetattr(fileno(fin), &t) != -1 && (t.c_lflag & ECHO) == 0)
	editmode = 0;
    if (!editmode)
	el_set(e, EL_EDITMODE, 0);

    // We'll let libedit do signalling, but we'll also ensure to
    // disable Go's signal handlers, because they need SA_ONSTACK to
    // be set and libedit doesn't do that on its own.
    el_set(e, EL_SIGNAL, 1);

    // Set up the prompt functions. Unfortunately we cannot use a real
    // callback into Go because Go doesn't like being called from an
    // alternate signal stack.
    el_set(e, EL_PROMPT, go_libedit_lprompt);
    el_set(e, EL_RPROMPT, go_libedit_rprompt);

    // Load the emacs keybindings by default. We need
    // to do that before the defaults are overridden below.
    el_set(e, EL_EDITOR, "emacs");

    go_libedit_rebind_ctrls(e);

    // Home/End keys.
    el_set(e, EL_BIND, "\\e[1~", "ed-move-to-beg", NULL);
    el_set(e, EL_BIND, "\\e[4~", "ed-move-to-end", NULL);
    el_set(e, EL_BIND, "\\e[7~", "ed-move-to-beg", NULL);
    el_set(e, EL_BIND, "\\e[8~", "ed-move-to-end", NULL);
    el_set(e, EL_BIND, "\\e[H", "ed-move-to-beg", NULL);
    el_set(e, EL_BIND, "\\e[F", "ed-move-to-end", NULL);

    // Delete/Insert keys.
    el_set(e, EL_BIND, "\\e[3~", "ed-delete-next-char", NULL);
    el_set(e, EL_BIND, "\\e[2~", "ed-quoted-insert", NULL);

    // Ctrl-left-arrow and Ctrl-right-arrow for word moving.
    el_set(e, EL_BIND, "\\e[1;5C", "em-next-word", NULL);
    el_set(e, EL_BIND, "\\e[1;5D", "ed-prev-word", NULL);
    el_set(e, EL_BIND, "\\e[5C", "em-next-word", NULL);
    el_set(e, EL_BIND, "\\e[5D", "ed-prev-word", NULL);
    el_set(e, EL_BIND, "\\e\\e[C", "em-next-word", NULL);
    el_set(e, EL_BIND, "\\e\\e[D", "ed-prev-word", NULL);

    // Read the settings from the configuration file.
    el_source(e, NULL);

    return e;
}

/************** history **************/

History* go_libedit_setup_history(EditLine *el, int maxEntries, int dedup) {
    if (!el) {
	errno = EINVAL;
	return NULL;
    }

    History *h = history_init();
    if (!h)
	return NULL;

    HistEvent ev;
    history(h, &ev, H_SETSIZE, maxEntries);
    history(h, &ev, H_SETUNIQUE, dedup);

    el_set(el, EL_HIST, history, h);
    return h;
}

static int readwrite_history(History *h, int op, char *filename) {
    if (!h || !filename) {
	errno = EINVAL;
	return -1;
    }
    errno = 0;
    HistEvent ev;
    int res;
    if ((res = history(h, &ev, op, filename)) == -1) {
	if (!errno)
	    errno = EINVAL;
	return -1;
    }
    return res;
}

int go_libedit_read_history(History *h, char *filename) {
    return readwrite_history(h, H_LOAD, filename);
}

int go_libedit_write_history(History *h, char *filename) {
    return readwrite_history(h, H_SAVE, filename);
}

int go_libedit_add_history(History *h, char *line) {
    return readwrite_history(h, H_ENTER, line);
}


/************* completion ************/

// We can't use rl_complete directly because that uses the readline
// emulation's own EditLine instance, and here we want to use our
// own. So basically re-implement on top of editline's internal
// fn_complete function.

int
fn_complete(EditLine *el,
	    char *(*complet_func)(const char *, int),
	    char **(*attempted_completion_function)(const char *, int, int),
	    const wchar_t *word_break, const wchar_t *special_prefixes,
	    const char *(*app_func)(const char *), size_t query_items,
	    int *completion_type, int *over, int *point, int *end,
	    const wchar_t *(*find_word_start_func)(const wchar_t *, const wchar_t *),
	    wchar_t *(*dequoting_func)(const wchar_t *),
	    char *(*quoting_func)(const char *));
static const wchar_t break_chars[] = L" \t\n\"\\'`@$><=;|&{(";


// In an unfortunate turn of circumstances, editline's fn_complete
// API does not pass the EditLine instance nor the clientdata field
// to the attempted_completion_function, yet we really want this.
// So we'll pass it as a hidden argument via a global variable.
// This effectively makes the entire library thread-unsafe. :'-(

static int global_instance;

static char **wrap_autocomplete(const char *word, int unused1, int unused2) {
    return go_libedit_getcompletions(global_instance, (char*)word);
}

static const char *_rl_completion_append_character_function(const char *_) {
    static const char *sp = " ";
    return sp;
}

static unsigned char _el_rl_complete(EditLine *el, int ch) {
    int avoid_filename_completion = 1;

    // Urgh...
    global_instance = go_libedit_get_clientdata(el);

    return (unsigned char)fn_complete(
	el,
	NULL /* complet_func */,
	wrap_autocomplete /* attempted_completion_function */,
	break_chars /* word_break */,
	NULL /* special_prefixes */,
	_rl_completion_append_character_function /* app_func */,
	100 /* query_items */,
	NULL /* completion_type */,
	&avoid_filename_completion /* over */,
	NULL /* point */,
	NULL /* end */,
	NULL /* find_word_start_func */,
	NULL /* dequoting_func */,
	NULL /* quoting_func */
	);
}


/*************** el_gets *************/

void *go_libedit_gets(EditLine *el, char *lprompt, char *rprompt,
		      void *p_sigcfg, int *count, int *interrupted, int widechar) {
    void *ret = NULL;
    int saveerr = 0;

    // Save and clear Go's signal handlers, set up our own for SIGINT / Ctrl+C.
    go_libedit_signal_t *sigcfg = (go_libedit_signal_t*)p_sigcfg;
    go_libedit_sig_set(el, sigcfg);

    // Prepare to be interrupted with SIGINT (Ctrl+C).
    if (sigsetjmp(jmpbuf, 1)) {
	saveerr = EINTR;
	*interrupted = 1;
	ret = NULL;
	goto restoresig;
    }

    // Install the prompts.
    go_libedit_lprompt_str = lprompt;
    go_libedit_rprompt_str = rprompt;

    // Read the line.
    if (widechar) {
	ret = (void *)el_wgets(el, count);
    } else {
	ret = (void *)el_gets(el, count);
    }
    saveerr = errno;

restoresig:
    go_libedit_lprompt_str = NULL;
    go_libedit_rprompt_str = NULL;

    // Remove our signal handler, restore Go's.
    go_libedit_sig_clr(sigcfg);

restore:
    // Restore errno.
    errno = saveerr;
    return ret;
}