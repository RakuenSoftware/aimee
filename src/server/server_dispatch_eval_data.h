/* server_dispatch_eval_data.h: the eval.* dispatch rows.
 *
 * Same device as server/cli_dispatch_defs_data.h on the client side: kept as an
 * include rather than inlined so the owning translation unit stays under the
 * source line-count limit. Included inside server_dispatch_table[]'s
 * initializer; the row type lives with the includer.
 *
 * The family is the unit here — run/results and the regression-candidate
 * surface move together, so an eval change lands in one place.
 */
{"eval.run", handle_eval_run}, {"eval.results", handle_eval_results},
    {"eval.candidates", handle_eval_candidates},
    {"eval.candidates-update", handle_eval_candidates_update},
