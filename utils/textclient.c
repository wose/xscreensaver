/* xscreensaver, Copyright (c) 2012 Jamie Zawinski <jwz@jwz.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 *
 * Running programs under a pipe or pty and returning bytes from them.
 * Uses these X resources:
 * 
 * program:		What to run.  Usually "xscreensaver-text".
 * relaunchDelay: secs	How long after the command dies before restarting.
 * usePty: bool		Whether to run the command interactively.
 * metaSendsESC: bool	Whether to send Alt-x as ESC x in pty-mode.
 * swapBSDEL: bool	Swap Backspace and Delete in pty-mode.
 */

#include "utils.h"

#ifndef USE_IPHONE /* whole file -- see OSX/iostextclient.m */

#include "textclient.h"
#include "resources.h"

#ifndef HAVE_COCOA
# define XK_MISCELLANY
# include <X11/keysymdef.h>
# include <X11/Xatom.h>
# include <X11/Intrinsic.h>
#endif

#include <stdio.h>

#include <signal.h>
#include <sys/wait.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
# include <fcntl.h>  /* for O_RDWR */
#endif

#ifdef HAVE_FORKPTY
# include <sys/ioctl.h>
# ifdef HAVE_PTY_H
#  include <pty.h>
# endif
# ifdef HAVE_UTIL_H
#  include <util.h>
# endif
#endif /* HAVE_FORKPTY */

/*#define DEBUG*/

extern const char *progname;

struct text_data {
  Display *dpy;
  char *program;
  int pix_w, pix_h, char_w, char_h;

  Bool pty_p;
  XtIntervalId pipe_timer;
  FILE *pipe;
  pid_t pid;
  XtInputId pipe_id;
  Bool input_available_p;
  Time subproc_relaunch_delay;
  XComposeStatus compose;

  Bool meta_sends_esc_p;
  Bool swap_bs_del_p;
  Bool meta_done_once;
  unsigned int meta_mask;

  const char *out_buffer;
  int out_column;
};


static void
subproc_cb (XtPointer closure, int *source, XtInputId *id)
{
  text_data *d = (text_data *) closure;
# ifdef DEBUG
  if (! d->input_available_p)
    fprintf (stderr, "%s: textclient: input available\n", progname);
# endif
  d->input_available_p = True;
}


static void
launch_text_generator (text_data *d)
{
  XtAppContext app = XtDisplayToApplicationContext (d->dpy);
  char buf[255];
  const char *oprogram = d->program;
  char *program = (char *) malloc (strlen (oprogram) + 50);

  strcpy (program, "( ");
  strcat (program, oprogram);

  /* Kludge!  Special-case "xscreensaver-text" to tell it how wide
     the screen is.  We used to do this by just always feeding
     `program' through sprintf() and setting the default value to
     "xscreensaver-text --cols %d", but that makes things blow up
     if someone ever uses a --program that includes a % anywhere.
   */
  if (!strcmp (oprogram, "xscreensaver-text"))
    sprintf (program + strlen(program), " --cols %d", d->char_w);

  strcat (program, " ) 2>&1");

# ifdef DEBUG
  fprintf (stderr, "%s: textclient: launch %s: %s\n", progname, 
           (d->pty_p ? "pty" : "pipe"), program);
# endif

#ifdef HAVE_FORKPTY
  if (d->pty_p)
    {
      int fd;
      struct winsize ws;
      
      ws.ws_col = d->char_w;
      ws.ws_row = d->char_h;
      ws.ws_xpixel = d->pix_w;
      ws.ws_ypixel = d->pix_h;
      
      d->pipe = NULL;
      if ((d->pid = forkpty(&fd, NULL, NULL, &ws)) < 0)
	{
          /* Unable to fork */
          sprintf (buf, "%.100s: forkpty", progname);
	  perror (buf);
	}
      else if (!d->pid)
	{
          /* This is the child fork. */
          char *av[10];
          int i = 0;
	  if (putenv ("TERM=vt100"))
            abort();
          av[i++] = "/bin/sh";
          av[i++] = "-c";
          av[i++] = program;
          av[i] = 0;
          execvp (av[0], av);
          sprintf (buf, "%.100s: %.100s", progname, oprogram);
	  perror (buf);
	  exit (1);
	}
      else
	{
          /* This is the parent fork. */
	  d->pipe = fdopen (fd, "r+");
	  d->pipe_id =
	    XtAppAddInput (app, fileno (d->pipe),
			   (XtPointer) (XtInputReadMask | XtInputExceptMask),
			   subproc_cb, (XtPointer) d);
# ifdef DEBUG
          fprintf (stderr, "%s: textclient: pid = %d\n", progname, d->pid);
# endif
	}
    }
  else
#endif /* HAVE_FORKPTY */
    {
      /* don't mess up controlling terminal on "-pipe -program tcsh". */
      static int protected_stdin_p = 0;
      if (! protected_stdin_p) {
        fclose (stdin);
        open ("/dev/null", O_RDWR); /* re-allocate fd 0 */
        protected_stdin_p = 1;
      }

      if ((d->pipe = popen (program, "r")))
	{
	  d->pipe_id =
	    XtAppAddInput (app, fileno (d->pipe),
			   (XtPointer) (XtInputReadMask | XtInputExceptMask),
			   subproc_cb, (XtPointer) d);
# ifdef DEBUG
          fprintf (stderr, "%s: textclient: popen\n", progname);
# endif
	}
      else
	{
          sprintf (buf, "%.100s: %.100s", progname, program);
	  perror (buf);
	}
    }

  free (program);
}


