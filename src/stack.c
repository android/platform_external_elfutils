/* Unwinding of frames like gstack/pstack.
   Copyright (C) 2013-2014 Red Hat, Inc.
   This file is part of elfutils.

   This file is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>
#include <assert.h>
#include <argp.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <string.h>
#include <locale.h>
#include <fcntl.h>
#include ELFUTILS_HEADER(dwfl)

#include <dwarf.h>
#include <system.h>
#include <printversion.h>

/* Name and version of program.  */
ARGP_PROGRAM_VERSION_HOOK_DEF = print_version;

/* Bug report address.  */
ARGP_PROGRAM_BUG_ADDRESS_DEF = PACKAGE_BUGREPORT;

/* non-printable argp options.  */
#define OPT_DEBUGINFO	0x100
#define OPT_COREFILE	0x101

static bool show_activation = false;
static bool show_module = false;
static bool show_build_id = false;
static bool show_source = false;
static bool show_one_tid = false;
static bool show_quiet = false;
static bool show_raw = false;
static bool show_modules = false;
static bool show_debugname = false;
static bool show_inlines = false;

static int maxframes = 256;

struct frame
{
  Dwarf_Addr pc;
  bool isactivation;
};

struct frames
{
  int frames;
  int allocated;
  struct frame *frame;
};

static Dwfl *dwfl = NULL;
static pid_t pid = 0;
static int core_fd = -1;
static Elf *core = NULL;
static const char *exec = NULL;
static char *debuginfo_path = NULL;

static const Dwfl_Callbacks proc_callbacks =
  {
    .find_elf = dwfl_linux_proc_find_elf,
    .find_debuginfo = dwfl_standard_find_debuginfo,
    .debuginfo_path = &debuginfo_path,
  };

static const Dwfl_Callbacks core_callbacks =
  {
    .find_elf = dwfl_build_id_find_elf,
    .find_debuginfo = dwfl_standard_find_debuginfo,
    .debuginfo_path = &debuginfo_path,
  };

#ifdef USE_DEMANGLE
static size_t demangle_buffer_len = 0;
static char *demangle_buffer = NULL;
#endif

/* Whether any frames have been shown at all.  Determines exit status.  */
static bool frames_shown = false;

/* Program exit codes. All frames shown without any errors is GOOD.
   Some frames shown with some non-fatal errors is an ERROR.  A fatal
   error or no frames shown at all is BAD.  A command line USAGE exit
   is generated by argp_error.  */
#define EXIT_OK     0
#define EXIT_ERROR  1
#define EXIT_BAD    2
#define EXIT_USAGE 64

static int
get_addr_width (Dwfl_Module *mod)
{
  // Try to find the address wide if possible.
  static int width = 0;
  if (width == 0 && mod)
    {
      Dwarf_Addr bias;
      Elf *elf = dwfl_module_getelf (mod, &bias);
      if (elf)
        {
	  GElf_Ehdr ehdr_mem;
	  GElf_Ehdr *ehdr = gelf_getehdr (elf, &ehdr_mem);
	  if (ehdr)
	    width = ehdr->e_ident[EI_CLASS] == ELFCLASS32 ? 8 : 16;
	}
    }
  if (width == 0)
    width = 16;

  return width;
}

static int
module_callback (Dwfl_Module *mod, void **userdata __attribute__((unused)),
		 const char *name, Dwarf_Addr start,
		 void *arg __attribute__((unused)))
{
  /* Forces resolving of main elf and debug files. */
  Dwarf_Addr bias;
  Elf *elf = dwfl_module_getelf (mod, &bias);
  Dwarf *dwarf = dwfl_module_getdwarf (mod, &bias);

  Dwarf_Addr end;
  const char *mainfile;
  const char *debugfile;
  const char *modname = dwfl_module_info (mod, NULL, NULL, &end, NULL,
                                          NULL, &mainfile, &debugfile);
  if (modname == NULL || strcmp (modname, name) != 0)
    {
      end = start + 1;
      mainfile = NULL;
      debugfile = NULL;
    }

  int width = get_addr_width (mod);
  printf ("0x%0*" PRIx64 "-0x%0*" PRIx64 " %s\n",
	  width, start, width, end, basename (name));

  const unsigned char *id;
  GElf_Addr id_vaddr;
  int id_len = dwfl_module_build_id (mod, &id, &id_vaddr);
  if (id_len > 0)
    {
      printf ("  [");
      do
	printf ("%02" PRIx8, *id++);
      while (--id_len > 0);
      printf ("]\n");
    }

  if (elf != NULL)
    printf ("  %s\n", mainfile != NULL ? mainfile : "-");
  if (dwarf != NULL)
    printf ("  %s\n", debugfile != NULL ? debugfile : "-");

  return DWARF_CB_OK;
}

