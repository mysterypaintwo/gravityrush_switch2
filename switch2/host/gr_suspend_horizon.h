/* Suspend and resume on the Switch 2, as an official game does: when the HOME menu opens or the
 * console goes to sleep, the whole game freezes (every thread, the sound and the GPU work it
 * submits) and continues where it was when it is back in focus, with the time it spent
 * suspended taken out of its clocks. */
#pragma once
#include <stdbool.h>

/* Called once from the main thread, before any other thread is created. The main thread
 * becomes the applet thread: gr_suspend_run_applet_loop never returns until the game is to
 * exit, and it is the only thread that is not frozen. */
void gr_suspend_init(void);

/* Runs the applet message loop (focus, sleep, resume, exit requests). It never returns: it ends
 * the process when the system asks the game to exit (closing it from the HOME menu) or when a
 * thread calls exit (see gr_suspend_exit). */
void gr_suspend_run_applet_loop(void) __attribute__((noreturn));

/* Ends the process safely from any thread (exit, _exit and _Exit lead here too). */
void gr_suspend_exit(int code) __attribute__((noreturn));

/* Sound and other parts that talk to system services stop before the threads freeze and
 * start again after they resume. Called on the applet thread, with the game's threads frozen
 * (suspend) or about to continue (resume): they must not take locks the game's threads use. */
typedef void (*GrSuspendFn)(bool suspend);
void gr_suspend_add_listener(GrSuspendFn fn);

bool gr_suspend_is_frozen(void);

/* Runs on the applet thread just before the return to the homebrew loader, after every other
 * thread has ended:
 * releases what the homebrew loader needs back (guest memory, the JIT, the heap). */
typedef void (*GrExitFn)(void);
void gr_suspend_set_exit_hook(GrExitFn fn);