static void
relaunch_generator_timer (XtPointer closure, XtIntervalId *id)
{
  text_data *d = (text_data *) closure;
  /* if (!d->pipe_timer) abort(); */
  d->pipe_timer = 0;
# ifdef DEBUG
  fprintf (stderr, "%s: textclient: launch timer fired\n", progname);
# endif
  launch_text_generator (d);
}


void
textclient_reshape (text_data *d,
                    int pix_w, int pix_h,
                    int char_w, int char_h)
{
# if defined(HAVE_FORKPTY) && defined(TIOCSWINSZ)

  d->pix_w  = pix_w;
  d->pix_h  = pix_h;
  d->char_w = char_w;
  d->char_h = char_h;

# ifdef DEBUG
  fprintf (stderr, "%s: textclient: reshape: %dx%d, %dx%d\n", progname,
           pix_w, pix_h, char_w, char_h);
# endif

  if (d->pid && d->pipe)
    {
      /* Tell the sub-process that the screen size has changed. */
      struct winsize ws;
      ws.ws_col = char_w;
      ws.ws_row = char_h;
      ws.ws_xpixel = pix_w;
      ws.ws_ypixel = pix_h;
      ioctl (fileno (d->pipe), TIOCSWINSZ, &ws);
      kill (d->pid, SIGWINCH);
    }
# endif /* HAVE_FORKPTY && TIOCSWINSZ */


  /* If we're running xscreensaver-text, then kill and restart it any
     time the window is resized so that it gets an updated --cols arg
     right away.  But if we're running something else, leave it alone.
   */
  if (!strcmp (d->program, "xscreensaver-text"))
    {
      if (d->pid)
        kill (d->pid, SIGTERM);
      if (d->pipe_id)
        XtRemoveInput (d->pipe_id);
      if (d->pipe)
        pclose (d->pipe);
      d->input_available_p = False;
      relaunch_generator_timer (d, 0);
    }
}


text_data *
textclient_open (Display *dpy)
{
  text_data *d = (text_data *) calloc (1, sizeof (*d));

# ifdef DEBUG
  fprintf (stderr, "%s: textclient: init\n", progname);
# endif

  d->dpy = dpy;

  if (get_boolean_resource (dpy, "usePty", "UsePty"))
    {
# ifdef HAVE_FORKPTY
      d->pty_p = True;
# else
      fprintf (stderr,
               "%s: no pty support on this system; using a pipe instead.\n",
               progname);
# endif
    }

  d->subproc_relaunch_delay =
    (1000 * get_integer_resource (dpy, "relaunchDelay", "Time"));

  d->meta_sends_esc_p = get_boolean_resource (dpy, "metaSendsESC", "Boolean");
  d->swap_bs_del_p    = get_boolean_resource (dpy, "swapBSDEL",    "Boolean");

  d->program = get_string_resource (dpy, "program", "Program");


# ifdef HAVE_FORKPTY
  /* Kludge for MacOS standalone mode: see OSX/SaverRunner.m. */
  {
    const char *s = getenv ("XSCREENSAVER_STANDALONE");
    if (s && *s && strcmp(s, "0"))
      {
        d->pty_p = 1;
        d->program = strdup (getenv ("SHELL"));
      }
  }
# endif

  launch_text_generator (d);

  return d;
}


void
textclient_close (text_data *d)
{
# ifdef DEBUG
  fprintf (stderr, "%s: textclient: free\n", progname);
# endif

  if (d->program)
    free (d->program);
  if (d->pipe_id)
    XtRemoveInput (d->pipe_id);
  if (d->pipe)
    pclose (d->pipe);
  if (d->pipe_timer)
    XtRemoveTimeOut (d->pipe_timer);
  memset (d, 0, sizeof (*d));
  free (d);
}

