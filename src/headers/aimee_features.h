/* aimee_features.h: Compile-time module selection shared by all build systems. */
#ifndef AIMEE_FEATURES_H
#define AIMEE_FEATURES_H 1

/* Optional and default-disabled. Build with 1 to include it. */
#ifndef AIMEE_WITH_PLUGIN_LOADER
#define AIMEE_WITH_PLUGIN_LOADER 0
#endif

/* Optional, but selected in normal builds for backward compatibility. */
#ifndef AIMEE_WITH_ROUNDTABLE
#define AIMEE_WITH_ROUNDTABLE 1
#endif

#endif /* AIMEE_FEATURES_H */