static int
frame_callback (Dwfl_Frame *state, void *arg)
{
  struct frames *frames = (struct frames *) arg;
  int nr = frames->frames;
  if (! dwfl_frame_pc (state, &frames->frame[nr].pc,
		       &frames->frame[nr].isactivation))
    return -1;

  frames->frames++;
  if (frames->frames == maxframes)
    return DWARF_CB_ABORT;

  if (frames->frames == frames->allocated)
    {
      frames->allocated *= 2;
      frames->frame = realloc (frames->frame,
			       sizeof (struct frame) * frames->allocated);
      if (frames->frame == NULL)
	error (EXIT_BAD, errno, "realloc frames.frame");
    }

  return DWARF_CB_OK;
}

static const char*
die_name (Dwarf_Die *die)
{
  Dwarf_Attribute attr;
  const char *name;
  name = dwarf_formstring (dwarf_attr_integrate (die,
						 DW_AT_MIPS_linkage_name,
						 &attr)
			   ?: dwarf_attr_integrate (die,
						    DW_AT_linkage_name,
						    &attr));
  if (name == NULL)
    name = dwarf_diename (die);

  return name;
}

static void
print_frame (int nr, Dwarf_Addr pc, bool isactivation,
	     Dwarf_Addr pc_adjusted, Dwfl_Module *mod,
	     const char *symname, Dwarf_Die *cudie,
	     Dwarf_Die *die)
{
  int width = get_addr_width (mod);
  printf ("#%-2u 0x%0*" PRIx64, nr, width, (uint64_t) pc);

  if (show_activation)
    printf ("%4s", ! isactivation ? "- 1" : "");

  if (symname != NULL)
    {
#ifdef USE_DEMANGLE
      // Require GNU v3 ABI by the "_Z" prefix.
      if (! show_raw && symname[0] == '_' && symname[1] == 'Z')
	{
	  int status = -1;
	  char *dsymname = __cxa_demangle (symname, demangle_buffer,
					   &demangle_buffer_len, &status);
	  if (status == 0)
	    symname = demangle_buffer = dsymname;
	}
#endif
      printf (" %s", symname);
    }

  const char* fname;
  Dwarf_Addr start;
  fname = dwfl_module_info(mod, NULL, &start,
			   NULL, NULL, NULL, NULL, NULL);
  if (show_module)
    {
      if (fname != NULL)
	printf (" - %s", fname);
    }

  if (show_build_id)
    {
      const unsigned char *id;
      GElf_Addr id_vaddr;
      int id_len = dwfl_module_build_id (mod, &id, &id_vaddr);
      if (id_len > 0)
	{
	  printf ("\n    [");
	  do
	    printf ("%02" PRIx8, *id++);
	  while (--id_len > 0);
	  printf ("]@0x%0" PRIx64 "+0x%" PRIx64,
		  start, pc_adjusted - start);
	}
    }

  if (show_source)
    {
      int line, col;
      const char* sname;
      line = col = -1;
      sname = NULL;
      if (die != NULL)
	{
	  Dwarf_Files *files;
	  if (dwarf_getsrcfiles (cudie, &files, NULL) == 0)
	    {
	      Dwarf_Attribute attr;
	      Dwarf_Word val;
	      if (dwarf_formudata (dwarf_attr (die, DW_AT_call_file, &attr),
				   &val) == 0)
		{
		  sname = dwarf_filesrc (files, val, NULL, NULL);
		  if (dwarf_formudata (dwarf_attr (die, DW_AT_call_line,
						   &attr), &val) == 0)
		    {
		      line = val;
		      if (dwarf_formudata (dwarf_attr (die, DW_AT_call_column,
						       &attr), &val) == 0)
			col = val;
		    }
		}
	    }
	}
      else
	{
	  Dwfl_Line *lineobj = dwfl_module_getsrc(mod, pc_adjusted);
	  if (lineobj)
	    sname = dwfl_lineinfo (lineobj, NULL, &line, &col, NULL, NULL);
	}

      if (sname != NULL)
	{
	  printf ("\n    %s", sname);
	  if (line > 0)
	    {
	      printf (":%d", line);
	      if (col > 0)
		printf (":%d", col);
	    }
	}
    }
  printf ("\n");
}

