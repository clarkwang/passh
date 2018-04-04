/* vi:set ts=8 sw=4 sta et:
 *
 * Author: Clark Wang <dearvoid at gmail.com>
 *
 * NOTE:
 *  - Interactive only when stdin is a tty.
 *  - In interactive mode, will not send passwords any more after user starts
 *    inputting from the keyboard.
 */

/*
 * - On OS X EI Capitan (10.11.6), definging _XOPEN_SOURCE=600 would cause
 *   SIGWINCH to be undefined.
 */
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#define _XOPEN_SOURCE 600 /* for posix_openpt() */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <regex.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#define BUFFSIZE         (8 * 1024)
#define DEFAULT_COUNT    0
#define DEFAULT_TIMEOUT  0
#define DEFAULT_PASSWD   "password"
#define DEFAULT_PROMPT   "[Pp]assword: \\{0,1\\}$"
#define DEFAULT_YESNO    "(yes/no)? \\{0,1\\}$"

#define ERROR_GENERAL    (200 + 1)
#define ERROR_USAGE      (200 + 2)
#define ERROR_TIMEOUT    (200 + 3)
#define ERROR_SYS        (200 + 4)
#define ERROR_MAX_TRIES  (200 + 5)

static struct {
    char *progname;
    bool reset_on_exit;
    struct termios save_termios;
    bool SIGCHLDed;
    bool received_winch;
    bool stdin_is_tty;
    bool now_interactive;

    int fd_ptym;

    struct {
        bool ignore_case;
        bool nohup_child;
        bool fatal_no_prompt;
        bool auto_yesno;
        char *password;
        char *passwd_prompt;
        char *yesno_prompt;
        regex_t re_prompt;
        regex_t re_yesno;
        int timeout;
        int tries;
        bool fatal_more_tries;
        char **command;

        char *log_to_pty;
        char *log_from_pty;
    } opt;
} g;

void
usage(int exitcode)
{
    printf("Usage: %s [OPTION]... COMMAND...\n"
           "\n"
           "  -c <N>          Send at most <N> passwords (0 means infinite. Default: %d)\n"
           "  -C              Exit if prompted for the <N+1>th password\n"
           "  -h              Help\n"
           "  -i              Case insensitive for password prompt matching\n"
           "  -n              Nohup the child (e.g. used for `ssh -f')\n"
           "  -p <password>   The password (Default: `" DEFAULT_PASSWD "')\n"
           "  -p env:<var>    Read password from env var\n"
           "  -p file:<file>  Read password from file\n"
           "  -P <prompt>     Regexp (BRE) for the password prompt\n"
           "                  (Default: `" DEFAULT_PROMPT "')\n"
           "  -l <file>       Save data written to the pty\n"
           "  -L <file>       Save data read from the pty\n"
           "  -t <timeout>    Timeout waiting for next password prompt\n"
           "                  (0 means no timeout. Default: %d)\n"
           "  -T              Exit if timed out waiting for password prompt\n"
           "  -y              Auto answer `(yes/no)?' questions\n"
#if 0
           "  -Y <pattern>    Regexp (BRE) for the `yes/no' prompt\n"
           "                  (Default: `" DEFAULT_YESNO "')\n"
#endif
           "\n"
           "Report bugs to Clark Wang <dearvoid@gmail.com>\n"
           "", g.progname, DEFAULT_COUNT, DEFAULT_TIMEOUT);

    exit(exitcode);
}

void
fatal(int rcode, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* in case stdout and stderr are the same */
    fflush(stdout);

    fprintf(stderr, "!! %s\r\n", buf);

    /* flush all open files */
    fflush(NULL);

    exit(rcode);
}

void
fatal_sys(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    int error = errno;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fatal(ERROR_SYS, "%s: %s (%d)", buf, strerror(error), error);
}

void
startup()
{
    g.opt.passwd_prompt = DEFAULT_PROMPT;
    g.opt.yesno_prompt = DEFAULT_YESNO;
    g.opt.password = DEFAULT_PASSWD;
    g.opt.tries = DEFAULT_COUNT;
    g.opt.timeout = DEFAULT_TIMEOUT;
}

char *
arg2pass(char *optarg)
{
    char *pass = NULL;

    if (strncmp(optarg, "file:", 5) == 0) {
        FILE *fp = fopen(optarg + 5, "r");
        char buf[64] = "";

        fgets(buf, sizeof(buf), fp);
        fclose(fp);

        pass = strtok(buf, " \r\n");
        if (pass) {
            pass = strdup(pass);
        } else {
            pass = strdup("");
        }
    } else if (strncmp(optarg, "env:", 4) == 0) {
        pass = getenv(optarg + 4);
        if (pass) {
            pass = strdup(pass);
        }
    } else {
        pass = strdup(optarg);
    }

    return pass;
}