int
textclient_getc (text_data *d)
{
  XtAppContext app = XtDisplayToApplicationContext (d->dpy);
  int ret = -1;

  if (XtAppPending (app) & (XtIMTimer|XtIMAlternateInput))
    XtAppProcessEvent (app, XtIMTimer|XtIMAlternateInput);

  if (d->out_buffer && *d->out_buffer)
    {
      ret = *d->out_buffer;
      d->out_buffer++;
    }
  else if (d->input_available_p && d->pipe)
    {
      unsigned char s[2];
      int n = read (fileno (d->pipe), (void *) s, 1);
      if (n > 0)
        ret = s[0];
      else		/* EOF */
        {
          if (d->pipe_id)
            XtRemoveInput (d->pipe_id);
          d->pipe_id = 0;

	  if (d->pid)
	    {
# ifdef DEBUG
              fprintf (stderr, "%s: textclient: waitpid %d\n",
                       progname, d->pid);
# endif
	      waitpid (d->pid, NULL, 0);
	      fclose (d->pipe);
              d->pid = 0;
	    }
	  else
	    {
# ifdef DEBUG
              fprintf (stderr, "%s: textclient: pclose\n", progname);
# endif
	      pclose (d->pipe);
	    }
          d->pipe = 0;

          if (d->out_column > 0)
            {
# ifdef DEBUG
              fprintf (stderr, "%s: textclient: adding blank line at EOF\n",
                       progname);
# endif
              d->out_buffer = "\r\n\r\n";
            }

# ifdef DEBUG
          fprintf (stderr, "%s: textclient: relaunching in %d\n", progname, 
                   (int) d->subproc_relaunch_delay);
# endif
          d->pipe_timer =
            XtAppAddTimeOut (app, d->subproc_relaunch_delay,
                             relaunch_generator_timer,
                             (XtPointer) d);
        }
      d->input_available_p = False;
    }

  if (ret == '\r' || ret == '\n')
    d->out_column = 0;
  else if (ret > 0)
    d->out_column++;

# ifdef DEBUG
  if (ret <= 0)
    fprintf (stderr, "%s: textclient: getc: %d\n", progname, ret);
  else if (ret < ' ')
    fprintf (stderr, "%s: textclient: getc: %03o\n", progname, ret);
  else
    fprintf (stderr, "%s: textclient: getc: '%c'\n", progname, (char) ret);
# endif

  return ret;
}


/* The interpretation of the ModN modifiers is dependent on what keys
   are bound to them: Mod1 does not necessarily mean "meta".  It only
   means "meta" if Meta_L or Meta_R are bound to it.  If Meta_L is on
   Mod5, then Mod5 is the one that means Meta.  Oh, and Meta and Alt
   aren't necessarily the same thing.  Icepicks in my forehead!
 */
static unsigned int
do_icccm_meta_key_stupidity (Display *dpy)
{
  unsigned int modbits = 0;
# ifndef HAVE_COCOA
  int i, j, k;
  XModifierKeymap *modmap = XGetModifierMapping (dpy);
  for (i = 3; i < 8; i++)
    for (j = 0; j < modmap->max_keypermod; j++)
      {
        int code = modmap->modifiermap[i * modmap->max_keypermod + j];
        KeySym *syms;
        int nsyms = 0;
        if (code == 0) continue;
        syms = XGetKeyboardMapping (dpy, code, 1, &nsyms);
        for (k = 0; k < nsyms; k++)
          if (syms[k] == XK_Meta_L || syms[k] == XK_Meta_R ||
              syms[k] == XK_Alt_L  || syms[k] == XK_Alt_R)
            modbits |= (1 << i);
        XFree (syms);
      }
  XFreeModifiermap (modmap);
# endif /* HAVE_COCOA */
  return modbits;
}


/* Returns a mask of the bit or bits of a KeyPress event that mean "meta". 
 */
static unsigned int
meta_modifier (text_data *d)
{
  if (!d->meta_done_once)
    {
      /* Really, we are supposed to recompute this if a KeymapNotify
         event comes in, but fuck it. */
      d->meta_done_once = True;
      d->meta_mask = do_icccm_meta_key_stupidity (d->dpy);
# ifdef DEBUG
      fprintf (stderr, "%s: textclient: ICCCM Meta is 0x%08X\n",
               progname, d->meta_mask);
# endif
    }
  return d->meta_mask;
}


Bool
textclient_putc (text_data *d, XKeyEvent *k)
{
  KeySym keysym;
  unsigned char c = 0;
  XLookupString (k, (char *) &c, 1, &keysym, &d->compose);
  if (c != 0 && d->pipe)
    {
      if (!d->swap_bs_del_p) ;
      else if (c == 127) c = 8;
      else if (c == 8)   c = 127;

      /* If meta was held down, send ESC, or turn on the high bit. */
      if (k->state & meta_modifier (d))
        {
          if (d->meta_sends_esc_p)
            fputc ('\033', d->pipe);
          else
            c |= 0x80;
        }

      fputc (c, d->pipe);
      fflush (d->pipe);
      k->type = 0;  /* don't interpret this event defaultly. */

# ifdef DEBUG
      fprintf (stderr, "%s: textclient: putc '%c'\n", progname, (char) c);
# endif

      return True;
    }
  return False;
}

#endif /* !USE_IPHONE -- whole file */