static void
print_inline_frames (int *nr, Dwarf_Addr pc, bool isactivation,
		     Dwarf_Addr pc_adjusted, Dwfl_Module *mod,
		     const char *symname, Dwarf_Die *cudie, Dwarf_Die *die)
{
  Dwarf_Die *scopes = NULL;
  int nscopes = dwarf_getscopes_die (die, &scopes);
  if (nscopes > 0)
    {
      /* scopes[0] == die, the lowest level, for which we already have
	 the name.  This is the actual source location where it
	 happened.  */
      print_frame ((*nr)++, pc, isactivation, pc_adjusted, mod, symname,
		   NULL, NULL);

      /* last_scope is the source location where the next frame/function
	 call was done. */
      Dwarf_Die *last_scope = &scopes[0];
      for (int i = 1; i < nscopes && (maxframes == 0 || *nr < maxframes); i++)
	{
	  Dwarf_Die *scope = &scopes[i];
	  int tag = dwarf_tag (scope);
	  if (tag != DW_TAG_inlined_subroutine
	      && tag != DW_TAG_entry_point
	      && tag != DW_TAG_subprogram)
	    continue;

	  symname = die_name (scope);
	  print_frame ((*nr)++, pc, isactivation, pc_adjusted, mod, symname,
		       cudie, last_scope);

	  /* Found the "top-level" in which everything was inlined?  */
	  if (tag == DW_TAG_subprogram)
	    break;

	  last_scope = scope;
	}
    }
  free (scopes);
}

