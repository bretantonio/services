project('services', 'c',
  version : '1.0.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
  ]
)

glib_dep    = dependency('glib-2.0',    version : '>= 2.56')
gio_dep     = dependency('gio-2.0',     version : '>= 2.56')
gobject_dep = dependency('gobject-2.0', version : '>= 2.56')

# ncurses — prefer the ncursesw (wide-char) variant; fall back to ncurses
ncurses_dep = dependency('ncursesw', required : false)
if not ncurses_dep.found()
  ncurses_dep = dependency('ncurses')
endif

# terminfo is sometimes split into its own lib (tinfo)
tinfo_dep = dependency('tinfow', required : false)

deps = [
  glib_dep,
  gio_dep,
  gobject_dep,
  ncurses_dep,
]

if tinfo_dep.found()
  deps += tinfo_dep
endif

executable('batteryd',
  sources      : ['batteryd.c'],
  dependencies : deps,
  install      : true,
)
