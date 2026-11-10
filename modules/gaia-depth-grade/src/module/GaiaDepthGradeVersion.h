#ifndef GaiaDepthGradeVersion_h
#define GaiaDepthGradeVersion_h

#define GDG_MODULE_VERSION_MAJOR     1
#define GDG_MODULE_VERSION_MINOR     0
#define GDG_MODULE_VERSION_REVISION  0
#define GDG_MODULE_VERSION_BUILD     0

#define GDG_STR_(x)  #x
#define GDG_STR(x)   GDG_STR_(x)

#define GDG_VERSION_STRING \
    GDG_STR(GDG_MODULE_VERSION_MAJOR) "." \
    GDG_STR(GDG_MODULE_VERSION_MINOR) "." \
    GDG_STR(GDG_MODULE_VERSION_REVISION) "." \
    GDG_STR(GDG_MODULE_VERSION_BUILD)

#endif