void
getargs(int argc, char **argv)
{
    int ch, i, r, reflag;

    if ((g.progname = strrchr(argv[0], '/')) != NULL) {
        ++g.progname;
    } else {
        g.progname = argv[0];
    }

    if (argc == 1 || (argc == 2 && strcmp("--help", argv[1]) == 0) ) {
        usage(0);
    }

    /*
     * If the first character of optstring is '+' or the environment variable
     * POSIXLY_CORRECT is set, then option processing stops as soon as a
     * nonoption argument is encountered.
     */
    while ((ch = getopt(argc, argv, "+:c:Chil:L:np:P:t:Ty")) != -1) {
        switch (ch) {
            case 'c':
                g.opt.tries = atoi(optarg);
                break;
            case 'C':
                g.opt.fatal_more_tries = true;
                break;
            case 'h':
                usage(0);

            case 'i':
                g.opt.ignore_case = true;
                break;

            case 'l':
                g.opt.log_to_pty = optarg;
                break;

            case 'L':
                g.opt.log_from_pty = optarg;
                break;

            case 'n':
                g.opt.nohup_child = true;
                break;

            case 'p':
                g.opt.password = arg2pass(optarg);
                for (i = 0; i < strlen(optarg); ++i) {
                    optarg[i] = '*';
                }
                if (g.opt.password == NULL) {
                    fatal(ERROR_USAGE, "Error: failed to get password");
                }
                break;

            case 'P':
                g.opt.passwd_prompt = optarg;
                break;

            case 't':
                g.opt.timeout = atoi(optarg);
                break;

            case 'T':
                g.opt.fatal_no_prompt = true;
                break;

            case 'y':
                g.opt.auto_yesno = true;
                break;
#if 0
            case 'Y':
                g.opt.yesno_prompt = optarg;
                break;
#endif
            case '?':
            default:
                fatal(ERROR_USAGE, "Error: unknown option '-%c'", optopt);
        }
    }
    argc -= optind;
    argv += optind;

    if (0 == argc) {
        fatal(ERROR_USAGE, "Error: no command specified");
    }
    g.opt.command = argv;

    if (0 == strlen(g.opt.passwd_prompt) ) {
        fatal(ERROR_USAGE, "Error: empty prompt");
    }

    /* Password: */
    reflag = 0;
    reflag |= g.opt.ignore_case ? REG_ICASE : 0;
    r = regcomp(&g.opt.re_prompt, g.opt.passwd_prompt, reflag);
    if (r != 0) {
        fatal(ERROR_USAGE, "Error: invalid RE for password prompt");
    }
    /* (yes/no)? */
    r = regcomp(&g.opt.re_yesno, g.opt.yesno_prompt, reflag);
    if (r != 0) {
        fatal(ERROR_USAGE, "Error: invalid RE for yes/no prompt");
    }
}

int
ptym_open(char *pts_name, int pts_namesz)
{
    char *ptr;
    int fdm;

    snprintf(pts_name, pts_namesz, "/dev/ptmx");

    fdm = posix_openpt(O_RDWR);
    if (fdm < 0)
        return (-1);

    if (grantpt(fdm) < 0) {
        close(fdm);
        return (-2);
    }

    if (unlockpt(fdm) < 0) {
        close(fdm);
        return (-3);
    }

    if ((ptr = ptsname(fdm)) == NULL) {
        close(fdm);
        return (-4);
    }

    snprintf(pts_name, pts_namesz, "%s", ptr);
    return (fdm);
}

int
ptys_open(char *pts_name)
{
    int fds;

    if ((fds = open(pts_name, O_RDWR)) < 0)
        return (-5);
    return (fds);
}

