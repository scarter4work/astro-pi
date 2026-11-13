#ifndef AutoContrastVersion_h
#define AutoContrastVersion_h

#define AC_MODULE_VERSION_MAJOR     0
#define AC_MODULE_VERSION_MINOR     1
#define AC_MODULE_VERSION_RELEASE   0
#define AC_MODULE_VERSION_BUILD     0

#define AC_STR_(x)  #x
#define AC_STR(x)   AC_STR_(x)

#define AC_VERSION_STRING \
    AC_STR(AC_MODULE_VERSION_MAJOR) "." \
    AC_STR(AC_MODULE_VERSION_MINOR) "." \
    AC_STR(AC_MODULE_VERSION_RELEASE) "." \
    AC_STR(AC_MODULE_VERSION_BUILD)

#endif