static void
print_frames (struct frames *frames, pid_t tid, int dwflerr, const char *what)
{
  if (frames->frames > 0)
    frames_shown = true;

  printf ("TID %lld:\n", (long long) tid);
  int frame_nr = 0;
  for (int nr = 0; nr < frames->frames && (maxframes == 0
					   || frame_nr < maxframes); nr++)
    {
      Dwarf_Addr pc = frames->frame[nr].pc;
      bool isactivation = frames->frame[nr].isactivation;
      Dwarf_Addr pc_adjusted = pc - (isactivation ? 0 : 1);

      /* Get PC->SYMNAME.  */
      Dwfl_Module *mod = dwfl_addrmodule (dwfl, pc_adjusted);
      const char *symname = NULL;
      Dwarf_Die die_mem;
      Dwarf_Die *die = NULL;
      Dwarf_Die *cudie = NULL;
      if (mod && ! show_quiet)
	{
	  if (show_debugname)
	    {
	      Dwarf_Addr bias = 0;
	      Dwarf_Die *scopes = NULL;
	      cudie = dwfl_module_addrdie (mod, pc_adjusted, &bias);
	      int nscopes = dwarf_getscopes (cudie, pc_adjusted - bias,
					     &scopes);

	      /* Find the first function-like DIE with a name in scope.  */
	      for (int i = 0; symname == NULL && i < nscopes; i++)
		{
		  Dwarf_Die *scope = &scopes[i];
		  int tag = dwarf_tag (scope);
		  if (tag == DW_TAG_subprogram
		      || tag == DW_TAG_inlined_subroutine
		      || tag == DW_TAG_entry_point)
		    symname = die_name (scope);

		  if (symname != NULL)
		    {
		      die_mem = *scope;
		      die = &die_mem;
		    }
		}
	      free (scopes);
	    }

	  if (symname == NULL)
	    symname = dwfl_module_addrname (mod, pc_adjusted);
	}

      if (show_inlines && die != NULL)
	print_inline_frames (&frame_nr, pc, isactivation, pc_adjusted, mod,
			     symname, cudie, die);
      else
	print_frame (frame_nr++, pc, isactivation, pc_adjusted, mod, symname,
		     NULL, NULL);
    }

  if (frames->frames > 0 && frame_nr == maxframes)
    error (0, 0, "tid %lld: shown max number of frames "
	   "(%d, use -n 0 for unlimited)", (long long) tid, maxframes);
  else if (dwflerr != 0)
    {
      if (frames->frames > 0)
	{
	  unsigned nr = frames->frames - 1;
	  Dwarf_Addr pc = frames->frame[nr].pc;
	  bool isactivation = frames->frame[nr].isactivation;
	  Dwarf_Addr pc_adjusted = pc - (isactivation ? 0 : 1);
	  Dwfl_Module *mod = dwfl_addrmodule (dwfl, pc_adjusted);
	  const char *mainfile = NULL;
	  const char *modname = dwfl_module_info (mod, NULL, NULL, NULL, NULL,
						  NULL, &mainfile, NULL);
	  if (modname == NULL || modname[0] == '\0')
	    {
	      if (mainfile != NULL)
		modname = mainfile;
	      else
		modname = "<unknown>";
	    }
	  error (0, 0, "%s tid %lld at 0x%" PRIx64 " in %s: %s", what,
		 (long long) tid, pc_adjusted, modname, dwfl_errmsg (dwflerr));
	}
      else
	error (0, 0, "%s tid %lld: %s", what, (long long) tid,
	       dwfl_errmsg (dwflerr));
    }
}

static int
thread_callback (Dwfl_Thread *thread, void *thread_arg)
{
  struct frames *frames = (struct frames *) thread_arg;
  pid_t tid = dwfl_thread_tid (thread);
  int err = 0;
  frames->frames = 0;
  switch (dwfl_thread_getframes (thread, frame_callback, thread_arg))
    {
    case DWARF_CB_OK:
    case DWARF_CB_ABORT:
      break;
    case -1:
      err = dwfl_errno ();
      break;
    default:
      abort ();
    }
  print_frames (frames, tid, err, "dwfl_thread_getframes");
  return DWARF_CB_OK;
}