pid_t
pty_fork(int *ptrfdm, char *slave_name, int slave_namesz,
    const struct termios *slave_termios,
    const struct winsize *slave_winsize)
{
    int fdm, fds;
    pid_t pid;
    char pts_name[32];

    if ((fdm = ptym_open(pts_name, sizeof(pts_name))) < 0)
        fatal_sys("can't open master pty: %s, error %d", pts_name, fdm);

    if (slave_name != NULL) {
        /*
         * Return name of slave.  Null terminate to handle case
         * where strlen(pts_name) > slave_namesz.
         */
        snprintf(slave_name, slave_namesz, "%s", pts_name);
    }

    if ((pid = fork()) < 0) {
        return (-1);
    } else if (pid == 0) {
        /*
         * child
         */
        if (setsid() < 0)
            fatal_sys("setsid error");

        /*
         * System V acquires controlling terminal on open().
         */
        if ((fds = ptys_open(pts_name)) < 0)
            fatal_sys("can't open slave pty");

        /* all done with master in child */
        close(fdm);

#if defined(TIOCSCTTY)
        /*
         * TIOCSCTTY is the BSD way to acquire a controlling terminal.
         *
         * Don't check the return code. It would fail in Cygwin.
         */
        ioctl(fds, TIOCSCTTY, (char *)0);
#endif
        /*
         * Set slave's termios and window size.
         */
        if (slave_termios != NULL) {
            if (tcsetattr(fds, TCSANOW, slave_termios) < 0)
                fatal_sys("tcsetattr error on slave pty");
        }
        if (slave_winsize != NULL) {
            if (ioctl(fds, TIOCSWINSZ, slave_winsize) < 0)
                fatal_sys("TIOCSWINSZ error on slave pty");
        }

        /*
         * Slave becomes stdin/stdout/stderr of child.
         */
        if (dup2(fds, STDIN_FILENO) != STDIN_FILENO)
            fatal_sys("dup2 error to stdin");
        if (dup2(fds, STDOUT_FILENO) != STDOUT_FILENO)
            fatal_sys("dup2 error to stdout");
        if (dup2(fds, STDERR_FILENO) != STDERR_FILENO)
            fatal_sys("dup2 error to stderr");
        if (fds != STDIN_FILENO && fds != STDOUT_FILENO &&
            fds != STDERR_FILENO) {
            close(fds);
        }

        return (0);
    } else {
        /*
         * parent
         */
        *ptrfdm = fdm;
        return (pid);
    }
}

int
tty_raw(int fd, struct termios *save_termios)
{
    int err;
    struct termios buf;

    if (tcgetattr(fd, &buf) < 0)
        return (-1);
    *save_termios = buf;

    /*
     * Echo off, canonical mode off, extended input
     * processing off, signal chars off.
     */
    buf.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    /*
     * No SIGINT on BREAK, CR-to-NL off, input parity
     * check off, don't strip 8th bit on input, output
     * flow control off.
     */
    buf.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    /*
     * Clear size bits, parity checking off.
     */
    buf.c_cflag &= ~(CSIZE | PARENB);

    /*
     * Set 8 bits/char.
     */
    buf.c_cflag |= CS8;

    /*
     * Output processing off.
     */
    buf.c_oflag &= ~(OPOST);

    /*
     * Case B: 1 byte at a time, no timer.
     */
    buf.c_cc[VMIN] = 1;
    buf.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSAFLUSH, &buf) < 0)
        return (-1);

    /*
     * Verify that the changes stuck.  tcsetattr can return 0 on
     * partial success.
     */
    if (tcgetattr(fd, &buf) < 0) {
        err = errno;
        tcsetattr(fd, TCSAFLUSH, save_termios);
        errno = err;
        return (-1);
    }
    if ((buf.c_lflag & (ECHO | ICANON | IEXTEN | ISIG)) ||
        (buf.c_iflag & (BRKINT | ICRNL | INPCK | ISTRIP | IXON)) ||
        (buf.c_cflag & (CSIZE | PARENB | CS8)) != CS8 ||
        (buf.c_oflag & OPOST) || buf.c_cc[VMIN] != 1 ||
        buf.c_cc[VTIME] != 0) {
        /*
         * Only some of the changes were made.  Restore the
         * original settings.
         */
        tcsetattr(fd, TCSAFLUSH, save_termios);
        errno = EINVAL;
        return (-1);
    }

    return (0);
}

int
tty_reset(int fd, struct termios *termio)
{
    if (tcsetattr(fd, TCSAFLUSH, termio) < 0)
        return (-1);
    return (0);
}

void
tty_atexit(void)
{
    if (g.reset_on_exit) {
        tty_reset(STDIN_FILENO, &g.save_termios);
    }
}

ssize_t
read_if_ready(int fd, char *buf, size_t n)
{
    struct timeval timeout;
    fd_set fds;
    int nread;

    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    if (select(fd + 1, &fds, NULL, NULL, &timeout) < 0) {
        return -1;
    }
    if (! FD_ISSET(fd, &fds) ) {
        return 0;
    }
    if ((nread = read(fd, buf, n) ) < 0) {
        return -1;
    }
    return nread;
}

