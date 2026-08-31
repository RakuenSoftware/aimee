/* economizer.h -- the C transport facade for the Go economizer.
 *
 * Context reduction, folding, condensation policy and the proof planner all live
 * in server-go/modules/economizer now and are reached over the event bus;
 * economizer_module_client.h is the whole call surface.
 *
 * Nothing exposed here owns reduction or compaction policy.
 */
#ifndef DEC_ECONOMIZER_H
#define DEC_ECONOMIZER_H 1

#include "economizer_module_client.h"

#endif /* DEC_ECONOMIZER_H */