static error_t
parse_opt (int key, char *arg __attribute__ ((unused)),
	   struct argp_state *state)
{
  switch (key)
    {
    case 'p':
      pid = atoi (arg);
      if (pid == 0)
	argp_error (state, N_("-p PID should be a positive process id."));
      break;

    case OPT_COREFILE:
      core_fd = open (arg, O_RDONLY);
      if (core_fd < 0)
	error (EXIT_BAD, errno, N_("Cannot open core file '%s'"), arg);
      elf_version (EV_CURRENT);
      core = elf_begin (core_fd, ELF_C_READ_MMAP, NULL);
      if (core == NULL)
	error (EXIT_BAD, 0, "core '%s' elf_begin: %s", arg, elf_errmsg(-1));
      break;

    case 'e':
      exec = arg;
      break;

    case OPT_DEBUGINFO:
      debuginfo_path = arg;
      break;

    case 'm':
      show_module = true;
      break;

    case 's':
      show_source = true;
      break;

    case 'a':
      show_activation = true;
      break;

    case 'd':
      show_debugname = true;
      break;

    case 'i':
      show_inlines = show_debugname = true;
      break;

    case 'v':
      show_activation = show_source = show_module = show_debugname = true;
      show_inlines = true;
      break;

    case 'b':
      show_build_id = true;
      break;

    case 'q':
      show_quiet = true;
      break;

    case 'r':
      show_raw = true;
      break;

    case '1':
      show_one_tid = true;
      break;

    case 'n':
      maxframes = atoi (arg);
      if (maxframes < 0)
	{
	  argp_error (state, N_("-n MAXFRAMES should be 0 or higher."));
	  return EINVAL;
	}
      break;

    case 'l':
      show_modules = true;
      break;

    case ARGP_KEY_END:
      if (core == NULL && exec != NULL)
	argp_error (state,
		    N_("-e EXEC needs a core given by --core."));

      if (pid == 0 && show_one_tid == true)
	argp_error (state,
		    N_("-1 needs a thread id given by -p."));

      if ((pid == 0 && core == NULL) || (pid != 0 && core != NULL))
	argp_error (state,
		    N_("One of -p PID or --core COREFILE should be given."));

      if (pid != 0)
	{
	  dwfl = dwfl_begin (&proc_callbacks);
	  if (dwfl == NULL)
	    error (EXIT_BAD, 0, "dwfl_begin: %s", dwfl_errmsg (-1));

	  int err = dwfl_linux_proc_report (dwfl, pid);
	  if (err < 0)
	    error (EXIT_BAD, 0, "dwfl_linux_proc_report pid %lld: %s",
		   (long long) pid, dwfl_errmsg (-1));
	  else if (err > 0)
	    error (EXIT_BAD, err, "dwfl_linux_proc_report pid %lld",
		   (long long) pid);
	}

      if (core != NULL)
	{
	  dwfl = dwfl_begin (&core_callbacks);
	  if (dwfl == NULL)
	    error (EXIT_BAD, 0, "dwfl_begin: %s", dwfl_errmsg (-1));
	  if (dwfl_core_file_report (dwfl, core, exec) < 0)
	    error (EXIT_BAD, 0, "dwfl_core_file_report: %s", dwfl_errmsg (-1));
	}

      if (dwfl_report_end (dwfl, NULL, NULL) != 0)
	error (EXIT_BAD, 0, "dwfl_report_end: %s", dwfl_errmsg (-1));

      if (pid != 0)
	{
	  int err = dwfl_linux_proc_attach (dwfl, pid, false);
	  if (err < 0)
	    error (EXIT_BAD, 0, "dwfl_linux_proc_attach pid %lld: %s",
		   (long long) pid, dwfl_errmsg (-1));
	  else if (err > 0)
	    error (EXIT_BAD, err, "dwfl_linux_proc_attach pid %lld",
		   (long long) pid);
	}

      if (core != NULL)
	{
	  if (dwfl_core_file_attach (dwfl, core) < 0)
	    error (EXIT_BAD, 0, "dwfl_core_file_attach: %s", dwfl_errmsg (-1));
	}

      /* Makes sure we are properly attached.  */
      if (dwfl_pid (dwfl) < 0)
	error (EXIT_BAD, 0, "dwfl_pid: %s\n", dwfl_errmsg (-1));
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

int
main (int argc, char **argv)
{
  /* We use no threads here which can interfere with handling a stream.  */
  __fsetlocking (stdin, FSETLOCKING_BYCALLER);
  __fsetlocking (stdout, FSETLOCKING_BYCALLER);
  __fsetlocking (stderr, FSETLOCKING_BYCALLER);

  /* Set locale.  */
  (void) setlocale (LC_ALL, "");

  const struct argp_option options[] =
    {
      { NULL, 0, NULL, 0, N_("Input selection options:"), 0 },
      { "pid", 'p', "PID", 0,
	N_("Show stack of process PID"), 0 },
      { "core", OPT_COREFILE, "COREFILE", 0,
	N_("Show stack found in COREFILE"), 0 },
      {  "executable", 'e', "EXEC", 0, N_("(optional) EXECUTABLE that produced COREFILE"), 0 },
      { "debuginfo-path", OPT_DEBUGINFO, "PATH", 0,
	N_("Search path for separate debuginfo files"), 0 },

      { NULL, 0, NULL, 0, N_("Output selection options:"), 0 },
      { "activation",  'a', NULL, 0,
	N_("Additionally show frame activation"), 0 },
      { "debugname",  'd', NULL, 0,
	N_("Additionally try to lookup DWARF debuginfo name for frame address"),
	0 },
      { "inlines",  'i', NULL, 0,
	N_("Additionally show inlined function frames using DWARF debuginfo if available (implies -d)"), 0 },
      { "module",  'm', NULL, 0,
	N_("Additionally show module file information"), 0 },
      { "source",  's', NULL, 0,
	N_("Additionally show source file information"), 0 },
      { "verbose", 'v', NULL, 0,
	N_("Show all additional information (activation, debugname, inlines, module and source)"), 0 },
      { "quiet", 'q', NULL, 0,
	N_("Do not resolve address to function symbol name"), 0 },
      { "raw", 'r', NULL, 0,
	N_("Show raw function symbol names, do not try to demangle names"), 0 },
      { "build-id",  'b', NULL, 0,
	N_("Show module build-id, load address and pc offset"), 0 },
      { NULL, '1', NULL, 0,
	N_("Show the backtrace of only one thread"), 0 },
      { NULL, 'n', "MAXFRAMES", 0,
	N_("Show at most MAXFRAMES per thread (default 256, use 0 for unlimited)"), 0 },
      { "list-modules", 'l', NULL, 0,
	N_("Show module memory map with build-id, elf and debug files detected"), 0 },
      { NULL, 0, NULL, 0, NULL, 0 }
    };

  const struct argp argp =
    {
      .options = options,
      .parser = parse_opt,
      .doc = N_("Print a stack for each thread in a process or core file.\n\
\n\
Program exits with return code 0 if all frames were shown without \
any errors.  If some frames were shown, but there were some non-fatal \
errors, possibly causing an incomplete backtrace, the program exits \
with return code 1.  If no frames could be shown, or a fatal error \
occurred the program exits with return code 2.  If the program was \
invoked with bad or missing arguments it will exit with return code 64.")
    };

  argp_parse (&argp, argc, argv, 0, NULL, NULL);

  if (show_modules)
    {
      printf ("PID %lld - %s module memory map\n", (long long) dwfl_pid (dwfl),
	      pid != 0 ? "process" : "core");
      if (dwfl_getmodules (dwfl, module_callback, NULL, 0) != 0)
	error (EXIT_BAD, 0, "dwfl_getmodules: %s", dwfl_errmsg (-1));
    }

  struct frames frames;
  /* When maxframes is zero, then 2048 is just the initial allocation
     that will be increased using realloc in framecallback ().  */
  frames.allocated = maxframes == 0 ? 2048 : maxframes;
  frames.frames = 0;
  frames.frame = malloc (sizeof (struct frame) * frames.allocated);
  if (frames.frame == NULL)
    error (EXIT_BAD, errno, "malloc frames.frame");

  if (show_one_tid)
    {
      int err = 0;
      switch (dwfl_getthread_frames (dwfl, pid, frame_callback, &frames))
	{
	case DWARF_CB_OK:
	case DWARF_CB_ABORT:
	  break;
	case -1:
	  err = dwfl_errno ();
	  break;
	default:
	  abort ();
	}
      print_frames (&frames, pid, err, "dwfl_getthread_frames");
    }
  else
    {
      printf ("PID %lld - %s\n", (long long) dwfl_pid (dwfl),
	      pid != 0 ? "process" : "core");
      switch (dwfl_getthreads (dwfl, thread_callback, &frames))
	{
	case DWARF_CB_OK:
	case DWARF_CB_ABORT:
	  break;
	case -1:
	  error (0, 0, "dwfl_getthreads: %s", dwfl_errmsg (-1));
	  break;
	default:
	  abort ();
	}
    }
  free (frames.frame);
  dwfl_end (dwfl);

  if (core != NULL)
    elf_end (core);

  if (core_fd != -1)
    close (core_fd);

#ifdef USE_DEMANGLE
  free (demangle_buffer);
#endif

  if (! frames_shown)
    error (EXIT_BAD, 0, N_("Couldn't show any frames."));

  return error_message_count != 0 ? EXIT_ERROR : EXIT_OK;
}