ssize_t
writen(int fd, const void *ptr, size_t n)
{
    size_t nleft;
    ssize_t nwritten;

    nleft = n;
    while (nleft > 0) {
        if ((nwritten = write(fd, ptr, nleft)) < 0) {
            if (nleft == n) {
                return (-1);
            } else {
                /* error, return amount written so far */
                break;
            }
        } else if (nwritten == 0) {
            break;
        }
        nleft -= nwritten;
        ptr += nwritten;
    }
    return (n - nleft);
}

/*
 * The only portable use of signal() is to set a signal's disposition
 * to SIG_DFL or SIG_IGN.  The semantics when using signal() to
 * establish a signal handler vary across systems (and POSIX.1
 * explicitly  permits  this variation); do not use it for this purpose.
 *
 * POSIX.1 solved the portability mess by specifying sigaction(2),
 * which provides explicit control of the semantics when a signal
 * handler is invoked; use that interface instead of signal().
 *
 * In the original UNIX systems, when a handler that was established
 * using signal() was invoked by the delivery of a signal, the
 * disposition of the signal would be reset to SIG_DFL, and the system
 * did not block delivery of further instances of the signal.  This is
 * equivalent to calling sigaction(2) with the following flags:
 *
 *   sa.sa_flags = SA_RESETHAND | SA_NODEFER;
 */
void
sig_handle(int signo, void (*handler)(int) )
{
    struct sigaction act;

    memset(&act, 0, sizeof(act) );
    act.sa_handler = handler;
    sigaction(signo, &act, NULL);
}

void
sig_child(int signo)
{
    g.SIGCHLDed = true;
}

void
sig_winch(int signum)
{
    g.received_winch = true;
    return;
}

#define write2(fd1, fd2, buf, len) \
    do { \
        int fds[2] = { fd1, fd2 }; \
        int i; \
        for (i = 0; i < 2; ++i) { \
            if (fds[i] < 0) { \
                continue; \
            } \
            if (writen(fds[i], buf, (len) ) != (len) ) { \
                fatal_sys("write: fd %d", fds[i]); \
            } \
        } \
    } while (0)
