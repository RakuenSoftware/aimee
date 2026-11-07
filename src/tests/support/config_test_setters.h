/* Config setters tests use to arrange state. No shipped caller sets these
 * keys natively, so the product no longer generates setters; each writes the
 * same key config_accessor_keys.def names, through the config client. */
#ifndef AIMEE_TEST_CONFIG_SETTERS_H
#define AIMEE_TEST_CONFIG_SETTERS_H 1

#include <stddef.h>

int config_set_calibration_buckets(int value);
int config_set_calibration_conformal_window(int value);
int config_set_calibration_enabled(int value);
int config_set_calibration_model_version(const char *value);
int config_set_calibration_prompt_version(const char *value);
int config_set_calibration_tau_memory_auto(double value);
int config_set_calibration_tau_memory_flag(double value);
int config_set_calibration_tau_working_profile_auto(double value);
int config_set_calibration_tau_working_profile_flag(double value);
int config_set_delegate_sandbox_image(const char *value);
int config_set_ensemble_aggregator(const char *value);
int config_set_kb_curator_extract_docs_enabled(int value);
int config_set_kb_mode(const char *value);
int config_set_kb_pdf_assets_enabled(int value);
int config_set_kb_pdf_ocr_enabled(int value);
int config_set_memory_recall_enabled(int value);
int config_set_provider(const char *value);
int config_set_reasoning_datalog_command(const char *value);
int config_set_require_session_worktree(int value);
int config_set_server_api_mtls(int value);
int config_set_workspace_count(int value);

#endif
