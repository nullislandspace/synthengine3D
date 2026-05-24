#include "se_version.h"

#define SE__STR2(x) #x
#define SE__STR(x)  SE__STR2(x)

char const* se_version_string(void) {
    return SE__STR(SE_VERSION_MAJOR) "."
           SE__STR(SE_VERSION_MINOR) "."
           SE__STR(SE_VERSION_PATCH);
}