void
big_loop()
{
    char buf1[BUFFSIZE];          /* for read() from stdin */
    char buf2[2 * BUFFSIZE + 1];  /* for read() from ptym, `+1' for adding the '\000' */
    char *cache = buf2;
    int nread, ncache = 0;
    struct timeval select_timeout;
    fd_set readfds;
    int i, r, status;
    regmatch_t re_match[1];
    time_t last_time = time(NULL);
    bool given_up = false;
    int passwords_seen = 0;
    int fd_to_pty = -1, fd_from_pty = -1;
    bool stdin_eof = false;
    int exit_code = -1;
    pid_t wait_return;

    if (g.opt.log_to_pty != NULL) {
        fd_to_pty = open(g.opt.log_to_pty, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd_to_pty < 0) {
            fatal_sys("open: %s", g.opt.log_to_pty);
        }
    }
    if (g.opt.log_from_pty != NULL) {
        fd_from_pty = open(g.opt.log_from_pty, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd_from_pty < 0) {
            fatal_sys("open: %s", g.opt.log_from_pty);
        }
    }

    /*
     * wait for the child to open the pty
     */
    do {
        /*
         * On Mac, fcntl(O_NONBLOCK) may fail before the child opens the pty
         * slave side. So wait a while for the child to open the pty slave.
         */
        fd_set writefds;

        select_timeout.tv_sec = 1;
        select_timeout.tv_usec = 0;

        FD_ZERO(&writefds);
        FD_SET(g.fd_ptym, &writefds);

        select(g.fd_ptym + 1, NULL, &writefds, NULL, &select_timeout);
        if (! FD_ISSET(g.fd_ptym, &writefds) ) {
            fatal(ERROR_GENERAL, "failed to wait for ptym to be writable");
        }
    } while (0);

    while (true) {
L_chk_sigchld:
        if (g.SIGCHLDed) {
            /*
             * NOTE:
             *  - WCONTINUED does not work on macOS (10.12.5)
             *  - On macOS, SIGCHLD can be generated when
             *     1. child process has terminated/exited
             *     2. the currently *running* child process is stopped (e.g. by `kill -STOP')
             *  - On Linux, SIGCHLD can be generated when
             *     1. child process has terminated/exited
             *     2. the currently *running* child process is stopped (e.g. by `kill -STOP')
             *     3. the currently *stopped* child process is continued (e.g. by `kill -CONT')
             *  - waitpid(WCONTINUED) works on Linux but not on macOS.
             */
            wait_return = waitpid(-1, &status, WUNTRACED | WCONTINUED);
            if (wait_return < 0) {
                fatal_sys("received SIGCHLD but waitpid() failed");
            }
            g.SIGCHLDed = false;

            if (WIFEXITED(status) ) {
                exit_code = WEXITSTATUS(status);
                goto L_done;
            } else if (WIFSIGNALED(status) ) {
                exit_code = status + 128;
                goto L_done;
            } else if (WIFSTOPPED(status) ) {
                /* Do nothing. Just wait for the child to be continued and wait
                 * for the next SIGCHLD. */
            } else if (WIFCONTINUED(status) ) {
                /* */
            } else {
                /* This should not happen. */
                goto L_done;
            }
        }

        if (g.opt.timeout != 0 && g.opt.fatal_no_prompt && passwords_seen == 0
            && labs(time(NULL) - last_time) > g.opt.timeout) {
            fatal(ERROR_TIMEOUT, "timeout waiting for password prompt");
        }

        if (g.received_winch && g.stdin_is_tty) {
            struct winsize ttysize;
            static int ourtty = -1;

            g.received_winch = false;

            if (ourtty < 0) {
#if 0
                ourtty = open("/dev/tty", 0);
#else
                ourtty = STDIN_FILENO;
#endif
            }
            if (ioctl(ourtty, TIOCGWINSZ, &ttysize) == 0) {
                ioctl(g.fd_ptym, TIOCSWINSZ, &ttysize);
            }
        }

        /* Keep sending EOF until the child exits
         *  - See http://lists.gnu.org/archive/html/help-bash/2016-11/msg00002.html
         *    (EOF ('\004') was lost if it's sent to bash too quickly)
         *  - We cannot simply close(fd_ptym) or the child will get SIGHUP. */
        while (stdin_eof) {
            struct termios term;
            char eof_char;
            static struct timeval last;
            struct timeval now;
            double diff;

            if (last.tv_sec == 0) {
                gettimeofday(&last, NULL);
                break;
            }

            gettimeofday(&now, NULL);
            diff = now.tv_sec + now.tv_usec / 1e6 - (last.tv_sec + last.tv_usec / 1e6);
            if (diff > -0.05 && diff < 0.05) {
                break;
            }
            last = now;

            if (tcgetattr(g.fd_ptym, &term) < 0) {
                goto L_done;
            }
            eof_char = term.c_cc[VEOF];
            if (write(g.fd_ptym, &eof_char, 1) < 0) {
                goto L_done;
            }
            write(fd_to_pty, &eof_char, 1);

            break;
        }

        FD_ZERO(&readfds);
        if (g.stdin_is_tty && !stdin_eof) {
            FD_SET(STDIN_FILENO, &readfds);
        }
        FD_SET(g.fd_ptym, &readfds);

        select_timeout.tv_sec = 1;
        select_timeout.tv_usec = 100 * 1000;

        r = select(g.fd_ptym + 1, &readfds, NULL, NULL, &select_timeout);
        if (r == 0) {
            /* timeout */
            continue;
        } else if (r < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                fatal_sys("select error");
            }
        }

        /*
         * copy data from ptym to stdout
         */
        if (FD_ISSET(g.fd_ptym, &readfds) ) {
            while (true) {
                nread = read_if_ready(g.fd_ptym, cache + ncache,
                    2 * BUFFSIZE - (cache - buf2));
                if (nread <= 0) {
                    /* child exited? */
                    goto L_chk_sigchld;
                }

                write2(STDOUT_FILENO, fd_from_pty, cache + ncache, nread);

                if (! given_up && g.opt.timeout != 0
                    && labs(time(NULL) - last_time) >= g.opt.timeout) {
                    given_up = true;
                }

                /* regexec() does not like NULLs */
                if (! given_up) {
                    for (i = 0; i < nread; ++i) {
                        if (cache[ncache + i] == 0) {
                            cache[ncache + i] = 0xff;
                        }
                    }
                }
                ncache += nread;
                /* make it NULL-terminated so regexec() would be happy */
                cache[ncache] = 0;

                /* match password prompt and send the password */
                if (! g.now_interactive && ! given_up) {
                    if (g.opt.auto_yesno && passwords_seen == 0
                        && regexec(&g.opt.re_yesno, cache, 1, re_match, 0) == 0)
                    {
                        /*
                         * (yes/no)?
                         */
                        char *yes = "yes\r";

                        write2(g.fd_ptym, fd_to_pty, yes, strlen(yes) );

                        ncache -= re_match[0].rm_eo;
                        cache += re_match[0].rm_eo;
                    } else if (regexec(&g.opt.re_prompt, cache, 1, re_match, 0) == 0) {
                        /*
                         * Password:
                         */

                        ++passwords_seen;

                        last_time = time(NULL);

                        if (g.opt.fatal_more_tries) {
                            if (g.opt.tries != 0 && passwords_seen > g.opt.tries) {
                                fatal(ERROR_MAX_TRIES, "still prompted for passwords after %d tries", g.opt.tries);
                            }
                        } else if (g.opt.tries != 0 && passwords_seen >= g.opt.tries) {
                            given_up = true;
                        }

                        write(g.fd_ptym, g.opt.password, strlen(g.opt.password));
                        write(g.fd_ptym, "\r", 1);

                        write(fd_to_pty, "********\r", strlen("********\r") );

                        ncache -= re_match[0].rm_eo;
                        cache += re_match[0].rm_eo;
                    }
                } else {
                    cache = buf2;
                    ncache = 0;
                }

                if (cache + ncache >= buf2 + 2 * BUFFSIZE) {
                    if (ncache > BUFFSIZE) {
                        cache += ncache - BUFFSIZE;
                        ncache = BUFFSIZE;
                    }
                    memmove(buf2, cache, ncache);
                    cache = buf2;
                }
            }
        }
        /*
         * copy data from stdin to ptym
         */
        if (!stdin_eof && FD_ISSET(STDIN_FILENO, &readfds) ) {
            if ((nread = read(STDIN_FILENO, buf1, BUFFSIZE)) < 0)
                fatal_sys("read error from stdin");
            else if (nread == 0) {
                /* EOF on stdin means we're done */
                stdin_eof = true;
            } else {
                g.now_interactive = true;
                write2(g.fd_ptym, fd_to_pty, buf1, nread);
            }
        }
    }

