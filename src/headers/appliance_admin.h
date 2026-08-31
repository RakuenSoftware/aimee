#ifndef AIMEE_APPLIANCE_ADMIN_H
#define AIMEE_APPLIANCE_ADMIN_H 1

#include <stddef.h>

/* Resolve the interactive appliance administrator recorded by runtime-web.
 * The generated first-boot login owns the appliance until setup replaces it;
 * afterwards bootstrap-replaced names the administrator. */
void appliance_admin_webuser(char *out, size_t out_n);

/* True only for the attested webuser:<name> principal named by the record above. */
int appliance_admin_principal_authorized(const char *principal);

#endif /* AIMEE_APPLIANCE_ADMIN_H */
