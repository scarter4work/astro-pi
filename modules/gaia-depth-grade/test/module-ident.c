/* module-ident: report a PixInsight module's identity exactly as the core's
 * module query does, without PixInsight. Test-only, never shipped.
 *
 * Protocol (PCL src/pcl/API.cpp, MetaModule.h InstallMode): dlopen the module,
 * InstallPixInsightModule(QueryModuleInfo) instantiates its MetaModule (no
 * processes, no core API calls), then IdentifyPixInsightModule phases 0, 1
 * and 255 hand out the api_module_description the core reads.
 *
 *   module-ident <module.so>  -> {"module":"<id>","version":"<MM.mm.rr.bbbb.LLL>","apiVersion":"0x0188"}
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

/* Layout of api_module_description, pcl/api/APIDefs.h (PCL 2.13.0). */
struct api_module_description
{
   const char*     uniqueId;
   const char*     versionInfo;
   const char*     name;
   const uint16_t* description;
   const uint16_t* company;
   const uint16_t* author;
   const uint16_t* copyright;
   const uint16_t* tradeMarks;
   const uint16_t* originalFileName;
   struct { uint16_t year; uint8_t month; uint8_t day; } releaseDate;
   uint32_t        apiVersion;
};

typedef int      (*install_fn)( int mode );
typedef uint32_t (*identify_fn)( struct api_module_description** d, int32_t phase );

int main( int argc, char** argv )
{
   if ( argc != 2 ) { fprintf( stderr, "usage: %s <module.so>\n", argv[0] ); return 2; }
   void* h = dlopen( argv[1], RTLD_NOW | RTLD_LOCAL );
   if ( !h ) { fprintf( stderr, "dlopen: %s\n", dlerror() ); return 1; }
   install_fn install = (install_fn)dlsym( h, "InstallPixInsightModule" );
   identify_fn identify = (identify_fn)dlsym( h, "IdentifyPixInsightModule" );
   if ( !install || !identify ) { fprintf( stderr, "missing PixInsight entry points\n" ); return 1; }
   if ( install( 1 /* InstallMode::QueryModuleInfo */ ) != 0 ) { fprintf( stderr, "install(QueryModuleInfo) failed\n" ); return 1; }
   struct api_module_description* d = NULL;
   if ( identify( &d, 0 ) != 0 || identify( &d, 1 ) != 0 || d == NULL || d->name == NULL )
   {
      fprintf( stderr, "identification failed\n" );
      return 1;
   }
   printf( "{\"module\":\"%s\",\"version\":\"%s\",\"apiVersion\":\"0x%04X\",\"releaseDate\":\"%04u-%02u-%02u\"}\n",
           d->name, d->versionInfo ? d->versionInfo : "", (unsigned)d->apiVersion,
           (unsigned)d->releaseDate.year, (unsigned)d->releaseDate.month, (unsigned)d->releaseDate.day );
   identify( &d, 255 );
   return 0;
}