L_done:
    /* the child has exited but there may be still some data for us
     * to read */
    while ((nread = read_if_ready(g.fd_ptym, buf2, BUFFSIZE) ) > 0) {
        write2(STDOUT_FILENO, fd_from_pty, buf2, nread);
    }

    if (fd_to_pty >= 0) {
        close(fd_to_pty);
    }
    if (fd_from_pty >= 0) {
        close(fd_from_pty);
    }

    if (exit_code < 0) {
        exit(ERROR_GENERAL);
    } else {
        exit(exit_code);
    }
}

int
main(int argc, char *argv[])
{
    char slave_name[32];
    pid_t pid;
    struct termios orig_termios;
    struct winsize size;

    startup();

    getargs(argc, argv);

    g.stdin_is_tty = isatty(STDIN_FILENO);

    sig_handle(SIGCHLD, sig_child);

    if (g.stdin_is_tty) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
            fatal_sys("tcgetattr error on stdin");
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, (char *) &size) < 0)
            fatal_sys("TIOCGWINSZ error");

        pid = pty_fork(&g.fd_ptym, slave_name, sizeof(slave_name),
            &orig_termios, &size);

    } else {
        pid = pty_fork(&g.fd_ptym, slave_name, sizeof(slave_name),
            NULL, NULL);
    }

    if (pid < 0) {
        fatal_sys("fork error");
    } else if (pid == 0) {
        /*
         * child
         */
        if (g.opt.nohup_child) {
            sig_handle(SIGHUP, SIG_IGN);
        }
        if (execvp(g.opt.command[0], g.opt.command) < 0)
            fatal_sys("can't execute: %s", argv[optind]);
    }

    /*
     * parent
     */

    /* stdout also needs to be checked. Or `passh ls -l | less' would not
     * restore the saved tty settings. */
    if (g.stdin_is_tty && isatty(STDOUT_FILENO) ) {
        /* user's tty to raw mode */
        if (tty_raw(STDIN_FILENO, &g.save_termios) < 0)
            fatal_sys("tty_raw error");

        /* reset user's tty on exit */
        g.reset_on_exit = true;
        if (atexit(tty_atexit) < 0)
            fatal_sys("atexit error");

        sig_handle(SIGWINCH, sig_winch);
    }

    big_loop();

    return 0;
}
